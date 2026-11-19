#pragma once

namespace cinemania::readback
{
// Called only at the verified capture, pre-cleanup and device lifecycle seams.
[[nodiscard]] bool capture(void* viewport) noexcept;
void cleanup(void* viewport) noexcept;
void device_change(void* viewport) noexcept;
void enable() noexcept;
} // namespace cinemania::readback
