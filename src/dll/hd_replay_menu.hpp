#pragma once

#include "hook_transaction.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace cinemania::hd_replay_menu
{
inline constexpr std::size_t kHookCount = 2;

using RecordAction = void(__fastcall*)(void*, void*) noexcept;
using DiscardPending = void (*)() noexcept;

// The hooks run on the game thread. The record action uses the native card callback ABI.
void prepare(unsigned char* image,
             std::span<EntryHook, kHookCount> entries,
             RecordAction record,
             DiscardPending discard_pending) noexcept;

// Returns the chooser's exact file path, or an empty string for unsupported selections.
[[nodiscard]] std::wstring selected_path(void* menu);
} // namespace cinemania::hd_replay_menu
