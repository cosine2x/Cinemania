#pragma once

#include "hook_transaction.hpp"
#include <array>
#include <memory>

namespace cinemania::game_export::detail
{
struct Session;
}

namespace cinemania::export_volume
{
using Session = game_export::detail::Session;

// The gameplay port and router context remain borrowed only until deactivate.
[[nodiscard]] bool
activate(const std::shared_ptr<Session>& session, void* port, void* context) noexcept;
void deactivate(const std::shared_ptr<Session>& session) noexcept;
int classify(void* port, bool sound) noexcept;
void source_gain(unsigned int source, int parameter, float value, const void* caller) noexcept;
[[nodiscard]] bool prepare(unsigned char* game,
                           std::size_t size,
                           std::array<EntryHook, 2>& entries,
                           std::array<ImportHook, 2>& imports) noexcept;
} // namespace cinemania::export_volume
