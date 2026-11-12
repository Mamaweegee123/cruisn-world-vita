#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <ultramodern/vita_memory.h>

extern "C" void vita_boot_log(const char*, ...) {}

static void expect_fault(uint64_t guest, size_t size, vita_memory_access access = VITA_MEMORY_READ) {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        (void)vita_memory_resolve_span(guest, size, access);
        std::exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
}

int main() {
    constexpr size_t page = 4096;
    auto* rdram = static_cast<uint8_t*>(std::calloc(1, 8 * 1024 * 1024));
    auto* handles = static_cast<uint8_t*>(std::calloc(1, page));
    auto* heap = static_cast<uint8_t*>(std::calloc(1, 2 * page));
    auto* sp = static_cast<uint8_t*>(std::calloc(1, 2 * page));
    auto* rom = static_cast<uint8_t*>(std::calloc(1, 2 * page));
    assert(rdram && handles && heap && sp && rom);
    vita_memory_initialize();
    assert(vita_memory_map_contiguous(0x00000000, rdram, 8 * 1024 * 1024, "RDRAM"));
    assert(vita_memory_map_contiguous(0x00800000, handles, page, "handles"));
    assert(vita_memory_map_contiguous(0x01000000, heap, 2 * page, "heap"));
    assert(vita_memory_map_contiguous(0x04000000, sp, 2 * page, "SP"));
    assert(vita_memory_map_contiguous(0x10000000, rom, 2 * page, "ROM"));
    assert(!vita_memory_map_contiguous(0x00000000, rdram, page, "conflict"));

    *static_cast<uint8_t*>(vita_memory_resolve(0x80300000, 1, VITA_MEMORY_WRITE)) = 0x5A;
    assert(*static_cast<uint8_t*>(vita_memory_resolve(0xA0300000, 1, VITA_MEMORY_READ)) == 0x5A);
    assert(vita_memory_resolve(0x80300000, 1, VITA_MEMORY_READ) == vita_memory_resolve(0xA0300000, 1, VITA_MEMORY_READ));
    assert(vita_memory_resolve(0x00800000, 1, VITA_MEMORY_READ) == handles);
    assert(vita_memory_resolve(0x81000000, 1, VITA_MEMORY_READ) == heap);
    assert(vita_memory_resolve(0x84000000, 1, VITA_MEMORY_READ) == sp);
    assert(vita_memory_resolve(0x84001000, 1, VITA_MEMORY_READ) == sp + page);
    assert(vita_memory_resolve(0xB0000000, 1, VITA_MEMORY_READ) == rom);

    // PI_STATUS_REG is an unmapped device adapter, not a sparse RAM page.
    // The native PI path is synchronous, so its observable busy state is zero.
    auto* pi_status_k1 = static_cast<volatile uint32_t*>(
        vita_memory_resolve(0xA4600010, sizeof(uint32_t), VITA_MEMORY_READ));
    auto* pi_status_k0 = static_cast<volatile uint32_t*>(
        vita_memory_resolve(0x84600010, sizeof(uint32_t), VITA_MEMORY_READ));
    assert(pi_status_k1 == pi_status_k0);
    assert(*pi_status_k1 == 0);
    assert(vita_memory_contiguous_size(0x04600010) == 0);

    // SP_STATUS_REG is a semantic CPU word-load adapter, not a sparse RAM page.
    auto* sp_status_k1 = static_cast<volatile uint32_t*>(
        vita_memory_resolve_word_load(0xA4040010, sizeof(uint32_t)));
    auto* sp_status_k0 = static_cast<volatile uint32_t*>(
        vita_memory_resolve_word_load(0x84040010, sizeof(uint32_t)));
    assert(sp_status_k1 == sp_status_k0);
    assert(*sp_status_k1 == 0); // idle/no-yield runtime model
    assert(vita_memory_contiguous_size(0x04040010) == 0);
    expect_fault(0xA4040010, sizeof(uint32_t), VITA_MEMORY_WRITE); // direct store remains rejected
    expect_fault(0xA4040014, sizeof(uint32_t)); // unknown nearby SP MMIO remains rejected

    const uint8_t in[] = {1, 2, 3, 4};
    uint8_t out[sizeof(in)]{};
    vita_memory_copy_to_guest(0x00000FFE, in, sizeof(in));
    vita_memory_copy_from_guest(out, 0x00000FFE, sizeof(out));
    assert(std::memcmp(in, out, sizeof(in)) == 0);
    assert(vita_memory_resolve_span(0x00000FFE, sizeof(in), VITA_MEMORY_READ) == rdram + 0xFFE);
    assert(vita_memory_contiguous_size(0x01000000) == 2 * page);
    assert(vita_memory_host_to_physical(heap + 17) == 0x01000011);
    // Handler-backed MMIO and arbitrary physical addresses stay unmapped;
    // they must fault rather than aliasing low RDRAM.
    expect_fault(0x84480000, 1);
    expect_fault(0xA2000000, 1);

    std::free(rdram); std::free(handles); std::free(heap); std::free(sp); std::free(rom);
    std::puts("vita_memory_test: PASS");
}
