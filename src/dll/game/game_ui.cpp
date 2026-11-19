#include "game_ui.hpp"

#include "game_abi.hpp"

#include <cstdint>
#include <string_view>

namespace cinemania::game_ui
{
namespace
{
// TMUF 2.12.0 CGameApp and its native FrameMessage entry.
constexpr std::size_t kFrameManagerOffset = 0x110;
constexpr std::size_t kShowDialogRva = 0x209910;

using game_abi::WideStringView;
using ShowDialog = void(__thiscall*)(void*, const WideStringView*, const WideStringView*, int, int);
} // namespace

void show_message(const unsigned char* image, void* app, const wchar_t* message) noexcept
{
    if (app == nullptr)
    {
        return;
    }

    void* manager = game_abi::field<void*>(app, kFrameManagerOffset);
    if (manager == nullptr)
    {
        return;
    }

    constexpr WideStringView ok{2, L"Ok"};
    const WideStringView body{static_cast<std::uint32_t>(std::wstring_view(message).size()),
                              message};
    game_abi::function_at<ShowDialog>(image, kShowDialogRva)(manager, &body, &ok, 0, 0);
}
} // namespace cinemania::game_ui
