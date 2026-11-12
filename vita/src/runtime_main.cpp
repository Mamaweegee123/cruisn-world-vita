// Cruis'n World Xtreme -- PS Vita real runtime entry point.
//
// Unlike smoke_main.cpp (which proves the toolchain/vitaGL/sceCtrl/ROM-path
// plumbing in isolation), this links the actual shared Cruis'n runtime:
// cruisn_recompiled_vita (the real recompiled N64 game code, same source as
// desktop) + librecomp + ultramodern, built with RECOMP_ENABLE_MODS=OFF (see
// docs/research/VITA_PORT_STATUS.md).
//
// This is a genuinely new, Vita-specific main() -- not a copy of
// runtime/src/main.cpp, which is full of SDL2/RT64-specific setup that
// doesn't apply here. The one exception is the boot-continuation sequence
// below (restore_initial_main_image/restore_ipl3_decrypted_residue/
// restore_ipl3_sp_handoff_state/register_entrypoint_continuation), which is
// copied verbatim from main.cpp: it's Cruis'n-specific IPL3/boot-state
// reconstruction, not platform glue, and main.cpp keeps those functions in
// an anonymous namespace so they can't be called from another translation
// unit. Extracting them into one shared, non-anonymous location that both
// desktop and Vita call is the correct long-term fix (see
// docs/research/VITA_PORT_STATUS.md's NEXT BEST STEP) -- not done here to
// avoid touching desktop's main.cpp under this pass's time constraints.
//
// IMPORTANT: everything below "compiles and links" is UNVERIFIED. No
// physical Vita or Vita3K was available in this environment. See
// docs/research/VITA_PORT_STATUS.md for exactly what was and wasn't checked.

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vitaGL.h>
#include <xxhash.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/rdp_stream.hpp"
#include "ultramodern/flight_recorder.hpp"
#include "lifecycle_confirm_exit_patch.h"
#include "hud_item_identification.h"
#include "vita_boot_log.h"
#include "vita_pacing.h"
#include <ultramodern/vita_memory.h>
#include "../renderer/cruisn_rdp_decoder.h"
#include "../renderer/cruisn_vitagl_backend.h"

// Same declarations as runtime/src/main.cpp -- defined in
// runtime/src/section_registration.c (already linked via cruisn_recompiled_vita).
extern "C" SectionTableEntry* cruisn_code_sections(void);
extern "C" size_t cruisn_num_code_sections(void);
extern "C" int* cruisn_overlay_sections_by_index(void);
extern "C" size_t cruisn_num_overlay_sections(void);

// Set by create_renderer() below, mirroring runtime/src/rt64_renderer.cpp's
// own g_rdram assignment. Read directly by cruisn_is_race_active() and by
// runtime/vendor/N64ModernRuntime/librecomp/src/overlays.cpp's get_function()
// diagnostic path.
uint8_t* g_rdram = nullptr;
static std::atomic_bool g_update_gfx_task2_observed{false};

enum VitaWatchdogPhase : int {
    VITA_WATCHDOG_IDLE = 0,
    VITA_WATCHDOG_TASK2_LOG_ENTER,
    VITA_WATCHDOG_TASK2_LOG_EXIT,
    VITA_WATCHDOG_CALLBACK_EARLY_RETURN,
    VITA_WATCHDOG_CTRL_PRE,
    VITA_WATCHDOG_CTRL_POST,
    VITA_WATCHDOG_CALLBACK_RETURN,
    VITA_WATCHDOG_MAINLOOP_AFTER_UPDATE_GFX,
};

static std::atomic<int> g_vita_watchdog_phase{VITA_WATCHDOG_IDLE};
static std::atomic_bool g_vita_watchdog_triggered{false};
static std::atomic_bool g_vita_watchdog_started{false};

extern "C" void vita_watchdog_set_phase(int phase) {
    g_vita_watchdog_phase.store(phase, std::memory_order_release);
    if (phase == VITA_WATCHDOG_TASK2_LOG_ENTER) {
        g_vita_watchdog_triggered.store(true, std::memory_order_release);
    }
}

namespace {

constexpr char kVitaWatchdogPath[] = "ux0:data/CruisnWorldXtreme/vita_watchdog.bin";

struct VitaWatchdogRecord {
    // Little-endian 32-byte record: magic u32, version u16, sample u16,
    // timestamp_us u64, phase i32, io_status i32, reserved u32[2].
    uint32_t magic;
    uint16_t version;
    uint16_t sample_index;
    uint64_t timestamp_us;
    int32_t phase;
    int32_t io_status;
    uint32_t reserved[2];
};

static_assert(sizeof(VitaWatchdogRecord) == 32, "watchdog record layout changed");

void vita_watchdog_thread_main() {
    // Bound the pre-trigger wait so a launch without task 2 cannot keep this
    // diagnostic thread alive indefinitely.
    constexpr unsigned kMaxWaitMs = 30'000;
    unsigned waited_ms = 0;
    while (!g_vita_watchdog_triggered.load(std::memory_order_acquire) &&
           waited_ms < kMaxWaitMs) {
        sceKernelDelayThread(1000);
        ++waited_ms;
    }
    if (!g_vita_watchdog_triggered.load(std::memory_order_acquire)) {
        return;
    }

    const SceUID fd = sceIoOpen(kVitaWatchdogPath,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }

    constexpr unsigned kDelaysUs[] = {10'000, 100'000, 500'000, 1'000'000};
    uint64_t elapsed_us = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint64_t target_us = kDelaysUs[i];
        sceKernelDelayThread(static_cast<unsigned>(target_us - elapsed_us));
        elapsed_us = target_us;
        VitaWatchdogRecord record{
            0x47445756u, // little-endian bytes "VWDG"
            1,
            static_cast<uint16_t>(i),
            vita_pace_elapsed_us(),
            g_vita_watchdog_phase.load(std::memory_order_acquire),
            0,
            {0, 0},
        };
        const SceSSize written = sceIoWrite(fd, &record, sizeof(record));
        if (written != static_cast<SceSSize>(sizeof(record))) {
            break;
        }
    }
    sceIoClose(fd);
}

void vita_watchdog_start() {
    bool expected = false;
    if (!g_vita_watchdog_started.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;
    }
    try {
        std::thread(vita_watchdog_thread_main).detach();
    } catch (...) {
        // Diagnostic initialization failure must not affect normal runtime.
    }
}

} // namespace

extern "C" bool vita_update_gfx_task2_observed() {
    return g_update_gfx_task2_observed.load(std::memory_order_relaxed);
}

// Stock FOV/widescreen behavior on Vita (matches
// docs/research/VITA_PORT_ARCHITECTURE.md Phase 12: "stock FOV/widescreen
// behavior where practical"). These two are called unconditionally from
// librecomp/src/rsp.cpp's per-graphics-task projection patch, so SOME
// definition is required regardless of platform; desktop's real
// implementation (runtime/src/main.cpp) depends on SDL_Window sizing for its
// AUTO aspect mode, which doesn't apply here. Returning 1.0 leaves the
// projection matrix scaling as a no-op -- original 4:3 N64 behavior, not
// widescreen. Porting real widescreen support to Vita is future work, not
// attempted in this pass.
extern "C" float cruisn_get_projection_factor(void) { return 1.0f; }
extern "C" float cruisn_get_fov_factor(void) { return 1.0f; }

// Verbatim from runtime/src/main.cpp (see g_rdram above) -- reads Cruis'n's
// own race-active mode word directly out of RDRAM, no desktop dependency.
extern "C" int cruisn_is_race_active(void) {
    if (g_rdram == nullptr) {
        return 0;
    }
    constexpr uint32_t kModeWord = 0x003B9AE0u;
    const uint32_t modeWord = *reinterpret_cast<const uint32_t*>(vita_memory_resolve(kModeWord, sizeof(uint32_t), VITA_MEMORY_READ));
    return modeWord == 1u;
}

// RT lifetime trace: an opt-in desktop RT64 debugging aid (see
// runtime/src/main.cpp's own copy). Always disabled here -- there's no Vita
// renderer to trace yet.
extern "C" int cruisn_rt_trace_enabled(void) { return 0; }
extern "C" uint32_t cruisn_rt_trace_next_seq(void) { return 0; }
extern "C" double cruisn_rt_trace_elapsed_ms(void) { return 0.0; }
extern "C" void cruisn_rt_trace_line(const char*) {}

// NOT extern "C" -- runtime/generated/cruisn_gfx_ucode.cpp and
// cruisn_audio_ucode.cpp define these with ordinary C++ linkage (matching
// runtime/src/main.cpp's own declarations). Must be at true file scope, not
// inside the anonymous namespace below, or these become distinct
// internal-linkage declarations that never bind to the real definitions.
RspExitReason cruisn_gfx_ucode(uint8_t* rdram, uint32_t ucode_addr);
RspExitReason cruisn_audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

// Diagnostics-only main.cpp globals with no boot-correctness dependency;
// stubbed to safe neutral values rather than replicating desktop's
// VI-origin-tracking/env-var-gated init machinery.
extern "C" uint32_t cruisn_get_frame_counter(void) { return 0; }
extern "C" int cruisn_get_hud_style(void) { return 0; }

// Special Options (ALL TRACKS/ALL UPGRADES save-safety hooks): not wired for
// Vita yet (see docs/research/VITA_PORT_STATUS.md). These pass the stock
// value through unchanged rather than pulling in
// xtreme_bonus_championship_guard.h/xtreme_upgrade_profile_guard.h/
// xtreme_track_selection_guard.h, so stock (non-Xtreme) game behavior is
// preserved exactly rather than guessed at.
extern "C" uint32_t cruisn_xtreme_bonus_championship_unlocked(uint8_t*, uint32_t stockFlag) { return stockFlag; }
extern "C" uint32_t cruisn_xtreme_bonus_championship_menu(uint8_t*, uint32_t stockFlag) { return stockFlag; }
extern "C" void cruisn_xtreme_sanitize_save_staging(uint8_t*) {}
extern "C" uint32_t cruisn_xtreme_effective_bonus_track_flag(uint8_t*, uint32_t stockFlag) { return stockFlag; }

// CPU write-watch diagnostics (CRUISN_HUD_CPU_WRITE_WATCH on desktop): opt-in
// investigation tooling with a SIGSEGV/mprotect-based implementation
// (cpu_store_probe.h) not ported here. No-op is correct: the desktop
// callers are unconditional call sites, but the watch itself only does
// anything once explicitly armed via that env var, which is never set here.
extern "C" void cruisn_hud_cpu_watch_arm(uint8_t*, uint32_t) {}
extern "C" void cruisn_hud_cpu_store_watch_arm(uint32_t) {}

namespace {

constexpr char kDataDir[] = "ux0:data/CruisnWorldXtreme";
constexpr char kLogPath[] = "ux0:data/CruisnWorldXtreme/vita_runtime_boot.log";
constexpr char kFlightRecorderPath[] = "ux0:data/CruisnWorldXtreme/flight_recorder_vita.csv";
constexpr char kFlightRecorderBootstrapPath[] = "ux0:data/CruisnWorldXtreme/flight_recorder_vita_bootstrap.csv";
constexpr char kFlightRecorderRequestPath[] = "ux0:data/CruisnWorldXtreme/dump_flight.request";
constexpr char kRomPath[] = "ux0:data/CruisnWorldXtreme/cruisn_world_us.z64";
constexpr long long kExpectedRomSize = 12582912;
constexpr unsigned long long kExpectedRomHash = 0x25719e5d267c09d0ULL;
// vitaGL's first initialization parameter backs glBegin/glEnd immediate-mode
// submissions. The smoke target needs none, but the real raw-RDP backend does.
constexpr int kVitaGlImmediatePoolBytes = 1024 * 1024;

// Deliberately mutable static storage: it is copied into the diagnostic VBO
// below, so this test never hands a stack address to vitaGL.
alignas(16) GLfloat g_vita_gl_test_triangle[3][3] = {
    {-0.5f, -0.5f, 0.0f},
    { 0.5f, -0.5f, 0.0f},
    { 0.0f,  0.5f, 0.0f},
};

bool g_log_ready = false;
std::atomic<uint64_t> g_gfx_tasks{0};
std::atomic<uint64_t> g_triangles{0};
std::atomic<uint64_t> g_texrects{0};
std::atomic<uint64_t> g_presents{0};

void log_line(const char* fmt, ...);

void run_vitagl_pre_game_draw_test() {
    log_line("[VITA-HWTEST] begin");

    glViewport(0, 0, 960, 544);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    GLint viewport[4] = {};
    GLint program = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    const GLenum setup_error = glGetError();
    log_line("[VITA-HWTEST] state viewport=%ld,%ld,%ld,%ld program=%ld tex=%u depth=%u blend=%u cull=%u glError=0x%04lX",
             static_cast<long>(viewport[0]), static_cast<long>(viewport[1]), static_cast<long>(viewport[2]),
             static_cast<long>(viewport[3]), static_cast<long>(program),
             static_cast<unsigned>(glIsEnabled(GL_TEXTURE_2D)), static_cast<unsigned>(glIsEnabled(GL_DEPTH_TEST)),
             static_cast<unsigned>(glIsEnabled(GL_BLEND)), static_cast<unsigned>(glIsEnabled(GL_CULL_FACE)),
             static_cast<unsigned long>(setup_error));

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    log_line("[VITA-HWTEST] clear complete");

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    GLuint vertex_buffer = 0;
    glGenBuffers(1, &vertex_buffer);
    log_line("[VITA-HWTEST] vbo generated id=%lu glError=0x%04lX", static_cast<unsigned long>(vertex_buffer),
             static_cast<unsigned long>(glGetError()));
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    log_line("[VITA-HWTEST] vbo bound glError=0x%04lX", static_cast<unsigned long>(glGetError()));
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vita_gl_test_triangle), g_vita_gl_test_triangle, GL_STATIC_DRAW);
    log_line("[VITA-HWTEST] vbo uploaded glError=0x%04lX", static_cast<unsigned long>(glGetError()));
    glEnableClientState(GL_VERTEX_ARRAY);
    // With GL_ARRAY_BUFFER bound, this is byte offset zero into vitaGL-owned
    // VBO storage rather than a guest CPU pointer.
    glVertexPointer(3, GL_FLOAT, 0, reinterpret_cast<const GLvoid*>(static_cast<uintptr_t>(0)));
    log_line("[VITA-HWTEST] draw begin");
    glDrawArrays(GL_TRIANGLES, 0, 3);
    log_line("[VITA-HWTEST] draw end");
    log_line("[VITA-HWTEST] glError=0x%04lX", static_cast<unsigned long>(glGetError()));
    glDisableClientState(GL_VERTEX_ARRAY);

    log_line("[VITA-HWTEST] swap begin");
    vglSwapBuffers(GL_FALSE);
    log_line("[VITA-HWTEST] swap end");
    sceKernelDelayThread(2 * 1000 * 1000);
}

void write_log_line(const char* fmt, va_list args) {
    if (!g_log_ready) return;

    // Vita3K creates a stdio log file but does not persist its writes while
    // the guest FILE* remains open.  Use direct SCE I/O and close each line;
    // this is also a deliberately small, heap-free bring-up logger.
    char buffer[512];
    const int formatted = vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
    if (formatted < 0) return;
    const size_t length = static_cast<size_t>(formatted) < sizeof(buffer) - 1
        ? static_cast<size_t>(formatted)
        : sizeof(buffer) - 2;
    buffer[length] = '\n';

    const SceUID log = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (log < 0) return;
    (void)sceIoWrite(log, buffer, static_cast<SceSize>(length + 1));
    (void)sceIoClose(log);
}

#if defined(CRUISN_VITA_CLEAN_PACING)
static bool clean_pacing_log_allowed(const char* fmt) {
    return std::strncmp(fmt, "[VITA-BOOT]", 11) == 0 ||
           std::strncmp(fmt, "[VITA-FAIL]", 11) == 0 ||
           std::strncmp(fmt, "[VITA-MEM", 9) == 0 ||
           std::strncmp(fmt, "[VITA-OUTER]", 12) == 0 ||
           std::strncmp(fmt, "[VITA-START]", 12) == 0 ||
           std::strncmp(fmt, "[VITA-BUILD]", 12) == 0 ||
           std::strncmp(fmt, "[VITA-PACE]", 11) == 0 ||
           std::strncmp(fmt, "[VITA-STARTPACE]", 16) == 0 ||
           std::strncmp(fmt, "[VITA-GFXPACE]", 14) == 0 ||
           std::strncmp(fmt, "[VITA-E0PHASE]", 14) == 0 ||
           std::strncmp(fmt, "[VITA-RSPPHASE]", 15) == 0 ||
           std::strncmp(fmt, "[VITA-LATERGFX]", 15) == 0 ||
           std::strncmp(fmt, "[VITA-AD6EC]", 12) == 0 ||
           std::strncmp(fmt, "[VITA-DPPOST]", 13) == 0 ||
           std::strncmp(fmt, "[VITA-INIT6]", 12) == 0 ||
           std::strncmp(fmt, "[VITA-T3FRONTIER]", 17) == 0 ||
           std::strncmp(fmt, "[VITA-SIWAIT]", 13) == 0 ||
           std::strncmp(fmt, "[VITA-RSPTRACE]", 15) == 0 ||
           std::strncmp(fmt, "[VITA-RSPSELECT]", 16) == 0 ||
           std::strncmp(fmt, "[VITA-MAINLOOP]", 15) == 0;
}
#endif

void log_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_log_line(fmt, args);
    va_end(args);
}

extern "C" void vita_boot_log(const char* fmt, ...) {
#if defined(CRUISN_VITA_CLEAN_PACING)
    if (!clean_pacing_log_allowed(fmt)) return;
#endif
    va_list args;
    va_start(args, fmt);
    write_log_line(fmt, args);
    va_end(args);
}

extern "C" void vita_boot_note_rdram_allocation_begin(size_t bytes) {
    vita_boot_log("[VITA-BOOT] RDRAM allocation begin bytes=%lu", static_cast<unsigned long>(bytes));
}
extern "C" void vita_boot_note_rdram_allocation_success(const void* ptr) {
    vita_boot_log("[VITA-BOOT] RDRAM allocation success ptr=%p", ptr);
}
extern "C" void vita_boot_note_rdram_allocation_failed(size_t bytes) {
    vita_boot_log("[VITA-FAIL] RDRAM allocation failed bytes=%lu", static_cast<unsigned long>(bytes));
}
extern "C" void vita_boot_note_runtime_preinit_begin(void) { vita_boot_log("[VITA-BOOT] runtime preinit begin"); }
extern "C" void vita_boot_note_runtime_preinit_complete(void) { vita_boot_log("[VITA-BOOT] runtime preinit complete"); }
extern "C" void vita_boot_note_recomp_entry_reached(void) { vita_boot_log("[VITA-BOOT] recomp entry reached"); }
extern "C" void vita_boot_note_gfx_task(void) {
    if (g_gfx_tasks.fetch_add(1) == 0) vita_boot_log("[VITA-GFX] first gfx task");
}
extern "C" void vita_boot_note_triangle(uint32_t count) { g_triangles.fetch_add(count); }
extern "C" void vita_boot_note_texrect(void) { g_texrects.fetch_add(1); }
extern "C" void vita_boot_note_present(void) {
    const uint64_t presents = g_presents.fetch_add(1) + 1;
    if (presents == 1 || presents == 10 || presents == 60 || presents == 300) {
        vita_boot_log("[VITA-STAT] presents=%llu gfxTasks=%llu triangles=%llu texrects=%llu",
                      (unsigned long long)presents, (unsigned long long)g_gfx_tasks.load(),
                      (unsigned long long)g_triangles.load(), (unsigned long long)g_texrects.load());
    }
}

// ---------------------------------------------------------------------
// RSP microcode selection -- same logic as runtime/src/main.cpp's
// select_rsp_microcode(), without its trace spam.
// ---------------------------------------------------------------------
RspUcodeFunc* select_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_GFXTASK) {
        vita_boot_note_gfx_task();
        return cruisn_gfx_ucode;
    }
    if (task->t.type == M_AUDTASK) {
        return cruisn_audio_ucode;
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// Real (not stub) Vita renderer: decodes Cruis'n's raw-RDP command stream
// (cruisn_rdp_decoder, see docs/research/VITA_RENDERER_COMPATIBILITY.md)
// and submits it through vitaGL's fixed-function pipeline
// (cruisn_vitagl_backend). send_dl() stays a no-op-log: Cruis'n never uses
// the F3DEX2/GBI display-list path.
// ---------------------------------------------------------------------
class VitaRdpRendererContext : public ultramodern::renderer::RendererContext {
public:
    explicit VitaRdpRendererContext(uint8_t* rdram) : rdram_(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        backend_.setRdram(rdram_);
        // 320x240 is the standard NTSC N64 framebuffer size; SetColorImage's
        // actual width is read per-frame by the decoder/backend regardless,
        // this only sets the initial viewport/ortho mapping (Phase 6).
        backend_.initGL(320, 240, 960, 544);
    }

    bool valid() override { return true; }

    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                        const ultramodern::renderer::GraphicsConfig&) override {
        return true;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask*) override {
        log_line("[VITA-BOOT] send_dl() called -- Cruis'n does not use this path (raw RDP only); "
                  "see VITA_RENDERER_COMPATIBILITY.md");
    }

    void send_rdp_range(uint32_t start, uint32_t end) override {
        if (!logged_first_rdp_range_) {
            logged_first_rdp_range_ = true;
            log_line("[VITA-RDP] first range start=0x%08X end=0x%08X", start, end);
        }
        if (end <= start || rdram_ == nullptr) {
            return;
        }
        const uint32_t* words = static_cast<const uint32_t*>(vita_memory_resolve_span(start, end - start, VITA_MEMORY_READ));
        const size_t wordCount = (end - start) / 4;
        if (wordCount != 0 && !logged_first_rdp_opcode_) {
            logged_first_rdp_opcode_ = true;
            log_line("[VITA-RDP] first command opcode=0x%02X", unsigned(words[0] >> 24) & 0x3F);
        }
#if defined(CRUISN_VITA_CLEAN_PACING)
        const bool traceFirstTask = false;
#else
        const bool traceFirstTask = !logged_first_rdp_task_trace_;
#endif
        if (traceFirstTask) {
            logged_first_rdp_task_trace_ = true;
            traceFirstRdpTask(start, words, wordCount);
        }
        const cruisn::vita_rdp::DecodeStats stats = cruisn::vita_rdp::decodeRdpRange(words, wordCount, backend_);
        if (traceFirstTask) {
            log_line("[VITA-RDP-TRACE] task end commands=%lu fullsync_seen=%u decoder_stop_reason=%s",
                     static_cast<unsigned long>(stats.commandsTotal),
                     stats.fullSyncsDecoded != 0 ? 1u : 0u,
                     cruisn::vita_rdp::decodeStopReasonName(stats.stopReason));
        }
    }

    void send_dummy_workload(uint32_t) override {}
    void update_screen() override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

private:
    static void traceFirstRdpTask(uint32_t start, const uint32_t* words, size_t wordCount) {
        const size_t commandWords = wordCount / 2;
        size_t commandIndex = 0;
        uint32_t traceIndex = 0;
        while (commandIndex < commandWords && traceIndex < 64) {
            const uint32_t w0 = words[commandIndex * 2];
            const uint32_t w1 = words[commandIndex * 2 + 1];
            const uint8_t opcode = static_cast<uint8_t>((w0 >> 24) & 0x3F);
            const size_t commandLength = ultramodern::rdp::command_words(opcode);
            log_line("[VITA-RDP-TRACE] #%lu addr=0x%08lX opcode=0x%02X w0=0x%08lX w1=0x%08lX",
                     static_cast<unsigned long>(traceIndex),
                     static_cast<unsigned long>(start + commandIndex * 8), unsigned(opcode),
                     static_cast<unsigned long>(w0), static_cast<unsigned long>(w1));
            if (commandLength > commandWords - commandIndex) {
                log_line("[VITA-RDP-TRACE] stream boundary=truncated command_words=%lu available=%lu",
                         static_cast<unsigned long>(commandLength),
                         static_cast<unsigned long>(commandWords - commandIndex));
                return;
            }
            commandIndex += commandLength;
            ++traceIndex;
        }
        log_line("[VITA-RDP-TRACE] stream boundary=%s command_headers=%lu bytes=%lu final_command_addr=0x%08lX",
                 commandIndex == commandWords ? "range_end" : "trace_cap",
                 static_cast<unsigned long>(traceIndex), static_cast<unsigned long>(wordCount * sizeof(uint32_t)),
                 static_cast<unsigned long>(start + (commandIndex == 0 ? 0 : (commandIndex - 1) * 8)));
    }

    uint8_t* rdram_ = nullptr;
    cruisn::vita_rdp::CruisnVitaGLBackend backend_;
    bool logged_first_rdp_range_ = false;
    bool logged_first_rdp_opcode_ = false;
    bool logged_first_rdp_task_trace_ = false;
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    g_rdram = rdram;
    log_line("[VITA-BOOT] renderer created (cruisn_vitagl_backend connected)");
    return std::make_unique<VitaRdpRendererContext>(rdram);
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    return ultramodern::renderer::WindowHandle{};
}

// ---------------------------------------------------------------------
// Audio/input: per this pass's scope, must not block boot. Real Vita
// backends (sceAudioOut, sceCtrl -> ultramodern::input::callbacks_t) are
// NOT wired here -- see docs/research/VITA_PORT_STATUS.md NOT STARTED.
// ---------------------------------------------------------------------
void queue_audio_samples(int16_t*, size_t samples) {
    static bool logged = false;
    if (!logged) { logged = true; log_line("[VITA-AUDIO] first buffer samples=%zu (null sink)", samples); }
}
size_t audio_frames_remaining() { return 0; }
void set_audio_frequency(uint32_t) {}

SceCtrlData g_pad{};
bool g_first_input_activity = false;
bool g_flight_recorder_dumped = false;
void poll_input() {
    memset(&g_pad, 0, sizeof(g_pad));
    sceCtrlPeekBufferPositive(0, &g_pad, 1);
    if (!g_first_input_activity && (g_pad.buttons != 0 || g_pad.lx != 128 || g_pad.ly != 128 || g_pad.rx != 128 || g_pad.ry != 128)) {
        g_first_input_activity = true;
        log_line("[VITA-INPUT] first activity buttons=0x%08X lx=%u ly=%u rx=%u ry=%u", g_pad.buttons, g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry);
    }
}

void update_gfx(void*) {
    constexpr uint64_t kTask2DumpDelayUs = 4'000'000;
    constexpr uint64_t kTask1FallbackDelayUs = 5'000'000;
    constexpr uint32_t kRecorderCapacityGuard = 7000;
    static bool task1_seen = false;
    static bool task2_seen = false;
    static uint64_t task1_seen_us = 0;
    static uint64_t task2_seen_us = 0;
    static uint64_t callback_count = 0;
    static uint64_t next_heartbeat_us = 0;
    static uint32_t heartbeat_count = 0;
    static bool ctrl_probe_done = false;
    static bool stat_probe_done = false;
    static bool callback_return_probe_done = false;
    const uint64_t now_us = vita_pace_elapsed_us();
    const uint32_t gfx_submit_count = vita_pace_gfx_submit_count();
    const uint32_t recorder_count = ultramodern::FlightRecorder::instance().get_record_count();
    ++callback_count;
    bool first_task2_callback = false;
    if (!task1_seen && gfx_submit_count >= 1) {
        task1_seen = true;
        task1_seen_us = now_us;
        next_heartbeat_us = now_us + 2'000'000;
        log_line("[VITA-MAINLOOP] observed_task1 elapsed_us=%llu gfx_submit=%u records=%u callbacks=%llu",
            static_cast<unsigned long long>(now_us), gfx_submit_count, recorder_count,
            static_cast<unsigned long long>(callback_count));
    }
    if (!task2_seen && gfx_submit_count >= 2) {
        task2_seen = true;
        first_task2_callback = true;
        task2_seen_us = now_us;
        g_update_gfx_task2_observed.store(true, std::memory_order_relaxed);
        vita_watchdog_set_phase(VITA_WATCHDOG_TASK2_LOG_ENTER);
        log_line("[VITA-MAINLOOP] observed_task2 elapsed_us=%llu gfx_submit=%u records=%u callbacks=%llu",
            static_cast<unsigned long long>(now_us), gfx_submit_count, recorder_count,
            static_cast<unsigned long long>(callback_count));
        vita_watchdog_set_phase(VITA_WATCHDOG_TASK2_LOG_EXIT);
    }
    if (task1_seen && heartbeat_count < 10 && now_us >= next_heartbeat_us) {
        log_line("[VITA-MAINLOOP] heartbeat elapsed_us=%llu gfx_submit=%u records=%u callbacks=%llu",
            static_cast<unsigned long long>(now_us), gfx_submit_count, recorder_count,
            static_cast<unsigned long long>(callback_count));
        ++heartbeat_count;
        next_heartbeat_us = now_us + 2'000'000;
    }

    if (g_flight_recorder_dumped) {
        vita_watchdog_set_phase(VITA_WATCHDOG_CALLBACK_EARLY_RETURN);
        return;
    }

    const char* automatic_reason = nullptr;
    if (recorder_count >= kRecorderCapacityGuard) {
        automatic_reason = "capacity_guard";
    } else if (task2_seen && now_us - task2_seen_us >= kTask2DumpDelayUs) {
        automatic_reason = "task2_deadline";
    } else if (task1_seen && !task2_seen && now_us - task1_seen_us >= kTask1FallbackDelayUs) {
        automatic_reason = "task1_fallback";
    }

    static uint32_t request_poll_ticks = 0;
    static bool request_poll_initialized = false;
    static bool request_was_present = false;
    bool host_request = false;
    if (++request_poll_ticks >= 100) {
        request_poll_ticks = 0;
        SceIoStat request_stat{};
        const bool trace_stat = task2_seen && !stat_probe_done;
        if (trace_stat) {
            log_line("[VITA-MAINLOOP] stat_pre callbacks=%llu", static_cast<unsigned long long>(callback_count));
        }
        const int stat_result = sceIoGetstat(kFlightRecorderRequestPath, &request_stat);
        if (trace_stat) {
            stat_probe_done = true;
            log_line("[VITA-MAINLOOP] stat_post result=%d callbacks=%llu", stat_result,
                static_cast<unsigned long long>(callback_count));
        }
        const bool request_present = stat_result >= 0;
        if (!request_poll_initialized) {
            request_poll_initialized = true;
        } else if (request_present && !request_was_present) {
            host_request = true;
        }
        request_was_present = request_present;
    }

    SceCtrlData diagnostic_pad{};
    const bool trace_ctrl = first_task2_callback && !ctrl_probe_done;
    if (trace_ctrl) {
        vita_watchdog_set_phase(VITA_WATCHDOG_CTRL_PRE);
        log_line("[VITA-MAINLOOP] ctrl_pre callbacks=%llu", static_cast<unsigned long long>(callback_count));
    }
    const int ctrl_result = sceCtrlPeekBufferPositive(0, &diagnostic_pad, 1);
    if (trace_ctrl) {
        vita_watchdog_set_phase(VITA_WATCHDOG_CTRL_POST);
    }
    if (trace_ctrl) {
        ctrl_probe_done = true;
        log_line("[VITA-MAINLOOP] ctrl_post result=%d callbacks=%llu", ctrl_result,
            static_cast<unsigned long long>(callback_count));
    }
    const bool button_request = (diagnostic_pad.buttons & SCE_CTRL_SELECT) != 0 &&
        (diagnostic_pad.buttons & SCE_CTRL_START) != 0;
    if (!host_request && !button_request && automatic_reason == nullptr) {
        if (task2_seen && !callback_return_probe_done) {
            callback_return_probe_done = true;
            log_line("[VITA-MAINLOOP] callback_return callbacks=%llu", static_cast<unsigned long long>(callback_count));
        }
        vita_watchdog_set_phase(VITA_WATCHDOG_CALLBACK_RETURN);
        return;
    }

    if (host_request) {
        log_line("[FLIGHT] host dump request detected path=%s", kFlightRecorderRequestPath);
    } else if (button_request) {
        log_line("[FLIGHT] SELECT+START dump request detected");
    } else {
        log_line("[FLIGHT] automatic dump reason=%s gfx_submit=%u records=%u elapsed_us=%llu",
            automatic_reason, gfx_submit_count, recorder_count,
            static_cast<unsigned long long>(now_us));
    }
    g_flight_recorder_dumped = true;
    log_line("[FLIGHT] dump requested");
    const bool normal_ok = ultramodern::FlightRecorder::instance().dump(kFlightRecorderPath);
    const bool bootstrap_ok = ultramodern::FlightRecorder::instance().dump_bootstrap(kFlightRecorderBootstrapPath);
    log_line("[FLIGHT] normal path=%s result=%s", kFlightRecorderPath, normal_ok ? "OK" : "FAILED");
    log_line("[FLIGHT] bootstrap path=%s result=%s", kFlightRecorderBootstrapPath, bootstrap_ok ? "OK" : "FAILED");
    if (host_request) {
        const int consume_result = sceIoRemove(kFlightRecorderRequestPath);
        log_line("[FLIGHT] host dump request consumed result=%d", consume_result);
    }
    log_line("[FLIGHT] dump complete");
}

bool get_input(int, uint16_t* buttons, float* x, float* y) {
    constexpr uint16_t kA = 0x8000, kB = 0x4000, kZ = 0x2000, kStart = 0x1000;
    constexpr uint16_t kDup = 0x0800, kDdown = 0x0400, kDleft = 0x0200, kDright = 0x0100;
    constexpr uint16_t kL = 0x0020, kR = 0x0010;
    uint16_t out = 0;
    if (g_pad.buttons & SCE_CTRL_CROSS) out |= kA;
    if (g_pad.buttons & SCE_CTRL_CIRCLE) out |= kB;
    if (g_pad.buttons & SCE_CTRL_LTRIGGER) out |= kZ;
    if (g_pad.buttons & SCE_CTRL_START) out |= kStart;
    if (g_pad.buttons & SCE_CTRL_UP) out |= kDup;
    if (g_pad.buttons & SCE_CTRL_DOWN) out |= kDdown;
    if (g_pad.buttons & SCE_CTRL_LEFT) out |= kDleft;
    if (g_pad.buttons & SCE_CTRL_RIGHT) out |= kDright;
    if (g_pad.buttons & SCE_CTRL_LTRIGGER) out |= kL;
    if (g_pad.buttons & SCE_CTRL_RTRIGGER) out |= kR;
    *buttons = out;
    *x = (float(g_pad.lx) - 128.0f) / 127.0f;
    *y = (128.0f - float(g_pad.ly)) / 127.0f;
    return true;
}
void set_rumble(int, bool) {}
ultramodern::input::connected_device_info_t connected_device(int controller_num) {
    return {
        .connected_device = controller_num == 0 ? ultramodern::input::Device::Controller : ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

void message_box(const char* msg) {
    log_line("[VITA-BOOT] runtime message: %s", msg);
}

// Same as main.cpp's sync_os_running_thread: mirrors ultramodern's dispatch
// into Cruis'n's own copy of libultra's "currently running thread" global,
// which some guest helpers read directly instead of via a host API.
constexpr uint32_t kOsRunningThreadAddress = 0x8034D110;
void sync_os_running_thread(uint8_t* rdram, int32_t thread) {
    *static_cast<int32_t*>(vita_memory_resolve(kOsRunningThreadAddress, sizeof(int32_t), VITA_MEMORY_WRITE)) = thread;
}

// ---------------------------------------------------------------------
// Boot-continuation sequence -- copied verbatim from runtime/src/main.cpp
// (see the file-level comment above for why). Constants and comments
// unchanged from their source.
// ---------------------------------------------------------------------
constexpr int32_t kOsRunQueueAddress = static_cast<int32_t>(0x8034D108);
constexpr int32_t kOsThreadTailAddress = static_cast<int32_t>(0x8034D100);

constexpr gpr kInitialMainImageVram = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(0x802AD400)));
constexpr uint32_t kInitialMainImageCartAddress = 0x10001000;
constexpr size_t kInitialMainImageLength = 0x100004;

void restore_initial_main_image(uint8_t* rdram) {
    recomp::do_rom_read(rdram, kInitialMainImageVram, kInitialMainImageCartAddress,
                        kInitialMainImageLength);
}

constexpr uint32_t kIpl3EncryptedRomOffset = 0x4F0;
constexpr uint32_t kIpl3DecryptedByteLength = 0x2BC;
constexpr uint32_t kIpl3DecryptDestVram = 0x80000000;
constexpr uint32_t kCic6106Ipl3Seed = 0x85;
constexpr uint32_t kIpl3DecryptCipherConstant = 0x0260BCD5;

void restore_ipl3_decrypted_residue(uint8_t* rdram) {
    recomp::do_rom_read(rdram, static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(kIpl3DecryptDestVram))),
        recomp::rom_base + kIpl3EncryptedRomOffset, kIpl3DecryptedByteLength);

    uint32_t keystream = static_cast<uint32_t>(kCic6106Ipl3Seed * kIpl3DecryptCipherConstant) + 1;
    for (uint32_t offset = 0; offset < kIpl3DecryptedByteLength; offset += 4) {
        int32_t encrypted = MEM_W(offset, static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(kIpl3DecryptDestVram))));
        MEM_W(offset, static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(kIpl3DecryptDestVram)))) =
            static_cast<int32_t>(static_cast<uint32_t>(encrypted) ^ keystream);
        keystream = keystream * kIpl3DecryptCipherConstant;
    }
}

constexpr gpr kSpDmemVram = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(0xA4000000)));
constexpr gpr kSpImemVram = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(0xA4001000)));

void restore_ipl3_sp_handoff_state(uint8_t* rdram) {
    for (uint32_t offset = 0; offset < 0x1000; offset += 4) {
        MEM_W(offset, kSpDmemVram) = -1;
        MEM_W(offset, kSpImemVram) = -1;
    }
    MEM_W(0, kSpImemVram) = 0x17D8;
}

constexpr uint32_t kOsInfoBlockVram = 0x80000300;
constexpr int32_t kOsTvTypeNtsc = 1;
constexpr int32_t kOsRomTypeCart = 0;
constexpr int32_t kOsRomBaseCart = static_cast<int32_t>(0xB0000000);
constexpr int32_t kOsResetTypeCold = 0;
constexpr int32_t kOsCicId6106 = 6104;
constexpr int32_t kOsVersion = 0;
constexpr int32_t kOsMemSizeBase = 0x400000;

void restore_ipl3_os_info_block(uint8_t* rdram) {
    const gpr base = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(kOsInfoBlockVram)));
    MEM_W(0x00, base) = kOsTvTypeNtsc;
    MEM_W(0x04, base) = kOsRomTypeCart;
    MEM_W(0x08, base) = kOsRomBaseCart;
    MEM_W(0x0C, base) = kOsResetTypeCold;
    MEM_W(0x10, base) = kOsCicId6106;
    MEM_W(0x14, base) = kOsVersion;
    MEM_W(0x18, base) = kOsMemSizeBase;
}

extern "C" void func_802AD450(uint8_t* rdram, recomp_context* ctx);

void register_entrypoint_continuation(uint8_t* rdram, recomp_context*) {
    ultramodern::set_scheduler_queue(rdram, kOsRunQueueAddress);
    ultramodern::set_queue_tail_sentinel(rdram, kOsThreadTailAddress);
    recomp::overlays::add_loaded_function(static_cast<int32_t>(0x802AD450), func_802AD450);
    cruisn_register_func_802BFA6C();
    restore_initial_main_image(rdram);
    restore_ipl3_decrypted_residue(rdram);
    restore_ipl3_os_info_block(rdram);
    restore_ipl3_sp_handoff_state(rdram);
    log_line("[VITA-BOOT] recomp context initialized (boot-continuation ran)");
}

void pi_transfer_complete(uint8_t* rdram) {
    constexpr int32_t kPiCompletionQueue = static_cast<int32_t>(0x803E5140);
    ultramodern::enqueue_external_message_src(kPiCompletionQueue, (OSMesg)0, false, ultramodern::EventMessageSource::Pi);
}

// ---------------------------------------------------------------------
// ROM validation (Level B) -- same xxHash check as cruisn_vita_smoke.
// ---------------------------------------------------------------------
enum class RomStatus { Missing, WrongSize, WrongHash, Ok, ReadFailed };

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

RomStatus validate_rom(const char* path, long long expected_size, unsigned long long expected_hash,
                       SceOff* actual_size, unsigned long long* actual_hash) {
    SceOff size = 0;
    if (!file_exists(path, &size)) {
        return RomStatus::Missing;
    }
    if (actual_size) *actual_size = size;
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
    if (actual_hash) *actual_hash = hash;
    return hash == expected_hash ? RomStatus::Ok : RomStatus::WrongHash;
}

uint64_t parse_expected_hash() {
    return kExpectedRomHash;
}

}  // namespace

int main() {
    // Enable the shared flight recorder before runtime/thread initialization so
    // Vita captures the same bootstrap events as the desktop frontend.
    ultramodern::FlightRecorder::instance().init();
    sceIoMkdir(kDataDir, 0777);
    const SceUID initial_log = sceIoOpen(
        kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (initial_log >= 0) {
        (void)sceIoClose(initial_log);
        g_log_ready = true;
    }
    // Start before runtime/game threads so the diagnostic remains independent
    // of the update_gfx callback once task 2 is observed.
    vita_watchdog_start();
    log_line("[VITA-BOOT] process start");
    log_line("[VITA-BOOT] logger ready");
    log_line("[VITA-BOOT] data directory ready");
    log_line("[VITA-BUILD] date=%s renderer=fast-rdp-v1", __DATE__);
    log_line("[VITA-BUILD] git=%s dirty=%d", CRUISN_VITA_BUILD_GIT, CRUISN_VITA_BUILD_DIRTY);

    SceOff actual_size = 0;
    unsigned long long actual_hash = 0;
    switch (validate_rom(kRomPath, kExpectedRomSize, kExpectedRomHash, &actual_size, &actual_hash)) {
        case RomStatus::Missing:
            log_line("[VITA-FAIL] ROM missing expected=%s", kRomPath);
            sceKernelExitProcess(0);
            return 0;
        case RomStatus::WrongSize:
            log_line("[VITA-FAIL] ROM wrong size actual=%lld expected=%lld", (long long)actual_size, kExpectedRomSize);
            sceKernelExitProcess(0);
            return 0;
        case RomStatus::ReadFailed:
            log_line("[VITA-FAIL] ROM read failed path=%s", kRomPath);
            sceKernelExitProcess(0);
            return 0;
        case RomStatus::WrongHash:
            log_line("[VITA-FAIL] ROM hash mismatch actual=0x%016llx expected=0x%016llx", actual_hash, kExpectedRomHash);
            sceKernelExitProcess(0);
            return 0;
        case RomStatus::Ok:
            log_line("[VITA-BOOT] ROM found");
            log_line("[VITA-BOOT] ROM size valid");
            log_line("[VITA-BOOT] ROM hash valid");
            break;
    }

    log_line("[VITA-GFX] renderer init begin");
    // vitaGL returns whether the requested display resolution was replaced
    // with a supported fallback, not an init-success boolean. GL_FALSE is
    // therefore the normal exact-960x544 result.
    const GLboolean vgl_resolution_fallback = vglInitExtended(kVitaGlImmediatePoolBytes, 960, 544, 0x800000,
                                                              SCE_GXM_MULTISAMPLE_NONE);
    log_line("[VITA-GFX] immediate pool bytes=%lu", static_cast<unsigned long>(kVitaGlImmediatePoolBytes));
    log_line("[VITA-GFX] vgl resolution fallback=%u", static_cast<unsigned>(vgl_resolution_fallback));
    log_line("[VITA-GFX] renderer init success");
#if defined(CRUISN_VITA_HWTEST)
    run_vitagl_pre_game_draw_test();
#endif
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    log_line("[VITA-INPUT] initialized");
    log_line("[VITA-AUDIO] null sink active");

    log_line("[VITA-BOOT] game registration begin");
    recomp::register_config_path(kDataDir);
    recomp::overlays::register_overlays(
        { cruisn_code_sections(), cruisn_num_code_sections(), cruisn_num_code_sections() },
        { cruisn_overlay_sections_by_index(), cruisn_num_overlay_sections() });

    const recomp::GameEntry game{
        .rom_hash = parse_expected_hash(),
        .internal_name = "CRUIS'N WORLD       ",
        .display_name = "Cruis'n World (USA Rev 0)",
        .game_id = u8"cruisn-world-us-r0",
        .mod_game_id = "cruisn-world-us-r0",
        .save_type = recomp::SaveType::Eep16k,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .entrypoint_address = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(0x802AD400))),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = register_entrypoint_continuation,
        .pi_transfer_complete_callback = pi_transfer_complete,
    };
    recomp::register_game(game);
    log_line("[VITA-BOOT] game registration complete");

#if defined(__vita__)
    vita_boot_log("[VITA-OUTER] registration returned");
#endif

    #if defined(__vita__)
    vita_boot_log("[VITA-OUTER] select_rom begin");
#endif

    const auto rom_status = recomp::select_rom(kRomPath, game.game_id);

#if defined(__vita__)
    vita_boot_log("[VITA-OUTER] select_rom returned status=%d", static_cast<int>(rom_status));
#endif
    if (rom_status != recomp::RomValidationError::Good) {
        log_line("[VITA-FAIL] runtime preinit failed select_rom=%d", static_cast<int>(rom_status));
        sceKernelExitProcess(0);
        return 0;
    }

    char app_name[] = "cruisn_world_vita";
    char game_option[] = "--game";
    char game_id[] = "cruisn-world-us-r0";
    char* runtime_argv[] = { app_name, game_option, game_id };

    #if defined(__vita__)
    vita_boot_log("[VITA-OUTER] Configuration construction begin");
#endif

    const recomp::Configuration configuration{
        .argc = 3,
        .argv = runtime_argv,
        .project_version = { 0, 1, 0, "-vita-bringup" },
        .window_handle = ultramodern::renderer::WindowHandle{},
        .rsp_callbacks = { .get_rsp_microcode = select_rsp_microcode },
        .renderer_callbacks = { .create_render_context = create_renderer },
        .audio_callbacks = {
            .queue_samples = queue_audio_samples,
            .get_frames_remaining = audio_frames_remaining,
            .set_frequency = set_audio_frequency,
        },
        .input_callbacks = {
            .poll_input = poll_input,
            .get_input = get_input,
            .set_rumble = set_rumble,
            .get_connected_device_info = connected_device,
        },
        .gfx_callbacks = { .create_window = create_window, .update_gfx = update_gfx },
        .error_handling_callbacks = { .message_box = message_box },
        .threads_callbacks = { .on_current_thread_changed = sync_os_running_thread },
    };

#if defined(__vita__)
    vita_boot_log("[VITA-OUTER] Configuration construction complete");
    vita_boot_log("[VITA-OUTER] calling recomp::start");
#endif

    log_line("[VITA-BOOT] recomp start begin");
    recomp::start(configuration);

    log_line("[VITA-BOOT] recomp::start returned");
    sceKernelExitProcess(0);
    return 0;
}
