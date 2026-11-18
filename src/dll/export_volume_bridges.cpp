#include "export_volume.hpp"

extern "C"
{
    extern void* cinemania_sound_original;
    extern void* cinemania_music_original;
    extern void* cinemania_sound_continuation;
    extern void* cinemania_sound_coefficient;

    int __cdecl cinemania_sound_classify(void* port) noexcept
    {
        return cinemania::export_volume::classify(port, true);
    }

    void __cdecl cinemania_music_classify(void* port) noexcept
    {
        cinemania::export_volume::classify(port, false);
    }
}

// EBP is the port at both verified mid-function seams. FXSAVE protects the
// live x87 stack and SSE state while the policy callback runs. PUSHAD's saved
// ESP slot stores the decision; POPAD ignores that slot. The override performs
// exactly the displaced x87 operation with unity in place of SoundVolume.
extern "C" __declspec(naked) void cinemania_sound_bridge()
{
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        cld
        push ebp
        call cinemania_sound_classify
        add esp, 4
        mov [ebx + 12], eax
        fxrstor [esp]
        mov esp, ebx
        cmp dword ptr [esp + 12], 0
        jne unity
        popad
        popfd
        jmp dword ptr [cinemania_sound_original]
    unity:
        popad
        popfd
        fld1
        push eax
        mov eax, dword ptr [cinemania_sound_coefficient]
        fmul qword ptr [eax]
        pop eax
        jmp dword ptr [cinemania_sound_continuation]
    }
}

extern "C" __declspec(naked) void cinemania_music_bridge()
{
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        cld
        push ebp
        call cinemania_music_classify
        add esp, 4
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [cinemania_music_original]
    }
}
