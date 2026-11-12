# Build requirements and reproducibility

## Historical toolchain
The existing handoff records VitaSDK 2026.08, `arm-vita-eabi-gcc` 15.2, CMake, SDL2 Vita, vitaGL, vitaShaRK, mathneon, taihen and platform import stubs. Exact package hashes and complete build-specific third-party notice inventory are not established here.

## Historical command
`tools/scripts/build_vita.sh` configures CMake with `$VITASDK/share/vita.toolchain.cmake` and targets `build/vita`. The script is included as a historical project command, not a verified self-contained command for this export. It expects the shared `runtime/`, `runtime/vendor/N64ModernRuntime/`, generated `runtime/generated/` RSP files and `build/recomp/functions/` guest C tree outside this candidate.

## Excluded required inputs
- Shared N64ModernRuntime and PC runtime sources.
- ROM-derived generated guest functions and graphics/audio RSP outputs.
- `runtime/src/cache_stubs.*`, entrypoint and section-registration files.
- VitaSDK and external libraries; not bundled.
- ROM, game assets, VPK/SELF outputs, and Vita `sce_sys` artwork.

The prior physical-device log does not establish a reproducible clean build. No build or test was run while preparing this handoff.
