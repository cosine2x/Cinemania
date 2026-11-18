#pragma once

#include <memory>

namespace cinemania::game_export::detail
{
struct Session;
}

namespace cinemania::native_render_ahead
{
void capture(void* viewport) noexcept;
void cleanup(void* viewport) noexcept;
void device_change(void* viewport) noexcept;
void retire(const std::shared_ptr<game_export::detail::Session>& session) noexcept;
} // namespace cinemania::native_render_ahead
