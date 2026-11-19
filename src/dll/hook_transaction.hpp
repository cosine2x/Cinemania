#pragma once

#include "passthrough_hooks.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace cinemania
{
struct EntryHook
{
    void* target;
    void* detour;
    void** original;
    const unsigned char* expected_bytes = nullptr;
    std::size_t expected_size = 0;
};

struct ImportHook
{
    void** slot;
    void* expected;
    void* replacement;
};

template <typename Function> void* function_address(Function function) noexcept
{
    // MinHook and the x86 game register executable addresses as untyped pointers.
    return reinterpret_cast<void*>(function);
}

template <typename Function> void** original_address(Function& function) noexcept
{
    // MinHook stores the trampoline in this x86 function pointer slot.
    return reinterpret_cast<void**>(&function);
}

// The signature must outlive the hook transaction.
template <typename Function, std::size_t Size>
EntryHook make_entry_hook(void* target,
                          Function detour,
                          const std::array<unsigned char, Size>& signature,
                          void** original = nullptr) noexcept
{
    return {target, function_address(detour), original, signature.data(), signature.size()};
}

// Caller validates all addresses and expected bytes before installation. After an activation
// attempt, MinHook and all prepared trampolines remain resident for process lifetime.
[[nodiscard]] HookResult install_hooks(std::span<EntryHook> entries, std::span<ImportHook> imports);
} // namespace cinemania
