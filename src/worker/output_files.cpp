#include "output_files.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>

#include <objbase.h>

namespace cinemania::worker
{
namespace
{
bool is_absolute_path(const std::wstring& path)
{
    const bool drive = path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' &&
                       (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.size() >= 5 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' &&
                     path[2] != L'.';
    return drive || unc;
}

bool has_mp4_extension(const std::wstring& path)
{
    if (path.size() < 4)
    {
        return false;
    }
    std::wstring extension = path.substr(path.size() - 4);
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](wchar_t character)
                   {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return extension == L".mp4";
}

DWORD utf8_path(const std::string& source, std::wstring& destination)
{
    if (source.empty() || source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return ERROR_INVALID_NAME;
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0);
    if (length == 0)
    {
        return GetLastError();
    }
    destination.resize(static_cast<std::size_t>(length));
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            source.data(),
                            static_cast<int>(source.size()),
                            destination.data(),
                            length) != length)
    {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}
} // namespace

OutputFiles::~OutputFiles()
{
    static_cast<void>(cleanup());
}

DWORD OutputFiles::cleanup()
{
    DWORD first_error = ERROR_SUCCESS;
    for (const auto& path : temporary_paths_)
    {
        if (!DeleteFileW(path.c_str()))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && first_error == ERROR_SUCCESS)
            {
                first_error = error;
            }
        }
    }
    temporary_paths_.clear();
    if (reservation_ != INVALID_HANDLE_VALUE)
    {
        if (!CloseHandle(reservation_) && first_error == ERROR_SUCCESS)
        {
            first_error = GetLastError();
        }
        reservation_ = INVALID_HANDLE_VALUE;
    }
    return first_error;
}

DWORD OutputFiles::prepare(const std::string& destination_utf8)
{
    DWORD error = utf8_path(destination_utf8, destination_);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }
    if (!is_absolute_path(destination_) || !has_mp4_extension(destination_))
    {
        return ERROR_INVALID_NAME;
    }
    const auto slash = destination_.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 == destination_.size())
    {
        return ERROR_INVALID_NAME;
    }
    directory_ = destination_.substr(0, slash + 1);
    const std::wstring reservation_path = destination_ + L".cinemania-reservation";
    reservation_ = CreateFileW(reservation_path.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                               nullptr);
    if (reservation_ == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    const DWORD attributes = GetFileAttributesW(destination_.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return ERROR_FILE_EXISTS;
    }
    error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND)
    {
        return error;
    }
    return ERROR_SUCCESS;
}

DWORD OutputFiles::make_temporary(std::wstring& path)
{
    GUID identifier{};
    const HRESULT result = CoCreateGuid(&identifier);
    if (FAILED(result))
    {
        return static_cast<DWORD>(result);
    }
    wchar_t guid[40]{};
    if (StringFromGUID2(identifier, guid, static_cast<int>(std::size(guid))) == 0)
    {
        return ERROR_INVALID_DATA;
    }
    path = directory_ + L".cinemania-" + guid + L".tmp";
    temporary_paths_.push_back(path);
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        temporary_paths_.pop_back();
        return error;
    }
    CloseHandle(file);
    return ERROR_SUCCESS;
}

DWORD OutputFiles::publish(const std::wstring& candidate)
{
    if (!MoveFileExW(candidate.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

const std::wstring& OutputFiles::destination() const
{
    return destination_;
}
} // namespace cinemania::worker
