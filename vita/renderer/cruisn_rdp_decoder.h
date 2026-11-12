// Cruis'n World Xtreme -- raw RDP command decoder for the Vita renderer.
//
// Pure, portable C++ -- see cruisn_rdp_state.h's file comment. Ports the
// bit-field layouts and triangle edge-walk math from desktop's
// runtime/vendor/rt64/src/gbi/rt64_gbi_rdp.cpp (RT64's own generic raw-RDP
// decoder, which Cruis'n drives directly -- see
// docs/research/VITA_RENDERER_COMPATIBILITY.md). This is NOT a fork of
// RT64's GBI/F3D display-list machinery (Cruis'n never uses that path);
// it's a from-scratch decoder for exactly the opcode set Cruis'n's raw-RDP
// stream uses, targeting IRdpBackend instead of RT64::RDP.
//
// Deliberately NOT ported: CRUISN_RDP_ANYTOUCH, CRUISN_RDP_VERTEX_SNAP, and
// the HUD-provenance/seam-probe/shade-emit-trace diagnostics. Those are
// desktop investigation tooling (all default-off), not part of the stock
// decode path Cruis'n actually needs. Vita starts from plain, stock decoded
// triangle geometry (this pass's Phase 3 instruction).

#pragma once

#include <cstddef>
#include <cstdint>

#include "cruisn_rdp_state.h"

namespace cruisn::vita_rdp {

enum class DecodeStopReason : uint8_t {
    RangeEnd,
    TruncatedCommand,
    TruncatedTriangle,
};

struct DecodeStats {
    uint32_t commandsTotal = 0;
    uint32_t trianglesDecoded = 0;
    uint32_t rectanglesDecoded = 0;      // fill + texture rectangles combined
    uint32_t textureLoadsDecoded = 0;    // LoadTile + LoadBlock + LoadTLUT combined
    uint32_t stateChanges = 0;           // SetXxx commands that aren't image/tile/color
    uint32_t fullSyncsDecoded = 0;
    uint32_t unsupportedOpcodes = 0;
    DecodeStopReason stopReason = DecodeStopReason::RangeEnd;
};

const char* decodeStopReasonName(DecodeStopReason reason);

// Decodes one RDP command range [words, words+wordCount) -- 32-bit words in
// host-native order, two words per 64-bit RDP command word (w0 then w1),
// exactly the layout of RDRAM itself (see the .cpp file comment for why a
// direct RDRAM pointer can be passed here unmodified on Vita). Returns
// aggregate stats for logging/testing; does not throw, never crashes on
// malformed/truncated input (matches Phase 11: unsupported/truncated input
// must not crash the caller).
DecodeStats decodeRdpRange(const uint32_t* words, size_t wordCount, IRdpBackend& backend);

}  // namespace cruisn::vita_rdp
