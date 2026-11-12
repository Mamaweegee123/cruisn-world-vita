// Host-side (no Vita/vitaGL needed) unit test for the Cruis'n Vita RDP
// decoder. Builds a synthetic-but-real RDP command stream by hand (using
// the exact bit layouts cruisn_rdp_decoder.cpp implements) and verifies
// decode counts against a RecordingBackend, per this pass's Phase 8
// requirement: "decoder does not crash; expected triangle/texture-load/
// rectangle counts; expected state transitions; unsupported-command count."
//
// Build (no CMake needed -- pure portable C++, two translation units):
//   clang++ -std=c++17 -I runtime/vendor/N64ModernRuntime/ultramodern/include \
//       vita/renderer/cruisn_rdp_decoder.cpp vita/tests/cruisn_rdp_decoder_test.cpp \
//       -o /tmp/cruisn_rdp_decoder_test && /tmp/cruisn_rdp_decoder_test

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../renderer/cruisn_rdp_decoder.h"
#include "cruisn_rdp_recording_backend.h"

using cruisn::vita_rdp::DecodeStats;
using cruisn::vita_rdp::decodeRdpRange;
using cruisn::vita_rdp::test::RecordingBackend;

namespace {

int g_failures = 0;

void require(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::fprintf(stderr, "ok:   %s\n", what);
    }
}

void push(std::vector<uint32_t>& words, uint32_t w0, uint32_t w1) {
    words.push_back(w0);
    words.push_back(w1);
}

}  // namespace

int main() {
    std::vector<uint32_t> words;

    // 1. SetColorImage: fmt=RGBA(0) siz=16bit(2) width=320 addr=0x00100000
    push(words, 0x3F000000u | (0u << 21) | (2u << 19) | (319u), 0x00100000u);
    // 2. SetScissor: ulx=0 uly=0 lrx=320*4 lry=240*4 (10.2 fixed)
    push(words, 0x2D000000u, (uint32_t(320 * 4) << 12) | uint32_t(240 * 4));
    // 3. SetOtherMode: cycle_type=1cycle(0), non-perspective (high bit 0x80000 clear)
    push(words, 0x2F000000u, 0x00000000u);
    // 4. SetCombine: arbitrary mux (0x3C123456 already has opcode 0x3C as its top byte)
    push(words, 0x3C123456u, 0x9ABCDEF0u);
    // 5. SetTile: tile=0 fmt=RGBA(0) siz=16bit(2) line=5 tmem=0
    push(words, 0x35000000u | (0u << 21) | (2u << 19) | (5u << 9) | 0u, 0u);
    // 6. SetTileSize: tile=0 uls=0 ult=0 lrs=32*4 lrt=32*4
    push(words, 0x32000000u, (uint32_t(0) << 24) | (uint32_t(32 * 4) << 12) | uint32_t(32 * 4));
    // 7. LoadTile: tile=0, same extents
    push(words, 0x34000000u, (uint32_t(0) << 24) | (uint32_t(32 * 4) << 12) | uint32_t(32 * 4));

    // 8. One shaded+textured triangle (commandId 0x0E = shade|tex, no zbuf).
    //    yh=0 ym=200(fixed, =50px*4) yl=400(fixed,=100px*4); all x/dxdy = 0.
    push(words, 0x0E000000u | 400u, (200u << 16) | 0u);  // word0: w0=opcode|tile|level|yl, w1=ym<<16|yh
    push(words, 0u, 0u);                                  // word1: xl, dxldy
    push(words, 0u, 0u);                                  // word2: xh, dxhdy
    push(words, 0u, 0u);                                  // word3: xm, dxmdy
    for (int i = 0; i < 8; ++i) push(words, 0u, 0u);       // shade payload (base/dx/de/dy, all zero)
    for (int i = 0; i < 8; ++i) push(words, 0u, 0u);       // tex payload (base/dx/de/dy, all zero)

    // 9. TexRect (LLE, 2 words): rect (0,0)-(100,50) px, tile=0, uls/ult/dsdx/dtdy=0.
    push(words, 0x24000000u | (uint32_t(100 * 4) << 12) | uint32_t(50 * 4), 0u);
    push(words, 0u, 0u);

    // 10. FillRect: rect (0,0)-(50,50) px.
    push(words, 0x36000000u | (uint32_t(50 * 4) << 12) | uint32_t(50 * 4), 0u);

    // 11. FullSync.
    push(words, 0x29000000u, 0u);

    // 12. One deliberately-unsupported opcode (0x15 is not assigned to
    //     anything in the decoder's switch, and isn't in the triangle range).
    push(words, 0x15000000u, 0u);

    RecordingBackend backend;
    DecodeStats stats = decodeRdpRange(words.data(), words.size(), backend);

    std::fprintf(stderr, "\n-- decode stats --\n");
    std::fprintf(stderr, "commandsTotal=%u triangles=%u rects=%u texLoads=%u stateChanges=%u fullSyncs=%u unsupported=%u\n",
                 stats.commandsTotal, stats.trianglesDecoded, stats.rectanglesDecoded,
                 stats.textureLoadsDecoded, stats.stateChanges, stats.fullSyncsDecoded, stats.unsupportedOpcodes);

    require(stats.trianglesDecoded == 1, "exactly one triangle decoded");
    require(stats.rectanglesDecoded == 2, "exactly two rectangles decoded (texrect + fillrect)");
    require(stats.textureLoadsDecoded == 1, "exactly one texture load decoded (LoadTile)");
    require(stats.fullSyncsDecoded == 1, "exactly one FullSync decoded");
    require(stats.unsupportedOpcodes == 1, "exactly one unsupported opcode counted");
    require(stats.stateChanges >= 5, "at least 5 state-change commands counted (combine/tile/tilesize/othermode/scissor)");

    require(backend.colorImageCalls == 1, "backend saw 1 setColorImage call");
    require(backend.lastColorImage.width == 320, "decoded color image width == 320");
    require(backend.lastColorImage.address == 0x00100000u, "decoded color image address matches");
    require(backend.scissorCalls == 1, "backend saw 1 setScissor call");
    require(backend.lastScissor[2] == 320 * 4, "decoded scissor lrx matches (raw 10.2 fixed)");
    require(backend.tileCalls == 1, "backend saw 1 setTile call");
    require(backend.tileSizeCalls == 1, "backend saw 1 setTileSize call");
    require(backend.loadTileCalls == 1, "backend saw 1 loadTile call");
    require(backend.combineCalls == 1, "backend saw 1 setCombine call");
    require(backend.otherModeCalls == 1, "backend saw 1 setOtherMode call");
    require(backend.drawTrisCalls == 1, "backend saw 1 drawTris call");
    require(backend.triangleCount == 1, "backend recorded exactly 1 triangle");
    require(backend.lastTriPos[1] == 0.0f, "decoded triangle v0.y == yh (0)");
    require(backend.lastTriPos[5] == 100.0f, "decoded triangle v1.y == yl (100px)");
    require(backend.lastTriPos[9] == 50.0f, "decoded triangle v2.y == ym (50px)");
    require(backend.texRectCalls == 1, "backend saw 1 drawTexRect call");
    require(backend.fillRectCalls == 1, "backend saw 1 fillRect call");
    require(backend.fullSyncCalls == 1, "backend saw 1 fullSync call");
    require(backend.unsupportedCalls == 1, "backend saw 1 onUnsupported call");

    // Truncated-input robustness: a command stream that ends mid-command
    // must not crash, and must be counted as unsupported rather than read
    // out of bounds.
    {
        std::vector<uint32_t> truncated = {0x3C000000u};  // SetCombine header with no payload word
        RecordingBackend backend2;
        DecodeStats stats2 = decodeRdpRange(truncated.data(), truncated.size(), backend2);
        require(stats2.commandsTotal == 0, "truncated single-word input decodes zero complete commands");
        (void)stats2;
    }
    {
        // A triangle header claiming shade+tex+depth but with no payload at all.
        std::vector<uint32_t> truncatedTri = {0x0F000000u, 0u};
        RecordingBackend backend3;
        decodeRdpRange(truncatedTri.data(), truncatedTri.size(), backend3);
        require(backend3.drawTrisCalls == 0, "truncated triangle does not call drawTris");
        require(backend3.unsupportedCalls == 1, "truncated triangle is reported via onUnsupported");
    }

    std::fprintf(stderr, "\n%s: %d failing check(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
