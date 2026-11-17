#pragma once

#include <windows.h>

namespace cinemania
{
class FileHandle
{
public:
    explicit FileHandle(HANDLE handle) : handle_(handle)
    {
    }

    ~FileHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    HANDLE get() const
    {
        return handle_;
    }

private:
    HANDLE handle_;
};

} // namespace cinemania
