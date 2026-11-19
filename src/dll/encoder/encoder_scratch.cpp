#include "encoder_scratch.hpp"

#include <iterator>

#include <objbase.h>

namespace cinemania::dll
{
namespace
{
class FindOwner
{
public:
    explicit FindOwner(HANDLE value) : value_(value)
    {
    }

    ~FindOwner()
    {
        if (value_ != INVALID_HANDLE_VALUE)
        {
            FindClose(value_);
        }
    }

    FindOwner(const FindOwner&) = delete;
    FindOwner& operator=(const FindOwner&) = delete;

    HANDLE get() const
    {
        return value_;
    }

private:
    HANDLE value_;
};

bool owned_temp_name(const wchar_t* name)
{
    const std::wstring value(name);
    if (value.size() != 53 || value.substr(0, 12) != L".cinemania-{" ||
        value.substr(48) != L"}.tmp")
    {
        return false;
    }

    for (std::size_t index = 12; index < 48; ++index)
    {
        const wchar_t c = value[index];
        const std::size_t position = index - 12;
        if (position == 8 || position == 13 || position == 18 || position == 23)
        {
            if (c != L'-')
            {
                return false;
            }
        }
        else if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') ||
                   (c >= L'a' && c <= L'f')))
        {
            return false;
        }
    }

    return true;
}

DWORD remove_owned_file(const std::wstring& path)
{
    const ULONGLONG deadline = GetTickCount64() + 2000;
    HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

    for (;;)
    {
        if (DeleteFileW(path.c_str()))
        {
            if (timer)
            {
                CloseHandle(timer);
            }
            return ERROR_SUCCESS;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
        {
            if (timer)
            {
                CloseHandle(timer);
            }
            return ERROR_SUCCESS;
        }
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) || !timer ||
            GetTickCount64() >= deadline)
        {
            if (timer)
            {
                CloseHandle(timer);
            }
            return error;
        }

        LARGE_INTEGER due{};
        due.QuadPart = -1000000; // Retry after 100 ms while kernel handle rundown completes.
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer, 200) != WAIT_OBJECT_0)
        {
            CloseHandle(timer);
            return error;
        }
    }
}
} // namespace

EncoderScratch::~EncoderScratch()
{
    if (guard_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(guard_);
    }
}

DWORD EncoderScratch::prepare(const std::wstring& destination)
{
    const auto slash = destination.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return ERROR_INVALID_NAME;
    }

    GUID id{};
    const HRESULT result = CoCreateGuid(&id);
    if (FAILED(result))
    {
        return static_cast<DWORD>(result);
    }

    wchar_t text[40]{};
    if (!StringFromGUID2(id, text, static_cast<int>(std::size(text))))
    {
        return ERROR_INVALID_DATA;
    }

    path_ = destination.substr(0, slash + 1) + L".cinemania-session-" + text;
    if (!CreateDirectoryW(path_.c_str(), nullptr))
    {
        const DWORD error = GetLastError();
        path_.clear();
        return error;
    }

    owned_ = true;
    guard_ = CreateFileW(path_.c_str(),
                         FILE_READ_ATTRIBUTES | DELETE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
    if (guard_ == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        return error;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(guard_, &information))
    {
        return GetLastError();
    }
    if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        return ERROR_INVALID_NAME;
    }

    return ERROR_SUCCESS;
}

DWORD EncoderScratch::cleanup() noexcept
{
    try
    {
        if (!owned_)
        {
            return ERROR_SUCCESS;
        }
        if (guard_ == INVALID_HANDLE_VALUE)
        {
            return ERROR_INVALID_HANDLE;
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(guard_, &information))
        {
            return GetLastError();
        }
        if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            return ERROR_INVALID_NAME;
        }

        DWORD error = ERROR_SUCCESS;

        {
            WIN32_FIND_DATAW entry{};
            FindOwner find(FindFirstFileW((path_ + L"\\*").c_str(), &entry));
            if (find.get() == INVALID_HANDLE_VALUE)
            {
                error = GetLastError();
            }
            else
            {
                do
                {
                    if (owned_temp_name(entry.cFileName) &&
                        !(entry.dwFileAttributes &
                          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                    {
                        const DWORD deletion = remove_owned_file(path_ + L"\\" + entry.cFileName);
                        if (deletion != ERROR_SUCCESS && error == ERROR_SUCCESS)
                        {
                            error = deletion;
                        }
                    }
                } while (FindNextFileW(find.get(), &entry));
                const DWORD enumeration = GetLastError();
                if (enumeration != ERROR_NO_MORE_FILES && error == ERROR_SUCCESS)
                {
                    error = enumeration;
                }
            }
        }

        if (error != ERROR_SUCCESS)
        {
            return error;
        }

        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(
                guard_, FileDispositionInfo, &disposition, sizeof(disposition)))
        {
            return GetLastError();
        }

        CloseHandle(guard_);
        guard_ = INVALID_HANDLE_VALUE;
        owned_ = false;
        path_.clear();

        return ERROR_SUCCESS;
    }
    catch (...)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}

const std::wstring& EncoderScratch::path() const
{
    return path_;
}
} // namespace cinemania::dll
