#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cinemania::hd_replay
{
inline constexpr std::uint32_t kSamplePeriodMs = 10;
inline constexpr std::uint32_t kMaxStateBytes = 0x430440;

struct SampleLayout
{
    std::uint32_t vehicle_class;
    std::uint32_t version;
    std::uint32_t period_ms;
    std::uint32_t count;
};

struct SourceFile
{
    std::wstring destination;
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> sample_header;
    std::size_t body_start = 0;
    std::size_t sample_start = 0;
    std::size_t sample_end = 0;
    std::uint32_t target_samples = 0;
    bool compressed = false;
};

[[nodiscard]] const wchar_t* open_source(unsigned char* game,
                                         const std::wstring& path,
                                         const SampleLayout& layout,
                                         SourceFile& source);
[[nodiscard]] const wchar_t* save_replay(unsigned char* game,
                                         const SourceFile& source,
                                         std::span<const std::uint8_t> states,
                                         std::span<const std::uint32_t> offsets);
} // namespace cinemania::hd_replay
