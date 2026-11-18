#pragma once

#include "hook_transaction.hpp"

#include <array>

namespace cinemania::openal_capture_hooks
{
// Prepare the pinned mixer hook before the game's audio clock starts.
[[nodiscard]] bool prepare_mixer() noexcept;

// Called as part of the game hook transaction after executable validation.
bool prepare(unsigned char* game,
             std::size_t image_size,
             EntryHook& entry,
             std::array<ImportHook, 7>& imports) noexcept;
} // namespace cinemania::openal_capture_hooks
