#include "worker_session.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

namespace
{
bool parse_number(std::wstring_view text, std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }
    value = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    return value != 0;
}

bool absolute_executable(const std::wstring& path)
{
    const bool drive =
        path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.size() >= 5 && path[0] == L'\\' && path[1] == L'\\';
    return drive || unc;
}

bool parse_options(int argc, wchar_t** argv, cinemania::worker::WorkerOptions& options)
{
    if (argc != 11 && argc != 13)
    {
        return false;
    }
    unsigned int seen = 0;
    for (int index = 1; index < argc; index += 2)
    {
        const std::wstring_view key(argv[index]);
        const std::wstring_view value(argv[index + 1]);
        unsigned int bit = 0;
        if (key == L"--ffmpeg")
        {
            bit = 1;
            options.ffmpeg = value;
        }
        else if (key == L"--scratch")
        {
            bit = 32;
            options.scratch = value;
        }
        else
        {
            std::uint64_t number = 0;
            if (!parse_number(value, number))
            {
                return false;
            }
            if (key == L"--session")
            {
                bit = 2;
                options.session = number;
            }
            else
            {
                if (number > std::numeric_limits<std::uintptr_t>::max())
                {
                    return false;
                }
                const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number));
                if (key == L"--data")
                {
                    bit = 4;
                    options.data = handle;
                }
                else if (key == L"--control")
                {
                    bit = 8;
                    options.control = handle;
                }
                else if (key == L"--status")
                {
                    bit = 16;
                    options.status = handle;
                }
            }
        }
        if (bit == 0 || (seen & bit) != 0)
        {
            return false;
        }
        seen |= bit;
    }
    return (seen == 31 || seen == 63) && absolute_executable(options.ffmpeg) &&
           (seen == 31 || absolute_executable(options.scratch)) &&
           options.data != options.control && options.data != options.status &&
           options.control != options.status && GetFileType(options.data) == FILE_TYPE_PIPE &&
           GetFileType(options.control) == FILE_TYPE_PIPE &&
           GetFileType(options.status) == FILE_TYPE_PIPE;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        cinemania::worker::WorkerOptions options;
        if (!parse_options(argc, argv, options))
        {
            return 1;
        }
        return cinemania::worker::run_session(options);
    }
    catch (...)
    {
        return 1;
    }
}
