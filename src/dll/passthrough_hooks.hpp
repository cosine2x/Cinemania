#pragma once

namespace cinemania
{
enum class HookErrorDomain
{
    none,
    validation,
    win32,
    minhook
};

struct HookResult
{
    HookErrorDomain domain = HookErrorDomain::none;
    const char* stage = "not attempted";
    unsigned long code = 0;
    unsigned int installed = 0;
    bool success = false;
    bool residual = false;
};

[[nodiscard]] HookResult install_passthrough_hooks();
} // namespace cinemania
