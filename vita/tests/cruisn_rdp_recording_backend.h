// Host-testable IRdpBackend implementation: records call counts/last-seen
// values instead of touching any GPU API. Lets cruisn_rdp_decoder.cpp be
// unit-tested on a plain host compiler (see docs/research/
// VITA_RDP_COMMAND_COVERAGE.md and this pass's Phase 8 instruction).
#pragma once

#include <cstdio>
#include <set>
#include <string>

#include "../renderer/cruisn_rdp_state.h"

namespace cruisn::vita_rdp::test {

class RecordingBackend : public IRdpBackend {
public:
    int colorImageCalls = 0, depthImageCalls = 0, textureImageCalls = 0;
    int combineCalls = 0, tileCalls = 0, tileSizeCalls = 0;
    int loadTileCalls = 0, loadBlockCalls = 0, loadTlutCalls = 0;
    int envColorCalls = 0, primColorCalls = 0, blendColorCalls = 0, fogColorCalls = 0, fillColorCalls = 0;
    int otherModeCalls = 0, primDepthCalls = 0, scissorCalls = 0, convertCalls = 0;
    int keyRCalls = 0, keyGBCalls = 0;
    int texRectCalls = 0, fillRectCalls = 0, drawTrisCalls = 0, triangleCount = 0, fullSyncCalls = 0;
    int unsupportedCalls = 0;
    std::set<uint64_t> uniqueCombines;

    ColorImage lastColorImage{};
    Tile lastTile[kTileCount]{};
    uint8_t lastScissorMode = 0;
    int32_t lastScissor[4] = {0, 0, 0, 0};
    float lastTriPos[12] = {};
    float lastTriTc[6] = {};
    float lastTriCol[12] = {};
    uint8_t lastTriTile = 0;

    void setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) override {
        ++colorImageCalls;
        lastColorImage = {fmt, siz, width, address};
    }
    void setDepthImage(uint32_t) override { ++depthImageCalls; }
    void setTextureImage(uint8_t, uint8_t, uint16_t, uint32_t) override { ++textureImageCalls; }
    void setCombine(uint64_t combine) override {
        ++combineCalls;
        uniqueCombines.insert(combine);
    }
    void setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem, uint8_t palette,
                uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks, uint8_t shiftt, uint8_t shifts) override {
        ++tileCalls;
        if (tile < kTileCount) {
            lastTile[tile] = {fmt, siz, line, tmem, palette, cmt, cms, maskt, masks, shiftt, shifts, 0, 0, 0, 0};
        }
    }
    void setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) override {
        ++tileSizeCalls;
        if (tile < kTileCount) {
            lastTile[tile].uls = uls;
            lastTile[tile].ult = ult;
            lastTile[tile].lrs = lrs;
            lastTile[tile].lrt = lrt;
        }
    }
    void loadTile(uint8_t, uint16_t, uint16_t, uint16_t, uint16_t) override { ++loadTileCalls; }
    void loadBlock(uint8_t, uint16_t, uint16_t, uint16_t, uint16_t) override { ++loadBlockCalls; }
    void loadTLUT(uint8_t, uint16_t, uint16_t, uint16_t, uint16_t) override { ++loadTlutCalls; }
    void setEnvColor(uint32_t) override { ++envColorCalls; }
    void setPrimColor(uint8_t, uint8_t, uint32_t) override { ++primColorCalls; }
    void setBlendColor(uint32_t) override { ++blendColorCalls; }
    void setFogColor(uint32_t) override { ++fogColorCalls; }
    void setFillColor(uint32_t) override { ++fillColorCalls; }
    void setOtherMode(uint32_t, uint32_t) override { ++otherModeCalls; }
    void setPrimDepth(uint16_t, uint16_t) override { ++primDepthCalls; }
    void setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) override {
        ++scissorCalls;
        lastScissorMode = mode;
        lastScissor[0] = ulx;
        lastScissor[1] = uly;
        lastScissor[2] = lrx;
        lastScissor[3] = lry;
    }
    void setConvert(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) override { ++convertCalls; }
    void setKeyR(uint32_t, uint32_t, uint32_t) override { ++keyRCalls; }
    void setKeyGB(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { ++keyGBCalls; }
    void drawTexRect(int32_t, int32_t, int32_t, int32_t, uint8_t, int16_t, int16_t, int16_t, int16_t, bool) override {
        ++texRectCalls;
    }
    void fillRect(int32_t, int32_t, int32_t, int32_t) override { ++fillRectCalls; }
    void drawTris(uint32_t count, const float* pos, const float* tc, const float* col, uint8_t tile,
                 uint8_t) override {
        ++drawTrisCalls;
        triangleCount += int(count);
        lastTriTile = tile;
        // count is triangles, not vertices: 3 vertices/triangle, 4 floats/vertex
        // for pos+col (12/tri) and 2 floats/vertex for tc (6/tri).
        for (int i = 0; i < 12 && i < int(count) * 12; ++i) lastTriPos[i] = pos[i];
        for (int i = 0; i < 6 && i < int(count) * 6; ++i) lastTriTc[i] = tc[i];
        for (int i = 0; i < 12 && i < int(count) * 12; ++i) lastTriCol[i] = col[i];
    }
    void fullSync() override { ++fullSyncCalls; }
    void onUnsupported(RdpEvent::Kind, uint8_t opcode, const char* detail) override {
        ++unsupportedCalls;
        std::fprintf(stderr, "[TEST] onUnsupported: opcode=0x%02X %s\n", opcode, detail ? detail : "(null)");
    }
};

}  // namespace cruisn::vita_rdp::test
