#pragma once

#include <string>
#include <vector>

#include <windows.h>

namespace cinemania::worker
{
class OutputFiles
{
public:
    OutputFiles() = default;
    ~OutputFiles();
    OutputFiles(const OutputFiles&) = delete;
    OutputFiles& operator=(const OutputFiles&) = delete;

    [[nodiscard]] DWORD prepare(const std::string& destination_utf8, const std::wstring& scratch);
    [[nodiscard]] DWORD make_temporary(std::wstring& path);
    [[nodiscard]] DWORD publish(const std::wstring& candidate);
    [[nodiscard]] DWORD cleanup();
    [[nodiscard]] const std::wstring& destination() const;

private:
    std::wstring destination_;
    std::wstring directory_;
    HANDLE reservation_ = INVALID_HANDLE_VALUE;
    std::vector<std::wstring> temporary_paths_;
};
} // namespace cinemania::worker
