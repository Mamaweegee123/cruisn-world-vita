// Cruis'n World Xtreme -- PS Vita smoke test.
//
// This is NOT the game. It exists to prove, on a real VPK, that:
//   - the app boots and reaches a controlled main loop (no crash on startup)
//   - vitaGL initializes and can clear/present a frame
//   - sceCtrl input can be polled
//   - the ux0:data writable path and ROM-discovery convention work
//   - the app can log its own progress and exit cleanly
//
// See docs/research/VITA_PORT_ARCHITECTURE.md (Phase 5/6/7) and
// docs/research/VITA_PORT_STATUS.md for what remains before the shared
// Cruis'n runtime can be linked in.

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <vitaGL.h>
#include <xxhash.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Matches the writable-data-path convention documented in
// docs/research/VITA_PORT_ARCHITECTURE.md Phase 7: one platform path, no ROM
// bundled in the VPK, user supplies their own dump.
constexpr char kDataDir[] = "ux0:data/CruisnWorldXtreme";
constexpr char kLogPath[] = "ux0:data/CruisnWorldXtreme/vita_boot.log";
constexpr char kRomPath[] = "ux0:data/CruisnWorldXtreme/cruisn_world_us.z64";

// Same reference ROM desktop validates against (runtime/src/main.cpp,
// parse_expected_hash()) -- same size, same XXH3_64bits algorithm, same
// value. Reusing this instead of introducing SHA-256 matches the existing,
// already-portable hash infrastructure (xxHash is vendored, header+single .c,
// no platform-specific code) rather than adding a second one.
constexpr long long kExpectedRomSize = 12582912;
constexpr unsigned long long kExpectedRomHash = 0x25719e5d267c09d0ULL;

FILE* g_log = nullptr;

void log_line(const char* fmt, ...) {
    if (!g_log) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

bool file_exists(const char* path, SceOff* out_size) {
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0) {
        return false;
    }
    if (out_size) {
        *out_size = st.st_size;
    }
    return true;
}

// [VITA-BOOT] markers per docs/research/VITA_PORT_ARCHITECTURE.md Phase 6.
// This validates the ROM on disk; it does not load it into the (not yet
// linked-in) shared game runtime -- see VITA_PORT_STATUS.md for what that
// still needs.
enum class RomStatus { Missing, WrongSize, WrongHash, Ok, ReadFailed };

RomStatus validate_rom(const char* path, long long expected_size, unsigned long long expected_hash) {
    SceOff size = 0;
    if (!file_exists(path, &size)) {
        return RomStatus::Missing;
    }
    if (size != expected_size) {
        return RomStatus::WrongSize;
    }

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return RomStatus::ReadFailed;
    }
    void* buffer = malloc(static_cast<size_t>(size));
    if (!buffer) {
        sceIoClose(fd);
        return RomStatus::ReadFailed;
    }
    SceSSize read = sceIoRead(fd, buffer, static_cast<SceSize>(size));
    sceIoClose(fd);
    if (read != static_cast<SceSSize>(size)) {
        free(buffer);
        return RomStatus::ReadFailed;
    }

    const unsigned long long hash = XXH3_64bits(buffer, static_cast<size_t>(size));
    free(buffer);
    return hash == expected_hash ? RomStatus::Ok : RomStatus::WrongHash;
}

}  // namespace

int main() {
    sceIoMkdir(kDataDir, 0777);  // Ignore EEXIST; this just ensures the path exists.

    g_log = fopen(kLogPath, "w");
    log_line("Cruis'n World Xtreme -- Vita smoke test boot");

    switch (validate_rom(kRomPath, kExpectedRomSize, kExpectedRomHash)) {
        case RomStatus::Missing:
            log_line("[VITA-BOOT] ROM NOT found at %s -- copy the US Cruis'n World .z64 there.",
                      kRomPath);
            break;
        case RomStatus::WrongSize:
            log_line("[VITA-BOOT] ROM at %s has the wrong size (expected %lld bytes)", kRomPath,
                      kExpectedRomSize);
            break;
        case RomStatus::ReadFailed:
            log_line("[VITA-BOOT] ROM at %s could not be read", kRomPath);
            break;
        case RomStatus::WrongHash:
            log_line("[VITA-BOOT] ROM at %s has the right size but the wrong content (hash mismatch)",
                      kRomPath);
            break;
        case RomStatus::Ok:
            log_line("[VITA-BOOT] ROM loaded and validated (size + XXH3_64bits match desktop's known-good hash)");
            break;
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    vglInitExtended(0, 960, 544, 0x800000, SCE_GXM_MULTISAMPLE_NONE);
    log_line("vitaGL initialized (960x544)");

    int frame = 0;
    bool exit_requested = false;

    while (!exit_requested) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        if ((frame % 60) == 0) {
            log_line("frame=%d buttons=0x%08X lx=%u ly=%u rx=%u ry=%u", frame, pad.buttons,
                      pad.lx, pad.ly, pad.rx, pad.ry);
        }

        // Hold Start+Select to exit cleanly -- there is no other UI yet.
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) {
            exit_requested = true;
        }

        const float t = static_cast<float>(frame % 300) / 300.0f;
        glClearColor(0.5f + 0.5f * t, 0.2f, 1.0f - 0.5f * t, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);

        ++frame;
    }

    log_line("Exit requested (Start+Select) at frame=%d", frame);
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }

    // This vitaGL version exposes no public vglEnd()/vglStop(); process exit
    // via sceKernelExitProcess() tears down the GPU context, matching common
    // vitaGL homebrew practice.
    sceKernelExitProcess(0);
    return 0;
}
