#include "hd_replay_menu.hpp"

#include "game_abi.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include <intrin.h>

namespace cinemania::hd_replay_menu
{
namespace
{
// TMUF 2.12.0 Replay Operations template, card descriptor, and file chooser layout.
constexpr std::size_t kOpenMenuRva = 0xc8090;
constexpr std::size_t kMenuExceptionRva = 0x68d218;
constexpr std::size_t kUnusedCardCallEndRva = 0xc835b;
constexpr std::size_t kShowControlRva = 0x368a30;
constexpr std::size_t kSetLabelRva = 0x369030;
constexpr std::size_t kSetActionRva = 0x1a0010;
constexpr std::size_t kCardVtableRva = 0x7738ec;
constexpr std::size_t kGridVtableRva = 0x79328c;
constexpr std::size_t kLabelVtableRva = 0x793614;
constexpr std::size_t kButtonVtableRva = 0x794594;
constexpr std::size_t kFileIdVtableRva = 0x7301bc;
constexpr std::size_t kFolderVtableRva = 0x72ffb4;
constexpr std::size_t kRootFolderVtableRva = 0x73067c;
constexpr std::size_t kParentControlOffset = 0x6c;
constexpr std::size_t kChildCountOffset = 0x144;
constexpr std::size_t kChildrenOffset = 0x148;
constexpr std::size_t kCellCountOffset = 0x158;
constexpr std::size_t kCellsOffset = 0x15c;
constexpr std::size_t kCardKindOffset = 0x178;
constexpr std::size_t kCardLabelOffset = 0x1a8;
constexpr std::size_t kCardDescriptorOffset = 0x1cc;
constexpr std::size_t kSelectionCountOffset = 0x710;
constexpr std::size_t kSelectionItemsOffset = 0x714;
constexpr std::size_t kInfoFileIdOffset = 0x14;
constexpr std::size_t kFileParentOffset = 0x14;
constexpr std::size_t kFileNameOffset = 0x74;
constexpr std::size_t kFolderNameOffset = 0x40;

using game_abi::field;
using game_abi::function_at;
using game_abi::WideStringView;
using ShowControl = void(__cdecl*)(void*, int);
using OpenMenu = void(__thiscall*)(void*, void**);

struct GridCell
{
    float column;
    float row;
    float depth;
};

static_assert(sizeof(GridCell) == 12);

const unsigned char* game_image = nullptr;
ShowControl original_show = nullptr;
OpenMenu original_menu = nullptr;
RecordAction record_action = nullptr;
DiscardPending discard_pending_action = nullptr;
// The stock menu opens and reveals the card synchronously on the UI thread.
void* opening_card = nullptr;

bool has_vtable(const void* object, std::size_t vtable_rva) noexcept
{
    return game_abi::has_vtable(object, game_image + vtable_rva);
}

void* card_label(void* card) noexcept
{
    if (field<unsigned int>(card, kChildCountOffset) < 2)
    {
        return nullptr;
    }

    void** children = field<void**>(card, kChildrenOffset);
    if (children == nullptr || !has_vtable(children[0], kLabelVtableRva) ||
        !has_vtable(children[1], kButtonVtableRva))
    {
        return nullptr;
    }
    return children[0];
}

void put_recording_first(void* card) noexcept
{
    void* grid = field<void*>(card, kParentControlOffset);
    constexpr unsigned int count = 8;
    constexpr unsigned int slot = 4;
    if (!has_vtable(grid, kGridVtableRva) ||
        field<unsigned int>(grid, kChildCountOffset) != count ||
        field<unsigned int>(grid, kCellCountOffset) != count)
    {
        return;
    }

    void** children = field<void**>(grid, kChildrenOffset);
    auto* cells = field<GridCell*>(grid, kCellsOffset);
    if (children == nullptr || children[slot] != card || cells == nullptr)
    {
        return;
    }

    // The stock template has one row per child. Rotate its first five rows once.
    for (unsigned int i = 0; i < count; ++i)
    {
        if (cells[i].column != 0.0f || cells[i].row != static_cast<float>(i))
        {
            return;
        }
    }

    for (unsigned int i = 0; i < slot; ++i)
    {
        cells[i].row += 1.0f;
    }
    cells[slot].row = 0.0f;
}

void __cdecl show_control(void* control, int visible) noexcept
{
    // Reuse the hidden Export Challenge and Replay card to keep the native layout and navigation.
    if (_ReturnAddress() == game_image + kUnusedCardCallEndRva && visible == 0 &&
        has_vtable(control, kCardVtableRva) && field<unsigned int>(control, kCardKindOffset) != 0 &&
        field<void*>(control, kCardDescriptorOffset) != nullptr && card_label(control) != nullptr)
    {
        put_recording_first(control);
        opening_card = control;
        visible = 1;
    }

    original_show(control, visible);
}

void __fastcall open_menu(void* menu, void*, void** fiber)
{
    const bool opening = *fiber == nullptr;
    original_menu(menu, fiber);
    if (!opening)
    {
        return;
    }

    void* card = std::exchange(opening_card, nullptr);
    if (card == nullptr || *fiber == nullptr || game_abi::fiber_finished(*fiber))
    {
        return;
    }

    discard_pending_action();
    void* label_control = card_label(card);
    if (label_control == nullptr)
    {
        original_show(card, 0);
        return;
    }

    // Assign through the descriptor setter so native layout refreshes retain the label.
    constexpr std::wstring_view label = L"Record HD Replay";
    const WideStringView text{static_cast<std::uint32_t>(label.size()), label.data()};
    using SetLabel = void(__cdecl*)(void*, const WideStringView*);
    using SetAction = void(__thiscall*)(void*, void*, void*);
    function_at<SetLabel>(game_image, kSetLabelRva)(card, &text);
    field<void*>(card, kCardLabelOffset) = label_control;
    function_at<SetAction>(game_image, kSetActionRva)(card, menu, function_address(record_action));
}

std::wstring_view game_string(const void* object, std::size_t offset)
{
    const auto& value = field<WideStringView>(object, offset);
    if (value.length == 0 || value.length >= 32767 || value.text == nullptr)
    {
        return {};
    }

    const std::wstring_view text(value.text, value.length);
    return text.find(L'\0') == std::wstring_view::npos ? text : std::wstring_view{};
}

bool is_absolute_root(std::wstring_view path)
{
    return path.starts_with(L"\\\\") ||
           (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'));
}
} // namespace

void prepare(unsigned char* image,
             std::span<EntryHook, kHookCount> entries,
             RecordAction record,
             DiscardPending discard_pending) noexcept
{
    static constexpr std::array<unsigned char, 7> show_signature{
        0x56, 0x8b, 0x74, 0x24, 0x08, 0x85, 0xf6};
    static game_abi::SehPrologue menu_signature;
    menu_signature = game_abi::seh_prologue(image + kMenuExceptionRva);

    game_image = image;
    record_action = record;
    discard_pending_action = discard_pending;
    entries[0] = make_entry_hook(
        image + kShowControlRva, show_control, show_signature, original_address(original_show));
    entries[1] = make_entry_hook(
        image + kOpenMenuRva, open_menu, menu_signature, original_address(original_menu));
}

std::wstring selected_path(void* menu)
{
    // The selected CGameCtnReplayRecordInfo points to the chooser's exact file ID.
    if (field<std::uint32_t>(menu, kSelectionCountOffset) != 1)
    {
        return {};
    }

    void** selection = field<void**>(menu, kSelectionItemsOffset);
    if (selection == nullptr || *selection == nullptr)
    {
        return {};
    }

    void* file_id = field<void*>(*selection, kInfoFileIdOffset);
    if (!has_vtable(file_id, kFileIdVtableRva))
    {
        return {};
    }

    std::wstring relative(game_string(file_id, kFileNameOffset));
    if (relative.empty() || relative.find_first_of(L"\\/:") != std::wstring::npos)
    {
        return {};
    }

    void* folder = field<void*>(file_id, kFileParentOffset);
    for (unsigned int depth = 0; depth < 64 && folder != nullptr; ++depth)
    {
        const auto name = game_string(folder, kFolderNameOffset);
        if (has_vtable(folder, kRootFolderVtableRva))
        {
            return is_absolute_root(name) ? std::wstring(name) + L"\\" + relative : std::wstring{};
        }

        if (!has_vtable(folder, kFolderVtableRva) || name.empty() || name == L"." ||
            name == L".." || name.find_first_of(L"\\/:") != std::wstring_view::npos)
        {
            return {};
        }

        relative.insert(0, L"\\");
        relative.insert(0, name);
        folder = field<void*>(folder, kFileParentOffset);
    }
    return {};
}
} // namespace cinemania::hd_replay_menu
