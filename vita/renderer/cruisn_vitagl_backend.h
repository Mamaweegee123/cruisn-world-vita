// Cruis'n World Xtreme -- Vita-only vitaGL submission backend.
//
// The ONLY file in vita/renderer/ that includes vitaGL/psp2 headers (see
// cruisn_rdp_state.h's file comment -- everything else must stay host-
// testable). Implements IRdpBackend using vitaGL's classic fixed-function
// pipeline (client vertex arrays + glDrawArrays and glTexEnvi GL_MODULATE),
// not GLSL ES shaders: this is a deliberate "start simple"
// choice (this pass's Phase 4 instruction) -- it needs no shader
// compilation step (nothing to get wrong that can't be caught until
// hardware/emulator testing), at the cost of not implementing the real N64
// color-combiner formula. See docs/research/VITA_RDP_COMMAND_COVERAGE.md
// for exactly what this trades away.
//
// RDP triangle/rect coordinates arriving here are already in final guest
// screen-pixel space (Cruis'n's raw-RDP decoder -- see
// cruisn_rdp_decoder.cpp -- reproduces exactly what desktop's
// decodeTriangles() produces, which is screen-space X/Y, not a 3D vertex
// needing an MVP transform). So the "renderer" here is just an orthographic
// pixel-to-NDC mapping (glOrtho), not a 3D pipeline.

#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cruisn_rdp_state.h"

namespace cruisn::vita_rdp {

class CruisnVitaGLBackend : public IRdpBackend {
public:
    // Must be called once before any decode/draw calls -- gives the backend
    // read access to guest RDRAM for texture loads (LoadTile/LoadBlock/
    // LoadTLUT). Uses the same rdram[(addr) ^ 3]-style byte addressing as
    // the rest of this runtime (see MEM_B in recomp.h) so decoded TMEM bytes
    // come out in normal (non-byte-lane-swapped) order.
    void setRdram(const uint8_t* rdram) { rdram_ = rdram; }

    // Initializes vitaGL and the fixed-function GL state used throughout
    // (blend/texture-env/ortho projection). Call once at startup, after
    // vglInit*(). guestWidth/guestHeight are the logical N64 raster size
    // (e.g. 320x240); displayWidth/displayHeight are the real Vita output
    // (960x544). Computes an aspect-correct, centered viewport -- Phase 6:
    // "prefer aspect-correct centered rendering" for first boot, no
    // widescreen stretching yet.
    void initGL(int guestWidth, int guestHeight, int displayWidth, int displayHeight);

    void setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) override;
    void setDepthImage(uint32_t address) override;
    void setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) override;
    void setCombine(uint64_t combine) override;
    void setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem, uint8_t palette,
                uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks, uint8_t shiftt, uint8_t shifts) override;
    void setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) override;
    void loadTile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) override;
    void loadBlock(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt) override;
    void loadTLUT(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) override;
    void setEnvColor(uint32_t color) override;
    void setPrimColor(uint8_t lodFrac, uint8_t lodMin, uint32_t color) override;
    void setBlendColor(uint32_t color) override;
    void setFogColor(uint32_t color) override;
    void setFillColor(uint32_t color) override;
    void setOtherMode(uint32_t high, uint32_t low) override;
    void setPrimDepth(uint16_t z, uint16_t dz) override;
    void setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) override;
    void setConvert(int32_t k0, int32_t k1, int32_t k2, int32_t k3, int32_t k4, int32_t k5) override;
    void setKeyR(uint32_t centerR, uint32_t scaleR, uint32_t widthR) override;
    void setKeyGB(uint32_t centerG, uint32_t scaleG, uint32_t widthG, uint32_t centerB, uint32_t scaleB,
                 uint32_t widthB) override;
    void drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                     int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) override;
    void fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) override;
    void drawTris(uint32_t count, const float* pos, const float* tc, const float* col, uint8_t tile,
                 uint8_t level) override;
    void fullSync() override;
    void onUnsupported(RdpEvent::Kind kind, uint8_t opcode, const char* detail) override;

private:
    struct TileCache {
        bool dirty = true;
        unsigned int glTexture = 0;  // GLuint, avoiding a vitaGL.h include in this header
        int width = 0, height = 0;
    };

    // Decodes the given tile's TMEM region into an RGBA8888 buffer and
    // uploads it as the current GL texture, using a small cache keyed by
    // tile index (invalidated on SetTile/SetTileSize/any Load*). Formats
    // covered: RGBA16, RGBA32, IA16, IA8, I8, I4, CI8+TLUT, CI4+TLUT (this
    // pass's Phase 5 investigation list). Anything else: logs once, binds a
    // 1x1 white texture so drawing continues without crashing.
    void bindTileTexture(uint8_t tileIndex);
    void decodeTileRGBA8(uint8_t tileIndex, std::vector<uint8_t>& outRgba8, int& outW, int& outH);

    void applyScissorAndViewport();
    // ulx/uly/lrx/lry here are ALREADY pixel-space floats (caller divides
    // the raw 10.2 fixed values by 4 first).
    void submitTexturedQuad(float ulx, float uly, float lrx, float lry, uint8_t tile, float uls, float ult,
                            float dsdx, float dtdy, bool flip);
    void submitFlatQuad(float ulx, float uly, float lrx, float lry, uint32_t rgba8888, bool traceFirstDraw);

    const uint8_t* rdram_ = nullptr;
    uint8_t tmem_[4096] = {};
    uint16_t tlut_[256] = {};  // RGBA16-packed palette, index 0..255 (siz=8bit) or 0..15 (siz=4bit)

    Tile tiles_[kTileCount];
    ColorImage colorImage_;
    uint32_t depthImageAddress_ = 0;
    struct {
        uint8_t fmt = 0, siz = 0;
        uint16_t width = 0;
        uint32_t address = 0;
    } textureImage_;
    uint64_t combine_ = 0;
    OtherMode otherMode_;
    uint32_t envColor_ = 0, primColor_ = 0, blendColor_ = 0, fogColor_ = 0, fillColor_ = 0;
    uint8_t scissorMode_ = 0;
    int32_t scissorUlx_ = 0, scissorUly_ = 0, scissorLrx_ = 0, scissorLry_ = 0;

    TileCache tileCache_[kTileCount];

    int guestWidth_ = 320, guestHeight_ = 240;
    int viewportX_ = 0, viewportY_ = 0, viewportW_ = 960, viewportH_ = 544;

    std::set<uint64_t> loggedCombines_;
    std::set<uint32_t> loggedUnsupportedFormats_;
    std::set<std::string> loggedUnsupportedEvents_;
    bool loggedFirstColorImage_ = false, loggedFirstTextureImage_ = false, loggedFirstTmemLoad_ = false;
    bool loggedFirstTexRect_ = false, loggedFirstFillRect_ = false, loggedFirstFullSync_ = false;
    bool loggedFirstTriangle_ = false;
    bool loggedFirstTexture_ = false;
    bool loggedFirstTexturedTriangle_ = false;
    bool loggedFirstPresent_ = false;
};

}  // namespace cruisn::vita_rdp
