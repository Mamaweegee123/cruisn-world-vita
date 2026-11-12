// Cruis'n World Xtreme -- Vita RDP state + backend interface.
//
// Pure, portable C++. NO vitaGL/psp2/platform includes here on purpose (see
// docs/research/VITA_RENDERER_COMPATIBILITY.md and this pass's Phase 8
// instruction): this header, cruisn_rdp_decoder.h/.cpp, and
// CruisnRdpRecordingBackend below must all build and run on the host (macOS)
// with a plain C++ compiler, so the decoder can be unit-tested without a
// Vita or Vita3K. Only cruisn_vitagl_backend.h/.cpp (a separate file) is
// Vita-only.
//
// IRdpBackend's method shapes intentionally match desktop RT64's `RDP`
// class (runtime/vendor/rt64/src/hle/rt64_rdp.h) and devnoname120/rt64-vita's
// `FastRDP` class -- both were confirmed, by direct signature comparison, to
// already expose this same API surface (see
// docs/research/VITA_RENDERER_COMPATIBILITY.md section 3). This is not a new
// design; it's the same boundary Cruis'n's own desktop raw-RDP decoder
// (runtime/vendor/rt64/src/gbi/rt64_gbi_rdp.cpp) already calls into.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cruisn::vita_rdp {

constexpr int kTileCount = 8;

struct Tile {
    uint8_t fmt = 0;     // 0=RGBA 1=YUV 2=CI 3=IA 4=I
    uint8_t siz = 0;     // 0=4bit 1=8bit 2=16bit 3=32bit
    uint16_t line = 0;   // TMEM stride, 64-bit words
    uint16_t tmem = 0;   // TMEM address, 64-bit words
    uint8_t palette = 0;
    uint8_t cmt = 0, cms = 0;     // clamp/wrap flags (t, s)
    uint8_t maskt = 0, masks = 0; // wrap mask (t, s)
    uint8_t shiftt = 0, shifts = 0;
    uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0; // SetTileSize/LoadTile extents, 10.2 fixed point
};

struct ColorImage {
    uint8_t fmt = 0;
    uint8_t siz = 0;
    uint16_t width = 0;
    uint32_t address = 0;
};

// Raw SetOtherMode register halves, plus the handful of decoded fields the
// decoder and backend actually need. Bit layout cross-validated against
// cen64 (see tools/decode_rdp_state.py's header comment).
struct OtherMode {
    uint32_t high = 0; // 24 bits, from p0(0,24)
    uint32_t low = 0;  // w1

    int cycleType() const { return int((high >> 20) & 0x3); }       // 0=1cyc 1=2cyc 2=copy 3=fill
    bool tlutEnable() const { return (high & 0x8000) != 0; }
    bool tlutIA() const { return (high & 0x4000) != 0; }             // else RGBA16
    bool textureFilterEnable() const { return (high & 0x2000) != 0; }
    bool texturePersp() const { return (high & 0x80000) != 0; }
    bool zCompareEnable() const { return (low & 0x10) != 0; }
    bool zUpdateEnable() const { return (low & 0x20) != 0; }
};

// A fully decoded triangle vertex, matching the layout desktop's
// decodeTriangles() produces (posWorkBuffer/texcoordWorkBuffer/
// colorWorkBuffer, see rt64_gbi_rdp.cpp). Kept as flat arrays (not a struct
// of arrays) in DrawTris' signature to match FastRDP::drawTris/RT64::RDP::
// drawTris exactly.
struct RdpEvent {
    enum class Kind {
        Unknown,
        SetColorImage,
        SetDepthImage,
        SetTextureImage,
        SetCombine,
        SetTile,
        SetTileSize,
        LoadTile,
        LoadBlock,
        LoadTLUT,
        SetEnvColor,
        SetPrimColor,
        SetBlendColor,
        SetFogColor,
        SetFillColor,
        SetOtherMode,
        SetPrimDepth,
        SetScissor,
        SetConvert,
        SetKeyR,
        SetKeyGB,
        TexRect,
        TexRectFlip,
        FillRect,
        LoadSync,
        PipeSync,
        TileSync,
        FullSync,
        Triangle,
        UnsupportedOpcode,
    };
};

// Implemented once for real on Vita (CruisnVitaGLBackend, Vita-only file)
// and once for host-side unit tests (CruisnRdpRecordingBackend below).
class IRdpBackend {
public:
    virtual ~IRdpBackend() = default;

    virtual void setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) = 0;
    virtual void setDepthImage(uint32_t address) = 0;
    virtual void setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) = 0;
    virtual void setCombine(uint64_t combine) = 0;
    virtual void setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem,
                         uint8_t palette, uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks,
                         uint8_t shiftt, uint8_t shifts) = 0;
    virtual void setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) = 0;
    virtual void loadTile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) = 0;
    virtual void loadBlock(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt) = 0;
    virtual void loadTLUT(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) = 0;
    virtual void setEnvColor(uint32_t color) = 0;
    virtual void setPrimColor(uint8_t lodFrac, uint8_t lodMin, uint32_t color) = 0;
    virtual void setBlendColor(uint32_t color) = 0;
    virtual void setFogColor(uint32_t color) = 0;
    virtual void setFillColor(uint32_t color) = 0;
    virtual void setOtherMode(uint32_t high, uint32_t low) = 0;
    virtual void setPrimDepth(uint16_t z, uint16_t dz) = 0;
    virtual void setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) = 0;
    virtual void setConvert(int32_t k0, int32_t k1, int32_t k2, int32_t k3, int32_t k4, int32_t k5) = 0;
    virtual void setKeyR(uint32_t centerR, uint32_t scaleR, uint32_t widthR) = 0;
    virtual void setKeyGB(uint32_t centerG, uint32_t scaleG, uint32_t widthG,
                          uint32_t centerB, uint32_t scaleB, uint32_t widthB) = 0;

    // ulx/uly/lrx/lry are raw 10.2 fixed-point RDP fields (pixel = raw >> 2),
    // matching what rt64_gbi_rdp.cpp's texrect/fillRect handlers pass in --
    // no unit conversion happens in the decoder, matching desktop exactly.
    virtual void drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile,
                             int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) = 0;
    virtual void fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) = 0;

    // pos/tc/col are flat float arrays, count*{4,2,4} floats respectively --
    // identical layout to FastRDP::drawTris/RT64::RDP::drawTris.
    virtual void drawTris(uint32_t count, const float* pos, const float* tc, const float* col,
                          uint8_t tile, uint8_t level) = 0;

    // task-boundary signal (RDP FullSync, opcode 0x29).
    virtual void fullSync() = 0;

    // Called once per unique (event kind, opcode) the decoder cannot handle,
    // and once per unique (combine-mux) the backend doesn't have a shader
    // for. Backends should log-once and continue (Phase 11: safe failure).
    virtual void onUnsupported(RdpEvent::Kind kind, uint8_t opcode, const char* detail) = 0;
};

}  // namespace cruisn::vita_rdp
