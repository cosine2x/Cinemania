#include "bootstrap_diagnostics.hpp"
#include "win32_file.hpp"

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

bool write_log(const char* message, int length)
{
    PWSTR local_app_data = nullptr;
    const HRESULT folder_result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(local_app_data, &CoTaskMemFree);
    if (FAILED(folder_result))
    {
        return false;
    }

    const std::wstring base(folder.get());
    const std::wstring directory = base + L"\\Cinemania";
    const std::wstring logs = directory + L"\\logs";
    if ((!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        (!CreateDirectoryW(logs.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS))
    {
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    {
        return false;
    }

    wchar_t name[100]{};
    if (swprintf_s(name,
                   L"\\%lu-%08lX%08lX.log",
                   GetCurrentProcessId(),
                   creation.dwHighDateTime,
                   creation.dwLowDateTime) < 0)
    {
        return false;
    }

    const std::wstring path = logs + name;
    FileHandle log(CreateFileW(path.c_str(),
                               GENERIC_WRITE,
                               FILE_SHARE_READ,
                               nullptr,
                               CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr));
    DWORD written = 0;
    if (log.get() == INVALID_HANDLE_VALUE ||
        !WriteFile(log.get(), message, static_cast<DWORD>(length), &written, nullptr) ||
        written != static_cast<DWORD>(length))
    {
        return false;
    }

    return true;
}
} // namespace

void report_bootstrap(const CheckResult& result)
{
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
        outcome = "compatible load-only";
    }
    else if (result.outcome == CheckOutcome::unsupported)
    {
        outcome = "unsupported";
    }

    char message[512]{};
    const int length =
        sprintf_s(message,
                  "Cinemania: %s; SHA256=%s; stage=%s; error domain=%s; code=0x%08lX\r\n",
                  outcome,
                  hash_text,
                  result.stage,
                  error_domain_name(result.domain),
                  result.code);
    if (length <= 0)
    {
        OutputDebugStringA("Cinemania: diagnostic formatting failed\n");
        return;
    }

    if (!write_log(message, length))
    {
        OutputDebugStringA(message);
    }
}
} // namespace cinemania
