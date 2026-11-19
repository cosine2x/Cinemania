#pragma once

#include "game/hook_transaction.hpp"

#include <cstddef>
#include <span>

namespace cinemania::hd_replay
{
inline constexpr std::size_t kHookCount = 7;
[[nodiscard]] bool prepare(unsigned char* game, std::span<EntryHook, kHookCount> entries) noexcept;
} // namespace cinemania::hd_replay
