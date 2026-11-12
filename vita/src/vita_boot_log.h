#pragma once

#include <cstddef>
#include <cstdint>

// Vita-only hardware bring-up log hooks.  These are deliberately tiny so
// shared runtime code can report allocation/preinit milestones under __vita__
// without acquiring a platform logging dependency.
extern "C" void vita_boot_log(const char* fmt, ...);
extern "C" void vita_boot_note_rdram_allocation_begin(size_t bytes);
extern "C" void vita_boot_note_rdram_allocation_success(const void* ptr);
extern "C" void vita_boot_note_rdram_allocation_failed(size_t bytes);
extern "C" void vita_boot_note_runtime_preinit_begin(void);
extern "C" void vita_boot_note_runtime_preinit_complete(void);
extern "C" void vita_boot_note_recomp_entry_reached(void);
extern "C" void vita_boot_note_gfx_task(void);
extern "C" void vita_boot_note_triangle(uint32_t count);
extern "C" void vita_boot_note_texrect(void);
extern "C" void vita_boot_note_present(void);
