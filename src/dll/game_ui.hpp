#pragma once

#include <cstddef>

namespace cinemania::game_ui
{
// Public localizer entry in TMUF 2.12.0. Other mods can hook this entry.
inline constexpr std::size_t kLocalizerRva = 0x502460;
using Localize = const wchar_t*(__thiscall*)(void*, const wchar_t*);

// Called on the game thread. TrackMania copies the text into its native dialog.
void show_message(const unsigned char* image, void* app, const wchar_t* message) noexcept;
} // namespace cinemania::game_ui
