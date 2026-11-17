#pragma once

#include <array>

namespace cinemania
{
enum class CheckOutcome
{
    failure,
    compatible,
    unsupported
};

enum class ErrorDomain
{
    none,
    win32,
    ntstatus
};

struct CheckResult
{
    CheckOutcome outcome = CheckOutcome::failure;
    const char* stage = "unknown";
    ErrorDomain domain = ErrorDomain::none;
    unsigned long code = 0;
    std::array<unsigned char, 32> hash{};
    bool has_hash = false;
};

[[nodiscard]] CheckResult check_executable();
} // namespace cinemania
