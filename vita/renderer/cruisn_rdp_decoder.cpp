#include "cruisn_rdp_decoder.h"

#include <cmath>
#include <cstring>

#include "ultramodern/rdp_stream.hpp"  // command_words() -- pure header, reused verbatim (see .h comment)

namespace cruisn::vita_rdp {
namespace {

// Bit extraction helpers matching rt64_gbi_rdp.cpp's DisplayList::p0/p1
// (p0 reads from the command's first word, p1 from its second).
inline uint32_t bits(uint32_t word, int bit, int width) {
    return (word >> bit) & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u));
}

// RDP hardware opcodes (6-bit, matches command word's top byte & 0x3F).
// Exact values cross-checked against runtime/vendor/rt64/src/shared/
// rt64_f3d_defines.h's G_* macros (their GBI form, masked & 0x3F for LLE use
// -- e.g. G_SETCIMG=0xff -> 0xff & 0x3f = 0x3f) and against
// ultramodern::rdp::command_words()'s own opcode ranges.
enum Opcode : uint32_t {
    kTriBase = 0x08,     // 0x08..0x0F, low 3 bits = depth/tex/shade flags
    kTexRect = 0x24,
    kTexRectFlip = 0x25,
    kLoadSync = 0x26,
    kPipeSync = 0x27,
    kTileSync = 0x28,
    kFullSync = 0x29,
    kSetKeyGB = 0x2A,
    kSetKeyR = 0x2B,
    kSetConvert = 0x2C,
    kSetScissor = 0x2D,
    kSetPrimDepth = 0x2E,
    kSetOtherMode = 0x2F,
    kLoadTLUT = 0x30,
    kSetTileSize = 0x32,
    kLoadBlock = 0x33,
    kLoadTile = 0x34,
    kSetTile = 0x35,
    kFillRect = 0x36,
    kSetFillColor = 0x37,
    kSetFogColor = 0x38,
    kSetBlendColor = 0x39,
    kSetPrimColor = 0x3A,
    kSetEnvColor = 0x3B,
    kSetCombine = 0x3C,
    kSetTexImg = 0x3D,
    kSetZImg = 0x3E,
    kSetColorImg = 0x3F,
};

struct Cmd {
    uint32_t w0, w1;
};

// Decodes exactly one triangle (the same "TODO do more than 1 tri at a time"
// limitation desktop's decodeTriangles() currently has -- ported as-is, not
// "improved", per this pass's Phase 3 instruction to match existing stock
// semantics exactly). cmd[0] is the base 4-word triangle header; shade/tex/
// depth payload follow per the shaded/textured/hasDepth flags, exactly
// mirroring rt64_gbi_rdp.cpp's decodeTriangles().
void decodeTriangle(const Cmd* cmd, size_t available, uint32_t commandId, IRdpBackend& backend,
                     const OtherMode& otherMode) {
    const bool hasDepth = (commandId & 1) != 0;
    const bool textured = (commandId & 2) != 0;
    const bool shaded = (commandId & 4) != 0;
    const uint32_t tile = (cmd[0].w0 >> 16) & 0x7;
    const uint32_t level = (cmd[0].w0 >> 19) & 0x7;

    size_t need = 4;
    if (shaded) need += 8;
    if (textured) need += 8;
    if (hasDepth) need += 2;
    if (available < need) {
        backend.onUnsupported(RdpEvent::Kind::Triangle, uint8_t((cmd[0].w0 >> 24) & 0x3F), "truncated triangle command");
        return;
    }

    int32_t ylFixed = int16_t(cmd[0].w0 & 0xFFFF);
    int32_t ymFixed = int16_t(cmd[0].w1 >> 16);
    int32_t yhFixed = int16_t(cmd[0].w1 & 0xFFFF);
    int32_t xlFixed = int32_t(cmd[1].w0);
    int32_t dxldyFixed = int32_t(cmd[1].w1);
    int32_t xhFixed = int32_t(cmd[2].w0);
    int32_t dxhdyFixed = int32_t(cmd[2].w1);
    int32_t xmFixed = int32_t(cmd[3].w0);
    int32_t dxmdyFixed = int32_t(cmd[3].w1);

    float yl = ylFixed / 4.0f, ym = ymFixed / 4.0f, yh = yhFixed / 4.0f;
    float y1 = yh, y2 = yl;
    float xl = xlFixed / 65536.0f, xm = xmFixed / 65536.0f, xh = xhFixed / 65536.0f;
    float dxldy = dxldyFixed / 65536.0f, dxmdy = dxmdyFixed / 65536.0f, dxhdy = dxhdyFixed / 65536.0f;
    float yFloor = std::floor(yh);
    float hIntercept = xh - dxhdy * yFloor;
    float x1 = dxhdy * y1 + hIntercept;
    float x2 = dxhdy * y2 + hIntercept;
    float x3 = xl, y3 = ym;
    (void)xm;
    (void)dxldy;

    float pos[3][4] = {{x1, y1, 0, 1}, {x2, y2, 0, 1}, {x3, y3, 0, 1}};
    float tc[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    float col[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    float dy1 = y1 - yFloor, dy2 = y2 - yFloor, dy3 = y3 - yFloor;
    float x3Opposite = dxhdy * y3 + hIntercept;
    float dx3 = x3 - x3Opposite;

    size_t idx = 4;
    if (shaded) {
        const Cmd* s = cmd + idx;
        // 4 color lanes (R,G,B,A), each: base(w0hi<<16|w2hi ... ) packed the
        // same way as rt64_gbi_rdp.cpp's baseColorFixed/colorDxFixed/colorDeFixed.
        auto lane = [&](int hiWordIdx, int loWordIdx, bool hi) -> int32_t {
            uint32_t a = hi ? (s[hiWordIdx].w0 >> 16) : (s[hiWordIdx].w0 & 0xFFFF);
            uint32_t b = hi ? (s[loWordIdx].w0 >> 16) : (s[loWordIdx].w0 & 0xFFFF);
            return int32_t((a << 16) | b);
        };
        auto laneAlpha = [&](int hiWordIdx, int loWordIdx, bool hi) -> int32_t {
            uint32_t a = hi ? (s[hiWordIdx].w1 >> 16) : (s[hiWordIdx].w1 & 0xFFFF);
            uint32_t b = hi ? (s[loWordIdx].w1 >> 16) : (s[loWordIdx].w1 & 0xFFFF);
            return int32_t((a << 16) | b);
        };
        float base[4], dcde[4];
        base[0] = lane(0, 2, true) / 65536.0f;
        base[1] = lane(0, 2, false) / 65536.0f;
        base[2] = laneAlpha(0, 2, true) / 65536.0f;
        base[3] = laneAlpha(0, 2, false) / 65536.0f;
        dcde[0] = lane(4, 6, true) / 65536.0f;
        dcde[1] = lane(4, 6, false) / 65536.0f;
        dcde[2] = laneAlpha(4, 6, true) / 65536.0f;
        dcde[3] = laneAlpha(4, 6, false) / 65536.0f;
        float dcdx[4];
        dcdx[0] = lane(1, 3, true) / 65536.0f;
        dcdx[1] = lane(1, 3, false) / 65536.0f;
        dcdx[2] = laneAlpha(1, 3, true) / 65536.0f;
        dcdx[3] = laneAlpha(1, 3, false) / 65536.0f;

        for (int c = 0; c < 4; ++c) {
            float v1 = base[c] + dcde[c] * dy1;
            float v2 = base[c] + dcde[c] * dy2;
            float v3opp = base[c] + dcde[c] * dy3;
            float v3 = v3opp + dcdx[c] * dx3;
            col[0][c] = v1 / 255.0f;
            col[1][c] = v2 / 255.0f;
            col[2][c] = v3 / 255.0f;
        }
        idx += 8;
    }

    if (textured) {
        const Cmd* s = cmd + idx;
        auto lane = [&](int hiWordIdx, int loWordIdx, bool hi, bool useW1) -> int32_t {
            uint32_t hiw = useW1 ? s[hiWordIdx].w1 : s[hiWordIdx].w0;
            uint32_t low = useW1 ? s[loWordIdx].w1 : s[loWordIdx].w0;
            uint32_t a = hi ? (hiw >> 16) : (hiw & 0xFFFF);
            uint32_t b = hi ? (low >> 16) : (low & 0xFFFF);
            return int32_t((a << 16) | b);
        };
        // S, T, W lanes: S/T use w0 of each pair, W uses w1 (matches
        // baseTexcoordFixed's third component reading .w1).
        float baseS = lane(0, 2, true, false) / 65536.0f;
        float baseT = lane(0, 2, false, false) / 65536.0f;
        float baseW = lane(0, 2, true, true) / 65536.0f;
        float deS = lane(4, 6, true, false) / 65536.0f;
        float deT = lane(4, 6, false, false) / 65536.0f;
        float deW = lane(4, 6, true, true) / 65536.0f;
        float dxS = lane(1, 3, true, false) / 65536.0f;
        float dxT = lane(1, 3, false, false) / 65536.0f;
        float dxW = lane(1, 3, true, true) / 65536.0f;

        float w1v = baseW + deW * dy1;
        float w2v = baseW + deW * dy2;
        float w3opp = baseW + deW * dy3;
        float w3v = w3opp + dxW * dx3;

        float s1 = baseS + deS * dy1, t1 = baseT + deT * dy1;
        float s2 = baseS + deS * dy2, t2 = baseT + deT * dy2;
        float s3opp = baseS + deS * dy3, t3opp = baseT + deT * dy3;
        float s3 = s3opp + dxS * dx3, t3 = t3opp + dxT * dx3;

        if (otherMode.texturePersp()) {
            pos[0][3] = 65536000.0f / w1v;
            pos[1][3] = 65536000.0f / w2v;
            pos[2][3] = 65536000.0f / w3v;
            tc[0][0] = (s1 / w1v) * 1024.0f;
            tc[0][1] = (t1 / w1v) * 1024.0f;
            tc[1][0] = (s2 / w2v) * 1024.0f;
            tc[1][1] = (t2 / w2v) * 1024.0f;
            tc[2][0] = (s3 / w3v) * 1024.0f;
            tc[2][1] = (t3 / w3v) * 1024.0f;
        } else {
            pos[0][3] = pos[1][3] = pos[2][3] = 1.0f;
            tc[0][0] = (s1 * 1024.0f) / 16384.0f;
            tc[0][1] = (t1 * 1024.0f) / 16384.0f;
            tc[1][0] = (s2 * 1024.0f) / 16384.0f;
            tc[1][1] = (t2 * 1024.0f) / 16384.0f;
            tc[2][0] = (s3 * 1024.0f) / 16384.0f;
            tc[2][1] = (t3 * 1024.0f) / 16384.0f;
        }
        idx += 8;
    } else {
        pos[0][3] = pos[1][3] = pos[2][3] = 1.0f;
    }

    if (hasDepth) {
        const Cmd* s = cmd + idx;
        float baseDepth = float(int32_t(s[0].w0)) * (1.0f / 65536.0f / 32768.0f);
        float depthDe = float(int32_t(s[1].w0)) * (1.0f / 65536.0f / 32768.0f);
        float depthDx = float(int32_t(s[0].w1)) * (1.0f / 65536.0f / 32768.0f);
        float v1 = baseDepth + depthDe * dy1;
        float v2 = baseDepth + depthDe * dy2;
        float v3opp = baseDepth + depthDe * dy3;
        float v3 = v3opp + depthDx * dx3;
        pos[0][2] = v1;
        pos[1][2] = v2;
        pos[2][2] = v3;
        idx += 2;
    }

    float flatPos[12], flatTc[6], flatCol[12];
    std::memcpy(flatPos, pos, sizeof(flatPos));
    std::memcpy(flatTc, tc, sizeof(flatTc));
    std::memcpy(flatCol, col, sizeof(flatCol));
    backend.drawTris(1, flatPos, flatTc, flatCol, uint8_t(tile), uint8_t(level));
}

}  // namespace

const char* decodeStopReasonName(DecodeStopReason reason) {
    switch (reason) {
        case DecodeStopReason::RangeEnd: return "range_end";
        case DecodeStopReason::TruncatedCommand: return "truncated_command";
        case DecodeStopReason::TruncatedTriangle: return "truncated_triangle";
    }
    return "unknown";
}

DecodeStats decodeRdpRange(const uint32_t* words, size_t wordCount, IRdpBackend& backend) {
    DecodeStats stats{};
    const Cmd* cmds = reinterpret_cast<const Cmd*>(words);
    size_t cmdCount = wordCount / 2;
    OtherMode currentOtherMode{};

    size_t i = 0;
    while (i < cmdCount) {
        const Cmd& c = cmds[i];
        const uint32_t opcode = (c.w0 >> 24) & 0x3F;
        // command_words() counts 64-bit words, i.e. Cmd entries 1:1 -- no conversion needed.
        const size_t cmdsThisCmd = ultramodern::rdp::command_words(uint8_t(opcode));
        ++stats.commandsTotal;

        if (opcode >= kTriBase && opcode <= kTriBase + 7) {
            ++stats.trianglesDecoded;
            if (cmdCount - i < cmdsThisCmd) {
                backend.onUnsupported(RdpEvent::Kind::Triangle, uint8_t(opcode), "truncated triangle command");
                stats.stopReason = DecodeStopReason::TruncatedTriangle;
                break;
            }
            decodeTriangle(&c, cmdCount - i, opcode, backend, currentOtherMode);
            i += cmdsThisCmd;
            continue;
        }

        if (i + cmdsThisCmd > cmdCount) {
            backend.onUnsupported(RdpEvent::Kind::Unknown, uint8_t(opcode), "truncated command at end of range");
            stats.stopReason = DecodeStopReason::TruncatedCommand;
            break;
        }

        switch (opcode) {
            case kSetColorImg:
                backend.setColorImage(uint8_t(bits(c.w0, 21, 3)), uint8_t(bits(c.w0, 19, 2)),
                                      uint16_t(bits(c.w0, 0, 12) + 1), c.w1);
                break;
            case kSetZImg:
                backend.setDepthImage(c.w1);
                break;
            case kSetTexImg:
                backend.setTextureImage(uint8_t(bits(c.w0, 21, 3)), uint8_t(bits(c.w0, 19, 2)),
                                        uint16_t(bits(c.w0, 0, 12) + 1), c.w1);
                break;
            case kSetCombine:
                backend.setCombine((uint64_t(c.w1) << 32) | uint64_t(c.w0));
                ++stats.stateChanges;
                break;
            case kSetTile:
                backend.setTile(uint8_t(bits(c.w1, 24, 3)), uint8_t(bits(c.w0, 21, 3)),
                                uint8_t(bits(c.w0, 19, 2)), uint16_t(bits(c.w0, 9, 9)),
                                uint16_t(bits(c.w0, 0, 9)), uint8_t(bits(c.w1, 20, 4)),
                                uint8_t(bits(c.w1, 18, 2)), uint8_t(bits(c.w1, 8, 2)),
                                uint8_t(bits(c.w1, 14, 4)), uint8_t(bits(c.w1, 4, 4)),
                                uint8_t(bits(c.w1, 10, 4)), uint8_t(bits(c.w1, 0, 4)));
                ++stats.stateChanges;
                break;
            case kSetTileSize:
                backend.setTileSize(uint8_t(bits(c.w1, 24, 3)), uint16_t(bits(c.w0, 12, 12)),
                                    uint16_t(bits(c.w0, 0, 12)), uint16_t(bits(c.w1, 12, 12)),
                                    uint16_t(bits(c.w1, 0, 12)));
                ++stats.stateChanges;
                break;
            case kLoadTile:
                backend.loadTile(uint8_t(bits(c.w1, 24, 3)), uint16_t(bits(c.w0, 12, 12)),
                                 uint16_t(bits(c.w0, 0, 12)), uint16_t(bits(c.w1, 12, 12)),
                                 uint16_t(bits(c.w1, 0, 12)));
                ++stats.textureLoadsDecoded;
                break;
            case kLoadBlock:
                backend.loadBlock(uint8_t(bits(c.w1, 24, 3)), uint16_t(bits(c.w0, 12, 12)),
                                  uint16_t(bits(c.w0, 0, 12)), uint16_t(bits(c.w1, 12, 12)),
                                  uint16_t(bits(c.w1, 0, 12)));
                ++stats.textureLoadsDecoded;
                break;
            case kLoadTLUT:
                backend.loadTLUT(uint8_t(bits(c.w1, 24, 3)), uint16_t(bits(c.w0, 12, 12)),
                                 uint16_t(bits(c.w0, 0, 12)), uint16_t(bits(c.w1, 12, 12)),
                                 uint16_t(bits(c.w1, 0, 12)));
                ++stats.textureLoadsDecoded;
                break;
            case kSetEnvColor:
                backend.setEnvColor(c.w1);
                ++stats.stateChanges;
                break;
            case kSetPrimColor:
                backend.setPrimColor(uint8_t(bits(c.w0, 0, 8)), uint8_t(bits(c.w0, 8, 5)), c.w1);
                ++stats.stateChanges;
                break;
            case kSetBlendColor:
                backend.setBlendColor(c.w1);
                ++stats.stateChanges;
                break;
            case kSetFogColor:
                backend.setFogColor(c.w1);
                ++stats.stateChanges;
                break;
            case kSetFillColor:
                backend.setFillColor(c.w1);
                ++stats.stateChanges;
                break;
            case kSetOtherMode:
                currentOtherMode.high = bits(c.w0, 0, 24);
                currentOtherMode.low = c.w1;
                backend.setOtherMode(currentOtherMode.high, currentOtherMode.low);
                ++stats.stateChanges;
                break;
            case kSetPrimDepth:
                backend.setPrimDepth(uint16_t(bits(c.w1, 16, 16)), uint16_t(bits(c.w1, 0, 16)));
                ++stats.stateChanges;
                break;
            case kSetScissor:
                backend.setScissor(uint8_t(bits(c.w1, 24, 2)), int32_t(bits(c.w0, 12, 12)),
                                   int32_t(bits(c.w0, 0, 12)), int32_t(bits(c.w1, 12, 12)),
                                   int32_t(bits(c.w1, 0, 12)));
                ++stats.stateChanges;
                break;
            case kSetConvert:
                backend.setConvert(int32_t(bits(c.w0, 13, 9)), int32_t(bits(c.w0, 4, 9)),
                                   int32_t((bits(c.w0, 0, 4) << 5) | bits(c.w1, 27, 5)),
                                   int32_t(bits(c.w1, 18, 9)), int32_t(bits(c.w1, 9, 9)),
                                   int32_t(bits(c.w1, 0, 9)));
                ++stats.stateChanges;
                break;
            case kSetKeyR:
                backend.setKeyR(bits(c.w1, 8, 8), bits(c.w1, 0, 8), bits(c.w1, 16, 12));
                ++stats.stateChanges;
                break;
            case kSetKeyGB:
                backend.setKeyGB(bits(c.w1, 24, 8), bits(c.w1, 16, 8), bits(c.w0, 12, 12),
                                 bits(c.w1, 8, 8), bits(c.w1, 0, 8), bits(c.w0, 0, 12));
                ++stats.stateChanges;
                break;
            case kTexRect:
            case kTexRectFlip: {
                // LLE (raw-RDP) layout -- Cruis'n uses this path, NOT the
                // HLE-only 3-word `texrect()`/`texrectFlip()` handlers (see
                // rt64_gbi_rdp.cpp's `isHLE ? &texrect : &texrectLLE` GBI-map
                // wiring). Real hardware TEXTURE_RECTANGLE is 2 command
                // words: word0=ulx/uly/lrx/lry/tile, word1.w0=uls/ult,
                // word1.w1=dsdx/dtdy -- matches command_words()==2 exactly.
                const int32_t ulx = int32_t(bits(c.w1, 12, 12));
                const int32_t uly = int32_t(bits(c.w1, 0, 12));
                const int32_t lrx = int32_t(bits(c.w0, 12, 12));
                const int32_t lry = int32_t(bits(c.w0, 0, 12));
                const uint8_t tile = uint8_t(bits(c.w1, 24, 3));
                const Cmd& c1 = cmds[i + 1];
                const int16_t uls = int16_t(bits(c1.w0, 16, 16));
                const int16_t ult = int16_t(bits(c1.w0, 0, 16));
                const int16_t dsdx = int16_t(bits(c1.w1, 16, 16));
                const int16_t dtdy = int16_t(bits(c1.w1, 0, 16));
                backend.drawTexRect(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == kTexRectFlip);
                ++stats.rectanglesDecoded;
                break;
            }
            case kFillRect:
                backend.fillRect(int32_t(bits(c.w1, 12, 12)), int32_t(bits(c.w1, 0, 12)),
                                 int32_t(bits(c.w0, 12, 12)), int32_t(bits(c.w0, 0, 12)));
                ++stats.rectanglesDecoded;
                break;
            case kLoadSync:
            case kPipeSync:
            case kTileSync:
                // No-ops on this runtime (matches desktop -- these exist for
                // real hardware's RDP command-buffer pipelining, not needed
                // here). Cruis'n's HUD-anchor tag carrier (E8 with a 'HU'/'HS'
                // magic payload) is a desktop diagnostic, not ported.
                break;
            case kFullSync:
                backend.fullSync();
                ++stats.fullSyncsDecoded;
                break;
            default:
                backend.onUnsupported(RdpEvent::Kind::UnsupportedOpcode, uint8_t(opcode), "opcode");
                ++stats.unsupportedOpcodes;
                break;
        }

        i += cmdsThisCmd;
    }

    return stats;
}

}  // namespace cruisn::vita_rdp
