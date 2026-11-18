#pragma once

#include <cstddef>

namespace cinemania::audio_capture::compat
{
// Pinned OpenAL router and wrapper object layouts, documented in
// research/m5/mixer-capture/ABI.md. Values are offsets, never live addresses.
inline constexpr std::size_t kPortDevice = 0x170;
inline constexpr std::size_t kPortContext = 0x174;
inline constexpr std::size_t kPortCapture = 0x178;
inline constexpr std::size_t kProxyContextDevice = 0x14;
inline constexpr std::size_t kProxyContextBackend = 0x154;
inline constexpr std::size_t kProxyDeviceBackend = 0x60;
inline constexpr std::size_t kMixContextFormat = 0x410;
inline constexpr std::size_t kMixContextDevice = 0x434;
} // namespace cinemania::audio_capture::compat
