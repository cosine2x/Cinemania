#pragma once

#include "executable_check.hpp"
#include "passthrough_hooks.hpp"

#include <string_view>

namespace cinemania
{
void report_bootstrap(const CheckResult& result, const HookResult& hooks);
void append_diagnostic(std::string_view message) noexcept;
} // namespace cinemania
