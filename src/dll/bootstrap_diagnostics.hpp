#pragma once

#include "executable_check.hpp"
#include "passthrough_hooks.hpp"

#include <string_view>

namespace cinemania
{
// Initialize before installing hooks; a failed contender never retries ownership.
void initialize_diagnostics() noexcept;
void report_bootstrap(const CheckResult& result, const HookResult& hooks);
void append_diagnostic(std::string_view message) noexcept;
} // namespace cinemania
