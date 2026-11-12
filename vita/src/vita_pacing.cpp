#include "vita_pacing.h"

#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" void vita_boot_log(const char*, ...);

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point epoch = Clock::now();
std::atomic<uint32_t> vi_seq{0};
std::atomic<uint32_t> gfx_submit_seq{0};
std::atomic<uint32_t> gfx_complete_seq{0};
std::atomic<uint32_t> present_seq{0};
std::atomic<uint32_t> gfxpace_state{0};
std::atomic<uint32_t> latergfx_counts[8]{};
std::atomic<uint32_t> latergfx_thresholds{0};
std::atomic<uint32_t> latergfx_lines{0};
std::atomic<bool> latergfx_baseline{false};
std::atomic<uint32_t> latergfx_ctor_total{0};
std::atomic<uint32_t> t3frontier_stage_value{0};
std::atomic_bool ad6ec_outcome_emitted{false};

const char* latergfx_name(unsigned index) {
    static const char* const names[8] = {
        "ctor", "wrapper", "sp_dequeue", "rsp_done",
        "rdp_publish", "rdp_receive", "present", "gfx_complete"
    };
    return index < 8 ? names[index] : "unknown";
}

uint64_t elapsed_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - epoch).count());
}

bool claim(std::atomic<uint32_t>& counter, uint32_t& seq) {
    uint32_t current = counter.load(std::memory_order_relaxed);
    while (current < 8 &&
           !counter.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
    }
    if (current >= 8) return false;
    seq = current + 1;
    return true;
}

void latergfx_snapshot(const char* trigger, uint32_t threshold) {
    uint32_t current = latergfx_lines.load(std::memory_order_relaxed);
    while (current < 16 &&
           !latergfx_lines.compare_exchange_weak(current, current + 1,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
    }
    if (current >= 16) return;
    vita_boot_log("[VITA-LATERGFX] t_us=%llu trigger=%s threshold=%u ctor=%u wrapper=%u sp_dequeue=%u rsp_done=%u rdp_publish=%u rdp_receive=%u present=%u gfx_complete=%u",
        static_cast<unsigned long long>(elapsed_us()), trigger, threshold,
        latergfx_counts[0].load(std::memory_order_relaxed),
        latergfx_counts[1].load(std::memory_order_relaxed),
        latergfx_counts[2].load(std::memory_order_relaxed),
        latergfx_counts[3].load(std::memory_order_relaxed),
        latergfx_counts[4].load(std::memory_order_relaxed),
        latergfx_counts[5].load(std::memory_order_relaxed),
        latergfx_counts[6].load(std::memory_order_relaxed),
        latergfx_counts[7].load(std::memory_order_relaxed));
}

void latergfx_record(unsigned index) {
    const uint32_t value = latergfx_counts[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (index == 7 && value == 1) {
        bool expected = false;
        if (latergfx_baseline.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            latergfx_snapshot("baseline", 1);
        }
    }
    static constexpr uint32_t thresholds[] = {2, 3, 4, 8, 16};
    for (unsigned i = 0; i < 5; ++i) {
        if (value != thresholds[i]) continue;
        const uint32_t bit = 1u << i;
        if ((latergfx_thresholds.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            latergfx_snapshot(latergfx_name(index), thresholds[i]);
        }
    }
}
}

extern "C" uint64_t vita_pace_elapsed_us() {
    return elapsed_us();
}

extern "C" void vita_pace_vi() {
    uint32_t seq = 0;
    if (claim(vi_seq, seq)) vita_boot_log("[VITA-PACE] vi seq=%u t_us=%llu", seq,
                                          static_cast<unsigned long long>(elapsed_us()));
}

extern "C" void vita_pace_gfx_submit(uint32_t task_type) {
    if (task_type != 1) return;
    uint32_t seq = 0;
    if (claim(gfx_submit_seq, seq)) vita_boot_log("[VITA-PACE] gfx_submit seq=%u t_us=%llu",
                                                   seq, static_cast<unsigned long long>(elapsed_us()));
}

extern "C" uint32_t vita_pace_gfx_submit_count() {
    return gfx_submit_seq.load(std::memory_order_relaxed);
}

extern "C" void vita_pace_gfx_complete() {
    uint32_t seq = 0;
    if (claim(gfx_complete_seq, seq)) vita_boot_log("[VITA-PACE] gfx_complete seq=%u t_us=%llu", seq,
                                                    static_cast<unsigned long long>(elapsed_us()));
}

extern "C" void vita_pace_present() {
    uint32_t seq = 0;
    if (claim(present_seq, seq)) vita_boot_log("[VITA-PACE] present seq=%u t_us=%llu", seq,
                                               static_cast<unsigned long long>(elapsed_us()));
}

void gfxpace_log(uint32_t bit, const char* name, uint32_t start = 0, uint32_t end = 0, bool range = false) {
    uint32_t old = gfxpace_state.load(std::memory_order_acquire);
    if ((old & 1u) == 0) return;
    for (;;) {
        if ((old & bit) != 0) return;
        if (gfxpace_state.compare_exchange_weak(old, old | bit, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            if (range) {
                vita_boot_log("[VITA-GFXPACE] %s t_us=%llu start=0x%08X end=0x%08X", name,
                    (unsigned long long)elapsed_us(), start, end);
            } else {
                vita_boot_log("[VITA-GFXPACE] %s t_us=%llu", name,
                    (unsigned long long)elapsed_us());
            }
            return;
        }
    }
}

extern "C" void vita_gfxpace_rsp_begin() {
    uint32_t expected = 0;
    if (gfxpace_state.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel)) {
        vita_boot_log("[VITA-GFXPACE] rsp_begin t_us=%llu", (unsigned long long)elapsed_us());
    }
}
extern "C" void vita_gfxpace_rsp_end() { gfxpace_log(1u << 1, "rsp_end"); }
extern "C" void vita_gfxpace_rdp_publish(uint32_t start, uint32_t end) { gfxpace_log(1u << 2, "rdp_publish", start, end, true); }
extern "C" void vita_gfxpace_rdp_receive(uint32_t start, uint32_t end) { gfxpace_log(1u << 3, "rdp_receive", start, end, true); }
extern "C" void vita_gfxpace_rdp_begin(uint32_t start, uint32_t end) { gfxpace_log(1u << 4, "rdp_begin", start, end, true); }
extern "C" void vita_gfxpace_swap_begin() { gfxpace_log(1u << 5, "swap_begin"); }
extern "C" void vita_gfxpace_swap_end() { gfxpace_log(1u << 6, "swap_end"); }

extern "C" void vita_latergfx_ctor() {
    latergfx_record(0);
    const uint32_t count = latergfx_ctor_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 2) t3frontier_stage_value.store(4, std::memory_order_release);
}
extern "C" void vita_latergfx_wrapper(uint32_t task_type) {
    if (task_type == 1) latergfx_record(1);
}
extern "C" void vita_latergfx_sp_dequeue(uint32_t task_type) {
    if (task_type == 1) latergfx_record(2);
}
extern "C" void vita_latergfx_rsp_done(uint32_t task_type) {
    if (task_type == 1) latergfx_record(3);
}
extern "C" void vita_latergfx_rdp_publish() { latergfx_record(4); }
extern "C" void vita_latergfx_rdp_receive() { latergfx_record(5); }
extern "C" void vita_latergfx_present() { latergfx_record(6); }
extern "C" void vita_latergfx_complete() { latergfx_record(7); }
extern "C" void vita_t3frontier_stage(uint32_t stage) {
    t3frontier_stage_value.store(stage, std::memory_order_release);
}
extern "C" uint32_t vita_t3frontier_guest_stage() {
    return t3frontier_stage_value.load(std::memory_order_acquire);
}
extern "C" uint32_t vita_latergfx_ctor_count() {
    return latergfx_ctor_total.load(std::memory_order_acquire);
}

extern "C" void vita_ad6ec_outcome(uint32_t path, uint32_t guard, uint32_t queue,
                                    uint32_t wrapper, int32_t result) {
    bool expected = false;
    if (!ad6ec_outcome_emitted.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
        return;
    }
    if (path == 0) {
        vita_boot_log("[VITA-AD6EC] path=skip guard=%u wrapper=0x%08X",
                      guard, wrapper);
    } else {
        vita_boot_log("[VITA-AD6EC] path=send_returned guard=%u queue=0x%08X wrapper=0x%08X result=%d",
                      guard, queue, wrapper, result);
    }
}
