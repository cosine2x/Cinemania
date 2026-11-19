#include "viewport_capture_hooks.hpp"

#include "deferred_readback.hpp"
#include "native_render_ahead.hpp"

#include <cstddef>

extern "C"
{
    void* cinemania_capture_original = nullptr;
    void* cinemania_cleanup_original = nullptr;
    void* cinemania_reset_original = nullptr;
    void* cinemania_teardown_original = nullptr;
    void* cinemania_capture_handled = nullptr;

    int __cdecl cinemania_capture_callback(void* viewport) noexcept
    {
        cinemania::native_render_ahead::capture(viewport);
        return cinemania::readback::capture(viewport) ? 1 : 0;
    }

    void __cdecl cinemania_cleanup_callback(void* viewport) noexcept
    {
        cinemania::readback::cleanup(viewport);
        cinemania::native_render_ahead::cleanup(viewport);
    }

    void __cdecl cinemania_device_callback(void* viewport) noexcept
    {
        cinemania::readback::device_change(viewport);
        cinemania::native_render_ahead::device_change(viewport);
    }
}

namespace cinemania::viewport_capture_hooks
{
void prepare(void* image_base) noexcept
{
    // 009a4631 is the shared tail after the complete capture block.
    cinemania_capture_handled = static_cast<std::byte*>(image_base) + 0x5a4631;
}
} // namespace cinemania::viewport_capture_hooks

// At 009a44bc and 009a4649 ESI is the viewport. At both function entries
// ECX is the this pointer. Each bridge saves EFLAGS and all GPRs, allocates
// a 16-byte aligned 512-byte FXSAVE area, and restores it before continuing.
// PUSHAD's ignored saved-ESP slot stores the capture decision; POPAD does not
// restore that slot. Both continuations restore the original flags and stack.
extern "C" __declspec(naked) void cinemania_capture_bridge()
{
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        cld
        push esi
        call cinemania_capture_callback
        add esp, 4
        mov [ebx + 12], eax
        fxrstor [esp]
        mov esp, ebx
        cmp dword ptr [esp + 12], 0
        jne handled
        popad
        popfd
        jmp dword ptr [cinemania_capture_original]
    handled:
        popad
        popfd
        jmp dword ptr [cinemania_capture_handled]
    }
}

extern "C" __declspec(naked) void cinemania_cleanup_bridge()
{
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        cld
        push esi
        call cinemania_cleanup_callback
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [cinemania_cleanup_original]
    }
}

extern "C" __declspec(naked) void cinemania_reset_bridge()
{
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        cld
        push ecx
        call cinemania_device_callback
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [cinemania_reset_original]
    }
}

extern "C" __declspec(naked) void cinemania_teardown_bridge()
{
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        cld
        push ecx
        call cinemania_device_callback
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [cinemania_teardown_original]
    }
}
