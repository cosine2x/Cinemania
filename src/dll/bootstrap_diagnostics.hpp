#pragma once

#include "executable_check.hpp"
#include "passthrough_hooks.hpp"

namespace cinemania
{
void report_bootstrap(const CheckResult& result, const HookResult& hooks);
} // namespace cinemania
