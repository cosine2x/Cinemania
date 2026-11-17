#pragma once

#include "passthrough_hooks.hpp"

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

// Caller validates all addresses and expected bytes before installation. After an activation
// attempt, MinHook and all prepared trampolines remain resident for process lifetime.
[[nodiscard]] HookResult install_hooks(std::span<EntryHook> entries, std::span<ImportHook> imports);
} // namespace cinemania
