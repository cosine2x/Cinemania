#pragma once

// The bridge owns only the x86 control-transfer contract. MinHook fills the
// trampoline pointers before enabling any detour.
extern "C"
{
    extern void* cinemania_capture_original;
    extern void* cinemania_cleanup_original;
    extern void* cinemania_reset_original;
    extern void* cinemania_teardown_original;

    void cinemania_capture_bridge();
    void cinemania_cleanup_bridge();
    void cinemania_reset_bridge();
    void cinemania_teardown_bridge();
}

namespace cinemania::viewport_capture_hooks
{
void prepare(void* image_base) noexcept;
}
