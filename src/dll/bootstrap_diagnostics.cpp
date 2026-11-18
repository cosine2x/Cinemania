#include "bootstrap_diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

// clang-format off
// Windows types must precede the shell declarations.
#include <windows.h>
#include <shlobj.h>
// clang-format on

namespace cinemania
{
namespace
{
const char* error_domain_name(ErrorDomain domain)
{
    switch (domain)
    {
        case ErrorDomain::none:
            return "none";
        case ErrorDomain::win32:
            return "Win32";
        case ErrorDomain::ntstatus:
            return "NTSTATUS";
    }
    return "unknown";
}

const char* hook_domain_name(HookErrorDomain domain)
{
    switch (domain)
    {
        case HookErrorDomain::none:
            return "none";
        case HookErrorDomain::validation:
            return "validation";
        case HookErrorDomain::win32:
            return "Win32";
        case HookErrorDomain::minhook:
            return "MinHook";
    }
    return "unknown";
}

enum class LogState
{
    uninitialized,
    disabled,
    failed,
    ready
};

// The handle and lock intentionally live until process exit; the DLL is pinned.
// Keep the handle even after truncate or write failure to prevent another owner.
SRWLOCK log_lock = SRWLOCK_INIT;
std::atomic<LogState> log_state{LogState::uninitialized};
HANDLE log_handle = INVALID_HANDLE_VALUE;

class LastErrorGuard
{
public:
    LastErrorGuard() : error_(GetLastError())
    {
    }

    ~LastErrorGuard()
    {
        SetLastError(error_);
    }

private:
    DWORD error_;
};

LogState open_log()
{
    PWSTR local_app_data = nullptr;
    const HRESULT folder_result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(local_app_data, &CoTaskMemFree);
    if (FAILED(folder_result))
    {
        return LogState::failed;
    }

    const std::wstring directory = std::wstring(folder.get()) + L"\\Cinemania";
    const std::wstring logs = directory + L"\\logs";
    if ((!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        (!CreateDirectoryW(logs.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS))
    {
        return LogState::failed;
    }

    const std::wstring path = logs + L"\\Cinemania.log";
    log_handle = CreateFileW(path.c_str(),
                             GENERIC_WRITE,
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (log_handle == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_SHARING_VIOLATION)
        {
            return LogState::disabled;
        }
        return LogState::failed;
    }

    if (!SetFilePointerEx(log_handle, LARGE_INTEGER{}, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(log_handle))
    {
        return LogState::failed;
    }
    return LogState::ready;
}

LogState write_log(std::string_view message) noexcept
{
    LogState state = log_state.load();
    if (state != LogState::ready || message.empty())
    {
        return state;
    }

    AcquireSRWLockExclusive(&log_lock);
    state = log_state.load();
    if (state == LogState::ready)
    {
        std::size_t offset = 0;
        while (offset < message.size())
        {
            const std::size_t remaining = message.size() - offset;
            const DWORD chunk =
                static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(MAXDWORD)));
            DWORD written = 0;
            if (!WriteFile(log_handle, message.data() + offset, chunk, &written, nullptr) ||
                written == 0 || written > chunk)
            {
                state = LogState::failed;
                log_state.store(state);
                break;
            }

            offset += written;
        }
    }

    ReleaseSRWLockExclusive(&log_lock);
    return state;
}
} // namespace

void initialize_diagnostics() noexcept
{
    LastErrorGuard preserve_error;
    AcquireSRWLockExclusive(&log_lock);
    if (log_state.load() != LogState::uninitialized)
    {
        ReleaseSRWLockExclusive(&log_lock);
        return;
    }

    LogState state = LogState::failed;
    try
    {
        state = open_log();
    }
    catch (...)
    {
        // Diagnostics cannot unwind across the bootstrap thread boundary.
        state = LogState::failed;
    }

    log_state.store(state);
    ReleaseSRWLockExclusive(&log_lock);
}

void append_diagnostic(std::string_view message) noexcept
{
    LastErrorGuard preserve_error;
    if (message.empty())
    {
        return;
    }

    if (write_log(message) == LogState::failed)
    {
        OutputDebugStringA("Cinemania: could not append a diagnostic.\n");
    }
}

void report_bootstrap(const CheckResult& result, const HookResult& hooks)
{
    LastErrorGuard preserve_error;
    char hash_text[65] = "unavailable";
    if (result.has_hash)
    {
        constexpr char digits[] = "0123456789abcdef";
        for (std::size_t index = 0; index < result.hash.size(); ++index)
        {
            hash_text[index * 2] = digits[result.hash[index] >> 4];
            hash_text[index * 2 + 1] = digits[result.hash[index] & 15];
        }
        hash_text[64] = '\0';
    }

    const char* outcome = "check failure";
    if (result.outcome == CheckOutcome::compatible)
    {
        outcome = hooks.success ? "compatible hooks installed" : "compatible hooks failed";
    }
    else if (result.outcome == CheckOutcome::unsupported)
    {
        outcome = "unsupported";
    }

    char message[512]{};
    const int length =
        sprintf_s(message,
                  "Cinemania: %s; SHA256=%s; stage=%s; error domain=%s; code=0x%08lX; hooks=%u; "
                  "hook stage=%s; hook domain=%s; hook code=0x%08lX; residual=%u\r\n",
                  outcome,
                  hash_text,
                  result.stage,
                  error_domain_name(result.domain),
                  result.code,
                  hooks.installed,
                  hooks.stage,
                  hook_domain_name(hooks.domain),
                  hooks.code,
                  hooks.residual ? 1U : 0U);
    if (length <= 0)
    {
        OutputDebugStringA("Cinemania: diagnostic formatting failed\n");
        return;
    }

    if (write_log(std::string_view(message, static_cast<std::size_t>(length))) == LogState::failed)
    {
        OutputDebugStringA(message);
    }
}
} // namespace cinemania
