#include "cruisn_vitagl_backend.h"
#include <ultramodern/vita_memory.h>
#include "../src/vita_boot_log.h"
#include "../../runtime/src/vita_pacing.h"

#include <cstdio>
#include <cstring>

#include <vitaGL.h>

namespace cruisn::vita_rdp {
namespace {

// Same byte-addressing convention as recomp.h's MEM_B macro: guest RDRAM is
// stored with a per-byte lane swap so 32-bit word reads come out host-
// native. Reading through this helper instead of a raw memcpy normalizes
// bytes back into plain big-endian guest order inside tmem_/decode buffers.
inline uint8_t guestByte(const uint8_t* rdram, uint32_t physAddr) {
#if defined(__vita__)
    (void)rdram;
    return *static_cast<const uint8_t*>(vita_memory_resolve(physAddr ^ 3u, 1, VITA_MEMORY_READ));
#else
    return rdram[(physAddr & 0x1FFFFFFFu) ^ 3];
#endif
}

inline void unpackRgba16(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    auto expand5 = [](uint32_t c) -> uint8_t { return uint8_t((c << 3) | (c >> 2)); };
    r = expand5((v >> 11) & 0x1F);
    g = expand5((v >> 6) & 0x1F);
    b = expand5((v >> 1) & 0x1F);
    a = (v & 1) ? 255 : 0;
}

}  // namespace

void CruisnVitaGLBackend::initGL(int guestWidth, int guestHeight, int displayWidth, int displayHeight) {
    guestWidth_ = guestWidth;
    guestHeight_ = guestHeight;

    // Aspect-correct, centered viewport (Phase 6: no widescreen stretch on
    // first boot). If Cruis'n's own widescreen projection patches already
    // ran guest-side (see docs/research/VITA_PORT_ARCHITECTURE.md's
    // widescreen-carries-over analysis), this viewport can later be widened
    // to match -- not done in this pass.
    const float guestAspect = float(guestWidth_) / float(guestHeight_);
    const float displayAspect = float(displayWidth) / float(displayHeight);
    if (displayAspect > guestAspect) {
        viewportH_ = displayHeight;
        viewportW_ = int(displayHeight * guestAspect);
    } else {
        viewportW_ = displayWidth;
        viewportH_ = int(displayWidth / guestAspect);
    }
    viewportX_ = (displayWidth - viewportW_) / 2;
    viewportY_ = (displayHeight - viewportH_) / 2;

    glViewport(viewportX_, viewportY_, viewportW_, viewportH_);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // top/bottom swapped: guest Y grows downward, matching RDP raster space.
    glOrthof(0.0f, float(guestWidth_), float(guestHeight_), 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    vita_boot_log("[VITA-GFX] renderer backend ready guest=%dx%d display=%dx%d", guestWidth_, guestHeight_, displayWidth, displayHeight);
}

void CruisnVitaGLBackend::setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
    colorImage_ = {fmt, siz, width, address};
    if (!loggedFirstColorImage_) { loggedFirstColorImage_ = true; vita_boot_log("[VITA-RDP] first color image"); }
}

void CruisnVitaGLBackend::setDepthImage(uint32_t address) { depthImageAddress_ = address; }

void CruisnVitaGLBackend::setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
    textureImage_ = {fmt, siz, width, address};
    if (!loggedFirstTextureImage_) { loggedFirstTextureImage_ = true; vita_boot_log("[VITA-RDP] first texture image"); }
}

void CruisnVitaGLBackend::setCombine(uint64_t combine) {
    combine_ = combine;
    if (loggedCombines_.insert(combine).second && loggedCombines_.size() <= 64) {
        vita_boot_log("[VITA-RDP-WARN] unsupported combine=0x%016llX", (unsigned long long)combine);
    }
}

void CruisnVitaGLBackend::setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem,
                                  uint8_t palette, uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks,
                                  uint8_t shiftt, uint8_t shifts) {
    if (tile >= kTileCount) return;
    tiles_[tile].fmt = fmt;
    tiles_[tile].siz = siz;
    tiles_[tile].line = line;
    tiles_[tile].tmem = tmem;
    tiles_[tile].palette = palette;
    tiles_[tile].cmt = cmt;
    tiles_[tile].cms = cms;
    tiles_[tile].maskt = maskt;
    tiles_[tile].masks = masks;
    tiles_[tile].shiftt = shiftt;
    tiles_[tile].shifts = shifts;
    tileCache_[tile].dirty = true;
}

void CruisnVitaGLBackend::setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile >= kTileCount) return;
    tiles_[tile].uls = uls;
    tiles_[tile].ult = ult;
    tiles_[tile].lrs = lrs;
    tiles_[tile].lrt = lrt;
    tileCache_[tile].dirty = true;
}

// Simplified TMEM load: straight row-major byte copy from the currently
// bound texture image into TMEM at the tile's configured location, using
// the guest byte-lane-normalizing read helper above. Does NOT reproduce
// real hardware's odd-row 64-bit bank swizzle (see
// docs/research/VITA_RDP_COMMAND_COVERAGE.md) -- documented, not silently
// assumed correct.
void CruisnVitaGLBackend::loadTile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (!loggedFirstTmemLoad_) { loggedFirstTmemLoad_ = true; vita_boot_log("[VITA-RDP] first TMEM load"); }
    if (tile >= kTileCount || rdram_ == nullptr) return;
    const Tile& t = tiles_[tile];
    const int bytesPerTexel = (t.siz == 3) ? 4 : (t.siz == 2) ? 2 : 1;  // siz 0/1 (4/8bit) handled at decode time
    const int x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    const int w = (x1 - x0 + 1);
    const int h = (y1 - y0 + 1);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return;

    const uint32_t srcRowStride = uint32_t(textureImage_.width) * bytesPerTexel;
    const uint32_t dstRowStride = uint32_t(t.line) * 8;
    const uint32_t dstBase = uint32_t(t.tmem) * 8;

    for (int row = 0; row < h; ++row) {
        const uint32_t srcRowAddr = textureImage_.address + (y0 + row) * srcRowStride + x0 * bytesPerTexel;
        const uint32_t dstRowOffset = dstBase + row * dstRowStride;
        const int rowBytes = w * bytesPerTexel;
        if (dstRowOffset + rowBytes > sizeof(tmem_)) break;
        for (int b = 0; b < rowBytes; ++b) {
            tmem_[dstRowOffset + b] = guestByte(rdram_, srcRowAddr + b);
        }
    }
    tileCache_[tile].dirty = true;
}

void CruisnVitaGLBackend::loadBlock(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt) {
    if (!loggedFirstTmemLoad_) { loggedFirstTmemLoad_ = true; vita_boot_log("[VITA-RDP] first TMEM load"); }
    // LoadBlock's source is a linear run, not a 2D rectangle; approximate it
    // as a single-row loadTile covering [uls, lrs] words -- adequate for
    // the "doesn't crash, produces a plausible texture" bar this pass sets,
    // not hardware-accurate. dxt (the per-row TMEM-address increment used
    // by real hardware for multi-row blocks) is intentionally unused here.
    (void)dxt;
    if (tile >= kTileCount || rdram_ == nullptr) return;
    const Tile& t = tiles_[tile];
    const uint32_t dstBase = uint32_t(t.tmem) * 8;
    const uint32_t words = (uint32_t(lrs) - uint32_t(uls) + 1);
    const uint32_t bytes = words * 8;  // LoadBlock counts in 64-bit words regardless of texel size
    if (dstBase + bytes > sizeof(tmem_)) return;
    const uint32_t srcAddr = textureImage_.address + uint32_t(ult) * 8;
    for (uint32_t b = 0; b < bytes; ++b) {
        tmem_[dstBase + b] = guestByte(rdram_, srcAddr + b);
    }
    tileCache_[tile].dirty = true;
}

void CruisnVitaGLBackend::loadTLUT(uint8_t tile, uint16_t uls, uint16_t, uint16_t lrs, uint16_t) {
    if (!loggedFirstTmemLoad_) { loggedFirstTmemLoad_ = true; vita_boot_log("[VITA-RDP] first TMEM load"); }
    if (rdram_ == nullptr) return;
    (void)tile;
    const int startIndex = uls >> 2;
    const int endIndex = lrs >> 2;
    for (int idx = startIndex; idx <= endIndex && idx < 256; ++idx) {
        const uint32_t srcAddr = textureImage_.address + uint32_t(idx - startIndex) * 2;
        const uint8_t hi = guestByte(rdram_, srcAddr);
        const uint8_t lo = guestByte(rdram_, srcAddr + 1);
        tlut_[idx] = uint16_t((hi << 8) | lo);
    }
    for (auto& cache : tileCache_) cache.dirty = true;
}

void CruisnVitaGLBackend::setEnvColor(uint32_t color) { envColor_ = color; }
void CruisnVitaGLBackend::setPrimColor(uint8_t, uint8_t, uint32_t color) { primColor_ = color; }
void CruisnVitaGLBackend::setBlendColor(uint32_t color) { blendColor_ = color; }
void CruisnVitaGLBackend::setFogColor(uint32_t color) { fogColor_ = color; }
void CruisnVitaGLBackend::setFillColor(uint32_t color) { fillColor_ = color; }

void CruisnVitaGLBackend::setOtherMode(uint32_t high, uint32_t low) {
    otherMode_.high = high;
    otherMode_.low = low;
    static std::set<uint64_t> loggedOtherModes;
    const uint64_t value = (uint64_t(high) << 32) | low;
    if (loggedOtherModes.insert(value).second && loggedOtherModes.size() <= 64) {
        vita_boot_log("[VITA-RDP-WARN] unsupported otherMode=0x%06X%08X", high, low);
    }
}

void CruisnVitaGLBackend::setPrimDepth(uint16_t, uint16_t) {}

void CruisnVitaGLBackend::setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    scissorMode_ = mode;
    scissorUlx_ = ulx;
    scissorUly_ = uly;
    scissorLrx_ = lrx;
    scissorLry_ = lry;
    applyScissorAndViewport();
}

void CruisnVitaGLBackend::setConvert(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {}
void CruisnVitaGLBackend::setKeyR(uint32_t, uint32_t, uint32_t) {}
void CruisnVitaGLBackend::setKeyGB(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

void CruisnVitaGLBackend::applyScissorAndViewport() {
    // Raw scissor fields are 10.2 fixed point pixels in guest raster space;
    // map to the centered Vita viewport established in initGL().
    const float sx0 = scissorUlx_ / 4.0f, sy0 = scissorUly_ / 4.0f;
    const float sx1 = scissorLrx_ / 4.0f, sy1 = scissorLry_ / 4.0f;
    const float scaleX = float(viewportW_) / float(guestWidth_);
    const float scaleY = float(viewportH_) / float(guestHeight_);
    const int gx = viewportX_ + int(sx0 * scaleX);
    const int gy = viewportY_ + int((float(guestHeight_) - sy1) * scaleY);  // GL scissor origin is bottom-left
    const int gw = int((sx1 - sx0) * scaleX);
    const int gh = int((sy1 - sy0) * scaleY);
    glEnable(GL_SCISSOR_TEST);
    if (gw > 0 && gh > 0) {
        glScissor(gx, gy, gw, gh);
    }
}

void CruisnVitaGLBackend::decodeTileRGBA8(uint8_t tileIndex, std::vector<uint8_t>& out, int& outW, int& outH) {
    const Tile& t = tiles_[tileIndex];
    const int w = int((t.lrs >> 2) - (t.uls >> 2)) + 1;
    const int h = int((t.lrt >> 2) - (t.ult >> 2)) + 1;
    outW = (w > 0 && w <= 256) ? w : 1;
    outH = (h > 0 && h <= 256) ? h : 1;
    out.assign(size_t(outW) * outH * 4, 255);

    const uint32_t base = uint32_t(t.tmem) * 8;
    const uint32_t rowStride = uint32_t(t.line) * 8;

    const uint32_t formatKey = (uint32_t(t.fmt) << 4) | t.siz;
    bool handled = true;
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            uint8_t r = 255, g = 255, b = 255, a = 255;
            switch (formatKey) {
                case (0u << 4) | 2u: {  // RGBA, 16-bit (RGBA5551)
                    uint32_t off = base + y * rowStride + x * 2;
                    if (off + 1 >= sizeof(tmem_)) break;
                    uint16_t v = uint16_t((tmem_[off] << 8) | tmem_[off + 1]);
                    unpackRgba16(v, r, g, b, a);
                    break;
                }
                case (0u << 4) | 3u: {  // RGBA, 32-bit (simplified, no bank split -- see header comment)
                    uint32_t off = base + y * rowStride + x * 4;
                    if (off + 3 >= sizeof(tmem_)) break;
                    r = tmem_[off];
                    g = tmem_[off + 1];
                    b = tmem_[off + 2];
                    a = tmem_[off + 3];
                    break;
                }
                case (3u << 4) | 2u: {  // IA16
                    uint32_t off = base + y * rowStride + x * 2;
                    if (off + 1 >= sizeof(tmem_)) break;
                    r = g = b = tmem_[off];
                    a = tmem_[off + 1];
                    break;
                }
                case (3u << 4) | 1u: {  // IA8 (4-bit I + 4-bit A)
                    uint32_t off = base + y * rowStride + x;
                    if (off >= sizeof(tmem_)) break;
                    uint8_t v = tmem_[off];
                    uint8_t i4 = (v >> 4) & 0xF, a4 = v & 0xF;
                    r = g = b = uint8_t((i4 << 4) | i4);
                    a = uint8_t((a4 << 4) | a4);
                    break;
                }
                case (4u << 4) | 1u: {  // I8
                    uint32_t off = base + y * rowStride + x;
                    if (off >= sizeof(tmem_)) break;
                    r = g = b = a = tmem_[off];
                    break;
                }
                case (4u << 4) | 0u: {  // I4
                    uint32_t off = base + y * rowStride + (x >> 1);
                    if (off >= sizeof(tmem_)) break;
                    uint8_t byte = tmem_[off];
                    uint8_t nib = (x & 1) ? (byte & 0xF) : (byte >> 4);
                    r = g = b = a = uint8_t((nib << 4) | nib);
                    break;
                }
                case (2u << 4) | 1u: {  // CI8 + TLUT
                    uint32_t off = base + y * rowStride + x;
                    if (off >= sizeof(tmem_)) break;
                    unpackRgba16(tlut_[tmem_[off]], r, g, b, a);
                    break;
                }
                case (2u << 4) | 0u: {  // CI4 + TLUT
                    uint32_t off = base + y * rowStride + (x >> 1);
                    if (off >= sizeof(tmem_)) break;
                    uint8_t byte = tmem_[off];
                    uint8_t nib = (x & 1) ? (byte & 0xF) : (byte >> 4);
                    unpackRgba16(tlut_[(t.palette << 4) | nib], r, g, b, a);
                    break;
                }
                default:
                    handled = false;
                    break;
            }
            size_t o = (size_t(y) * outW + x) * 4;
            out[o] = r;
            out[o + 1] = g;
            out[o + 2] = b;
            out[o + 3] = a;
        }
    }

    if (!handled && loggedUnsupportedFormats_.insert(formatKey).second) {
        std::fprintf(stderr, "[VITA-RDP] unsupported texture format fmt=%u siz=%u (using flat white fallback)\n",
                     t.fmt, t.siz);
    }
}

void CruisnVitaGLBackend::bindTileTexture(uint8_t tileIndex) {
    if (tileIndex >= kTileCount) return;
    TileCache& cache = tileCache_[tileIndex];
    if (cache.dirty) {
        std::vector<uint8_t> rgba8;
        int w = 0, h = 0;
        decodeTileRGBA8(tileIndex, rgba8, w, h);
        if (cache.glTexture == 0) {
            GLuint id = 0;
            glGenTextures(1, &id);
            cache.glTexture = id;
        }
        glBindTexture(GL_TEXTURE_2D, cache.glTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8.data());
        cache.width = w;
        cache.height = h;
        cache.dirty = false;

        if (!loggedFirstTexture_) {
            loggedFirstTexture_ = true;
            std::fprintf(stderr, "[VITA-RDP] first texture (tile=%u %dx%d fmt=%u siz=%u)\n", tileIndex, w, h,
                        tiles_[tileIndex].fmt, tiles_[tileIndex].siz);
        }
    } else {
        glBindTexture(GL_TEXTURE_2D, cache.glTexture);
    }
}

void CruisnVitaGLBackend::submitTexturedQuad(float ulx, float uly, float lrx, float lry, uint8_t tile, float uls,
                                             float ult, float dsdx, float dtdy, bool flip) {
    glEnable(GL_TEXTURE_2D);
    bindTileTexture(tile);
    const TileCache& cache = tileCache_[tile];
    const float tw = cache.width > 0 ? float(cache.width) : 1.0f;
    const float th = cache.height > 0 ? float(cache.height) : 1.0f;
    const float w = lrx - ulx, h = lry - uly;
    const float s0 = uls / 32.0f / tw, t0 = ult / 32.0f / th;  // texcoords are s10.5 fixed point
    const float s1 = s0 + (flip ? (h * dtdy / 1024.0f) : (w * dsdx / 1024.0f)) / tw;
    const float t1 = t0 + (flip ? (w * dsdx / 1024.0f) : (h * dtdy / 1024.0f)) / th;

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(s0, t0);
    glVertex2f(ulx, uly);
    glTexCoord2f(flip ? s0 : s1, flip ? t1 : t0);
    glVertex2f(lrx, uly);
    glTexCoord2f(s1, t1);
    glVertex2f(lrx, lry);

    glTexCoord2f(s0, t0);
    glVertex2f(ulx, uly);
    glTexCoord2f(s1, t1);
    glVertex2f(lrx, lry);
    glTexCoord2f(flip ? s1 : s0, flip ? t0 : t1);
    glVertex2f(ulx, lry);
    glEnd();
}

void CruisnVitaGLBackend::submitFlatQuad(float ulx, float uly, float lrx, float lry, uint32_t rgba8888,
                                         bool traceFirstDraw) {
    glDisable(GL_TEXTURE_2D);
    const float r = ((rgba8888 >> 24) & 0xFF) / 255.0f;
    const float g = ((rgba8888 >> 16) & 0xFF) / 255.0f;
    const float b = ((rgba8888 >> 8) & 0xFF) / 255.0f;
    const float a = (rgba8888 & 0xFF) / 255.0f;

    // Keep this array explicit rather than going through vitaGL's legacy
    // glBegin/glEnd immediate-mode pool. The order is two clockwise screen
    // space triangles: TL/TR/BR then TL/BR/BL.
    const GLfloat positions[6][3] = {
        {ulx, uly, 0.0f}, {lrx, uly, 0.0f}, {lrx, lry, 0.0f},
        {ulx, uly, 0.0f}, {lrx, lry, 0.0f}, {ulx, lry, 0.0f},
    };
    const GLfloat colors[6][4] = {
        {r, g, b, a}, {r, g, b, a}, {r, g, b, a},
        {r, g, b, a}, {r, g, b, a}, {r, g, b, a},
    };

    if (traceFirstDraw) vita_boot_log("[VITA-DRAW] fillrect vertices ready");
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    static bool loggedArrayBufferUnbound = false;
    if (!loggedArrayBufferUnbound) {
        loggedArrayBufferUnbound = true;
        vita_boot_log("[VITA-GFXAB] fillrect_array_buffer_unbound");
    }
    glVertexPointer(3, GL_FLOAT, 0, positions);
    glColorPointer(4, GL_FLOAT, 0, colors);
    if (traceFirstDraw) vita_boot_log("[VITA-DRAW] fillrect glDrawArrays begin");
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (traceFirstDraw) {
        vita_boot_log("[VITA-DRAW] fillrect glDrawArrays end");
        vita_boot_log("[VITA-DRAW] glError=0x%04lX", static_cast<unsigned long>(glGetError()));
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_TEXTURE_2D);
}

void CruisnVitaGLBackend::drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile,
                                      int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    if (!loggedFirstTexRect_) { loggedFirstTexRect_ = true; vita_boot_log("[VITA-RDP] first texrect"); }
    vita_boot_note_texrect();
    submitTexturedQuad(ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f, tile, float(uls), float(ult),
                       float(dsdx), float(dtdy), flip);
}

void CruisnVitaGLBackend::fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    const bool traceFirstDraw = !loggedFirstFillRect_;
    if (traceFirstDraw) {
        loggedFirstFillRect_ = true;
        vita_boot_log("[VITA-RDP] first fillrect");
        vita_boot_log("[VITA-DRAW] fillrect begin");
    }
    submitFlatQuad(ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f, fillColor_, traceFirstDraw);
    if (traceFirstDraw) vita_boot_log("[VITA-RDP] fillrect complete");
}

void CruisnVitaGLBackend::drawTris(uint32_t count, const float* pos, const float* tc, const float* col,
                                   uint8_t tile, uint8_t) {
    vita_boot_note_triangle(count);
    if (!loggedFirstTriangle_) {
        loggedFirstTriangle_ = true;
        vita_boot_log("[VITA-RDP] first triangle");
    }

    // Cycle type COPY/FILL never reach here via real Cruis'n traffic in
    // practice (those use fillRect/texrect), but guard anyway: 1-cycle/
    // 2-cycle both use this same triangle path.
    const bool hasTexture = tiles_[tile].fmt != 0 || tileCache_[tile].glTexture != 0;
    // Heuristic, not real combiner decode (see setCombine's log-once
    // above): if this tile has ever been loaded, assume the triangle wants
    // it textured; otherwise flat-shade from vertex color.
    if (hasTexture && tileCache_[tile].glTexture != 0) {
        if (!loggedFirstTexturedTriangle_) {
            loggedFirstTexturedTriangle_ = true;
            vita_boot_log("[VITA-RDP] first textured triangle");
        }
        glEnable(GL_TEXTURE_2D);
        bindTileTexture(tile);
        const TileCache& cache = tileCache_[tile];
        const float tw = cache.width > 0 ? float(cache.width) : 1.0f;
        const float th = cache.height > 0 ? float(cache.height) : 1.0f;
        glBegin(GL_TRIANGLES);
        for (uint32_t i = 0; i < count * 3; ++i) {
            glColor4f(col[i * 4 + 0], col[i * 4 + 1], col[i * 4 + 2], col[i * 4 + 3]);
            glTexCoord2f((tc[i * 2 + 0] / 1024.0f) / tw, (tc[i * 2 + 1] / 1024.0f) / th);
            glVertex2f(pos[i * 4 + 0], pos[i * 4 + 1]);
        }
        glEnd();
    } else {
        glDisable(GL_TEXTURE_2D);
        glBegin(GL_TRIANGLES);
        for (uint32_t i = 0; i < count * 3; ++i) {
            glColor4f(col[i * 4 + 0], col[i * 4 + 1], col[i * 4 + 2], col[i * 4 + 3]);
            glVertex2f(pos[i * 4 + 0], pos[i * 4 + 1]);
        }
        glEnd();
        glEnable(GL_TEXTURE_2D);
    }
}

void CruisnVitaGLBackend::fullSync() {
    if (!loggedFirstFullSync_) { loggedFirstFullSync_ = true; vita_boot_log("[VITA-RDP] first fullsync"); }
    vita_boot_note_present();
    vita_gfxpace_swap_begin();
    vita_pace_present();
    vita_latergfx_present();
    const bool firstPresent = !loggedFirstPresent_;
    if (firstPresent) vita_boot_log("[VITA-GFX] first present begin");
    vglSwapBuffers(GL_FALSE);
    vita_gfxpace_swap_end();
    if (firstPresent) {
        loggedFirstPresent_ = true;
        vita_boot_log("[VITA-GFX] first present end");
        vita_boot_log("[VITA-GFX] first present");
    }
    glClear(GL_COLOR_BUFFER_BIT);
}

void CruisnVitaGLBackend::onUnsupported(RdpEvent::Kind, uint8_t opcode, const char* detail) {
    const std::string event = std::to_string(opcode) + ":" + (detail ? detail : "(unknown)");
    if (loggedUnsupportedEvents_.insert(event).second) vita_boot_log("[VITA-RDP-WARN] unsupported opcode=0x%02X", opcode);
}

}  // namespace cruisn::vita_rdp
