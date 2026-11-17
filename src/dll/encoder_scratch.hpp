#pragma once

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace cinemania::dll
{
// Owns the directory identity guard. Cleanup must only run after the child job is empty.
class EncoderScratch
{
public:
    EncoderScratch() = default;
    ~EncoderScratch();
    EncoderScratch(const EncoderScratch&) = delete;
    EncoderScratch& operator=(const EncoderScratch&) = delete;

    [[nodiscard]] DWORD prepare(const std::wstring& destination);
    [[nodiscard]] DWORD cleanup() noexcept;
    [[nodiscard]] const std::wstring& path() const;

private:
    std::wstring path_;
    HANDLE guard_ = INVALID_HANDLE_VALUE;
    bool owned_ = false;
};
} // namespace cinemania::dll
