#include "hd_replay.hpp"

#include "hd_replay_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <intrin.h>
#include <memory>
#include <string_view>
#include <utility>

#include <windows.h>

namespace cinemania::hd_replay
{
namespace
{
// TMUF 2.12.0 replay operations and the CTrackManiaRace validation fiber.
constexpr unsigned int kValidateOperation = 0x38;
constexpr unsigned int kValidationRunning = 0x5b7;
constexpr unsigned int kRaceFinished = 2;
constexpr unsigned int kValidReplay = 1;
constexpr std::uint32_t kUnsetTime = 0xffffffff;
constexpr std::uint32_t kOutroDurationMs = 3000;
constexpr std::size_t kReplayValidation = 0x88760;
constexpr std::size_t kReplayValidationCall = 0x9c30b;
constexpr std::size_t kValidationSuccessTextCall = 0x88cf0;
constexpr std::size_t kRaceValidation = 0x82760;
constexpr std::size_t kReplayMenu = 0xc8090;
constexpr std::size_t kReplayMenuSlotCallEnd = 0xc835b;
constexpr std::size_t kShowControl = 0x368a30;
constexpr std::size_t kBeforeValidationCleanup = 0x82fa6;
constexpr std::size_t kYieldRaceValidation = 0x831b8;
constexpr std::size_t kRaceInputFinish = 0x7d3e3;
constexpr std::size_t kAutomaticBraking = 0x7d841;
constexpr std::size_t kRaceInputContinuation = 0x7d967;
constexpr std::size_t kGhostRecorderSize = 0x198;
constexpr ULONGLONG kTailTimeoutMs = 6000;

using ValidateReplay =
    void(__thiscall*)(void*, void**, void*, void*, unsigned int*, int, int, void*, int);
using ValidateRace = void(__thiscall*)(void*, void**, void*, unsigned int*, void*, int, int, int);
using ShowControl = void(__cdecl*)(void*, int);
using ReplayMenu = void(__thiscall*)(void*, void**);

unsigned char* game = nullptr;
ValidateRace original_race = nullptr;
ShowControl original_show = nullptr;
ReplayMenu original_menu = nullptr;
void* replay_call_trampoline = nullptr;
void* success_text_trampoline = nullptr;
void* original_cleanup = nullptr;
void* input_finish_trampoline = nullptr;
const void* replay_call_continuation = nullptr;
const void* success_text_continuation = nullptr;
const void* native_validation_body = nullptr;
const void* yield_validation = nullptr;
const void* automatic_braking = nullptr;
const void* race_input_continuation = nullptr;
void* opening_card = nullptr;

struct GameWideString
{
    std::uint32_t length;
    const wchar_t* text;
};

struct GridCell
{
    float column;
    float row;
    float depth;
};

struct Recording
{
    SourceFile source;
    SampleLayout layout{};
    std::uint32_t finish_time = kUnsetTime;
    std::uint32_t verified_finish = kUnsetTime;
    DWORD thread = 0;
    void* replay = nullptr;
    void** replay_fiber = nullptr;
    void** race_fiber = nullptr;
    void* race = nullptr;
    void* ghost = nullptr;
    std::uint32_t* period = nullptr;
    std::uint32_t previous_period = 0;
    ULONGLONG tail_deadline = 0;
    bool inside_validation = false;
    const wchar_t* error = nullptr;
    const wchar_t* completion = nullptr;
    std::vector<std::uint8_t> states;
    std::vector<std::uint32_t> offsets;
};

std::unique_ptr<Recording> recording;

template <typename T> T& field(void* object, std::size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

template <typename Function> Function at(std::size_t rva)
{
    return reinterpret_cast<Function>(game + rva);
}

bool is_type(void* object, std::size_t vtable)
{
    return object != nullptr && field<const void*>(object, 0) == game + vtable;
}

bool has_ended(void* fiber)
{
    return fiber == reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1));
}

std::uint32_t outro_sample_count(std::uint32_t finish) noexcept
{
    const auto end_ms = static_cast<std::uint64_t>(finish) + kOutroDurationMs;
    const auto count = (end_ms + kSamplePeriod - 1) / kSamplePeriod + 1;
    // The native recorder caps the packed vehicle state buffer.
    constexpr unsigned int min_state_bytes = 35;
    return count <= kMaxStateBytes / min_state_bytes ? static_cast<std::uint32_t>(count) : 0;
}

std::uint32_t phase(void* fiber)
{
    return fiber == nullptr || has_ended(fiber) ? kUnsetTime : field<std::uint32_t>(fiber, 4);
}

void show_message(void* app, const wchar_t* message) noexcept
{
    void* manager = app != nullptr ? field<void*>(app, 0x110) : nullptr;
    if (manager == nullptr)
    {
        return;
    }
    constexpr GameWideString ok{2, L"Ok"};
    const GameWideString body{static_cast<std::uint32_t>(std::wstring_view(message).size()),
                              message};
    using ShowDialog =
        void(__thiscall*)(void*, const GameWideString*, const GameWideString*, int, int);
    at<ShowDialog>(0x209910)(manager, &body, &ok, 0, 0);
}

std::wstring_view game_string(void* object, std::size_t offset)
{
    const auto length = field<std::uint32_t>(object, offset);
    const wchar_t* text = field<const wchar_t*>(object, offset + 4);
    if (length == 0 || length >= 32767 || text == nullptr)
    {
        return {};
    }
    const std::wstring_view value(text, length);
    return value.find(L'\0') == std::wstring_view::npos ? value : std::wstring_view{};
}

std::wstring replay_path(void* menu)
{
    // The selection is a CFastBuffer<CGameCtnReplayRecordInfo*>. Its file id
    // is associated with the exact item selected by the user in the chooser.
    if (field<std::uint32_t>(menu, 0x710) != 1)
    {
        return {};
    }
    void** selection = field<void**>(menu, 0x714);
    void* info = selection != nullptr ? *selection : nullptr;
    void* fid = info != nullptr ? field<void*>(info, 0x14) : nullptr;
    if (!is_type(fid, 0x7301bc))
    {
        return {};
    }
    std::wstring relative(game_string(fid, 0x74));
    if (relative.empty() || relative.find_first_of(L"\\/:") != std::wstring::npos)
    {
        return {};
    }

    void* folder = field<void*>(fid, 0x14);
    for (unsigned int depth = 0; depth < 64 && folder != nullptr; ++depth)
    {
        const auto name = game_string(folder, 0x40);
        if (is_type(folder, 0x73067c))
        {
            if (name.size() >= 3 && name[1] == L':' && (name[2] == L'\\' || name[2] == L'/'))
            {
                return std::wstring(name) + L"\\" + relative;
            }
            if (name.starts_with(L"\\\\"))
            {
                return std::wstring(name) + L"\\" + relative;
            }
            return {};
        }
        if (!is_type(folder, 0x72ffb4) || name.empty() || name == L"." || name == L".." ||
            name.find_first_of(L"\\/:") != std::wstring_view::npos)
        {
            return {};
        }
        relative.insert(0, L"\\");
        relative.insert(0, name);
        folder = field<void*>(folder, 0x14);
    }
    return {};
}

void __fastcall click(void* menu, void*) noexcept
{
    void* app = field<void*>(menu, 0x784);
    try
    {
        if (recording || field<unsigned int>(menu, 0x700) != 0)
        {
            return;
        }
        void* replay = app != nullptr ? field<void*>(app, 0x1d0) : nullptr;
        if (replay == nullptr || field<unsigned int>(replay, 0x18) != 1)
        {
            show_message(app, L"Select a replay containing one recorded car.");
            return;
        }
        void** ghosts = field<void**>(replay, 0x1c);
        void* source_ghost = ghosts != nullptr ? *ghosts : nullptr;
        if (!is_type(source_ghost, 0x7660e4) || field<std::uint32_t>(source_ghost, 0x10c) == 0)
        {
            show_message(app, L"This replay has no recorded inputs to validate.");
            return;
        }
        const auto path = replay_path(menu);
        if (path.empty())
        {
            show_message(app, L"Select a single replay saved on a local drive.");
            return;
        }

        auto task = std::make_unique<Recording>();
        task->thread = GetCurrentThreadId();
        task->replay = replay;
        task->finish_time = field<std::uint32_t>(source_ghost, 0xe8);
        if (task->finish_time != kUnsetTime && outro_sample_count(task->finish_time) == 0)
        {
            show_message(app, L"This replay is too long for the game's HD ghost recorder.");
            return;
        }
        task->layout = {field<std::uint32_t>(source_ghost, 0x4c),
                        field<std::uint32_t>(source_ghost, 0x48),
                        field<std::uint32_t>(source_ghost, 0x44),
                        field<std::uint32_t>(source_ghost, 0x50)};
        if (const wchar_t* error = open_source(game, path, task->layout, task->source))
        {
            show_message(app, error);
            return;
        }
        recording = std::move(task);
        // Dispatch the ordinary Validate event so TMInterface can select its
        // validation mode even though our call uses the stock validation fiber.
        using SelectValidate = void(__thiscall*)(void*);
        at<SelectValidate>(0xc6a40)(menu);
        if (field<unsigned int>(menu, 0x700) != kValidateOperation)
        {
            recording.reset();
            show_message(app, L"The game could not start replay validation.");
        }
    }
    catch (...)
    {
        show_message(app, L"Could not prepare the selected replay.");
    }
}

void put_recording_first(void* card) noexcept
{
    void* grid = field<void*>(card, 0x6c);
    constexpr unsigned int count = 8;
    constexpr unsigned int slot = 4;
    if (!is_type(grid, 0x79328c) || field<unsigned int>(grid, 0x144) != count ||
        field<unsigned int>(grid, 0x158) != count)
    {
        return;
    }
    void** children = field<void**>(grid, 0x148);
    auto* cells = field<GridCell*>(grid, 0x15c);
    if (children == nullptr || children[slot] != card || cells == nullptr)
    {
        return;
    }
    // The stock template uses one column with each child in its own row.
    // Leave unfamiliar layouts alone and only move the slot once per template.
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
    if (_ReturnAddress() == game + kReplayMenuSlotCallEnd && visible == 0 &&
        is_type(control, 0x7738ec) && field<unsigned int>(control, 0x144) >= 2 &&
        field<unsigned int>(control, 0x178) != 0 && field<void*>(control, 0x1cc) != nullptr)
    {
        // Use the unused card from the replay template to retain native
        // appearance, placement, and keyboard navigation.
        void** children = field<void**>(control, 0x148);
        if (children != nullptr && is_type(children[0], 0x793614) && is_type(children[1], 0x794594))
        {
            put_recording_first(control);
            opening_card = control;
            visible = 1;
        }
    }
    original_show(control, visible);
}

void __fastcall replay_menu(void* menu, void*, void** fiber)
{
    const bool opening = *fiber == nullptr;
    original_menu(menu, fiber);
    if (!opening)
    {
        return;
    }
    void* card = std::exchange(opening_card, nullptr);
    if (card == nullptr || *fiber == nullptr || has_ended(*fiber))
    {
        return;
    }
    if (recording && recording->replay_fiber == nullptr)
    {
        recording.reset();
    }
    void** children = field<void**>(card, 0x148);
    if (children == nullptr || !is_type(children[0], 0x793614) || !is_type(children[1], 0x794594))
    {
        original_show(card, 0);
        return;
    }
    // Use the native setter so subsequent layout refreshes also see the label.
    constexpr std::wstring_view label = L"Record HD Replay";
    const GameWideString text{static_cast<std::uint32_t>(label.size()), label.data()};
    using SetLabel = void(__cdecl*)(void*, const GameWideString*);
    at<SetLabel>(0x369030)(card, &text);
    field<void*>(card, 0x1a8) = children[0];
    using SetAction = void(__thiscall*)(void*, void*, void*);
    at<SetAction>(0x1a0010)(card, menu, reinterpret_cast<void*>(click));
}

void stop_recording(Recording& task) noexcept
{
    using Method = void(__thiscall*)(void*);
    if (task.ghost != nullptr)
    {
        at<Method>(0x276190)(task.ghost);
        at<Method>(0x524910)(task.ghost);
        task.ghost = nullptr;
    }
    if (task.period != nullptr)
    {
        if (*task.period == kSamplePeriod)
        {
            *task.period = task.previous_period;
        }
        task.period = nullptr;
    }
}

void start_recording(Recording& task, void* player_info)
{
    void* player = player_info != nullptr ? field<void*>(player_info, 0x238) : nullptr;
    void* vehicle = player != nullptr ? field<void*>(player, 0x28) : nullptr;
    if (vehicle == nullptr || field<void*>(vehicle, 0x14) == nullptr)
    {
        task.error = L"Validation did not create a car to record.";
        return;
    }
    void* command_core = field<void*>(game, 0x9731e0);
    if (command_core == nullptr)
    {
        task.error = L"The replay sample clock is unavailable.";
        return;
    }
    using Allocate = void*(__cdecl*)(std::size_t);
    using Construct = void*(__thiscall*)(void*);
    using AddRef = int(__thiscall*)(void*);
    using Start = void(__thiscall*)(void*, void*, std::uint32_t);
    void* memory = at<Allocate>(0x3079)(kGhostRecorderSize);
    if (memory == nullptr)
    {
        task.error = L"Could not allocate a car sample recorder.";
        return;
    }
    task.ghost = at<Construct>(0x1e0ad0)(memory);
    at<AddRef>(0x523e60)(task.ghost);
    task.period = &field<std::uint32_t>(command_core, 0xd0);
    task.previous_period = *task.period;
    *task.period = kSamplePeriod;
    at<Start>(0x1dfe90)(task.ghost, vehicle, field<std::uint32_t>(task.race, 0x314));
}

void copy_samples(Recording& task, std::uint32_t actual_finish)
{
    void* ghost = task.ghost;
    const std::uint32_t count = field<std::uint32_t>(ghost, 0x50);
    const std::uint32_t size = field<std::uint32_t>(ghost, 0x34);
    const auto* states = field<const std::uint8_t*>(ghost, 0x30);
    const auto* offsets = field<const std::uint32_t*>(ghost, 0x54);
    if (actual_finish == kUnsetTime ||
        (task.finish_time != kUnsetTime && actual_finish != task.finish_time) ||
        field<std::uint32_t>(ghost, 0x44) != kSamplePeriod ||
        field<std::uint32_t>(ghost, 0x48) != task.layout.version ||
        field<std::uint32_t>(ghost, 0x4c) != task.layout.vehicle_class ||
        field<std::uint32_t>(ghost, 0x68) != 1 || count < task.source.target_samples ||
        count > kMaxStateBytes || size > kMaxStateBytes || size == 0 || states == nullptr ||
        offsets == nullptr)
    {
        task.error =
            L"Validation did not capture the full 10 ms replay with the original finish time.";
        return;
    }
    task.states.assign(states, states + size);
    task.offsets.assign(offsets, offsets + count);
}

bool __cdecl defer_cleanup(void* race, void** fiber) noexcept
{
    Recording* task = recording.get();
    if (task == nullptr || task->thread != GetCurrentThreadId() || !task->inside_validation ||
        task->race != race || task->race_fiber != fiber || task->ghost == nullptr)
    {
        return false;
    }
    if (task->verified_finish == kUnsetTime)
    {
        // Preserve the native verdict at the finish. The race can transition
        // away from its finished state while we collect the playback tail.
        void* player = field<void*>(race, 0x330);
        if (field<unsigned int>(*fiber, 0x1c) != kValidReplay || player == nullptr ||
            field<unsigned int>(player, 0x314) != kRaceFinished)
        {
            return false;
        }
        const auto finish = field<std::uint32_t>(player, 0x2a8);
        if (finish == kUnsetTime ||
            (task->finish_time != kUnsetTime && finish != task->finish_time))
        {
            return false;
        }
        task->verified_finish = finish;
        task->source.target_samples = outro_sample_count(finish);
        if (task->source.target_samples == 0)
        {
            task->error = L"This replay is too long for the game's HD ghost recorder.";
            stop_recording(*task);
            return false;
        }
        task->tail_deadline = GetTickCount64() + kTailTimeoutMs;
    }
    if (field<std::uint32_t>(task->ghost, 0x50) < task->source.target_samples &&
        GetTickCount64() < task->tail_deadline)
    {
        field<unsigned int>(*fiber, 4) = kValidationRunning;
        return true;
    }
    try
    {
        copy_samples(*task, task->verified_finish);
    }
    catch (...)
    {
        task->error = L"Could not copy the recorded car samples.";
    }
    stop_recording(*task);
    field<unsigned int>(*fiber, 0x1c) = kValidReplay;
    return false;
}

bool __cdecl use_automatic_braking(void* race, void* player) noexcept
{
    const Recording* task = recording.get();
    if (task == nullptr || task->thread != GetCurrentThreadId() || task->race != race ||
        task->ghost == nullptr || field<void*>(race, 0x330) != player || player == nullptr)
    {
        return false;
    }
    return task->verified_finish != kUnsetTime ||
           field<unsigned int>(player, 0x314) == kRaceFinished;
}

// Stock validation has just restored the normal clock and zeroed EDI. Use
// its saved input time to run the ordinary race's automatic braking block.
__declspec(naked) void race_input_finish()
{
    __asm
    {
        pushfd
        pushad
        push esi
        push ebp
        call use_automatic_braking
        add esp, 8
        test al, al
        jnz brake
        popad
        popfd
        jmp dword ptr [race_input_continuation]
    brake:
        popad
        popfd
        mov edi, dword ptr [esp + 0x10]
        mov dword ptr [esp + 0x34], 0
        jmp dword ptr [automatic_braking]
    }
}

void __fastcall validate_race(void* race,
                              void*,
                              void** fiber,
                              void* inputs,
                              unsigned int* result,
                              void* message,
                              int notify,
                              int draw,
                              int options)
{
    Recording* task = recording.get();
    const bool active =
        task != nullptr && task->thread == GetCurrentThreadId() && task->inside_validation;
    if (active && *fiber == nullptr && task->race_fiber == nullptr)
    {
        task->race_fiber = fiber;
        task->race = race;
    }
    const bool ours = active && task->race_fiber == fiber && task->race == race;
    original_race(race, fiber, inputs, result, message, notify, draw, options);
    if (!ours)
    {
        return;
    }
    const auto current = phase(*fiber);
    if (current == kValidationRunning && task->ghost == nullptr && task->states.empty() &&
        task->error == nullptr)
    {
        start_recording(*task, field<void*>(race, 0x330));
    }
    else if (current != kValidationRunning)
    {
        stop_recording(*task);
    }
}

// TMInterface detours ReplayValidate's first six bytes. Recreate that pinned
// prologue for Cinemania's action and enter the checked stock continuation.
// The ordinary Validate command still takes the public game entry.
__declspec(naked) void stock_replay_validation()
{
    __asm
    {
        push ebp
        mov ebp, esp
        and esp, 0xfffffff8
        jmp dword ptr [native_validation_body]
    }
}

void __fastcall validate_replay(void* app,
                                void*,
                                void** fiber,
                                void* replay,
                                void* message,
                                unsigned int* result,
                                int load_map,
                                int draw,
                                void* extra,
                                int options)
{
    Recording* task = recording.get();
    if (task == nullptr || task->thread != GetCurrentThreadId() || task->replay != replay ||
        (task->replay_fiber != nullptr && task->replay_fiber != fiber))
    {
        at<ValidateReplay>(kReplayValidation)(
            app, fiber, replay, message, result, load_map, draw, extra, options);
        return;
    }
    task->replay_fiber = fiber;
    task->inside_validation = true;
    reinterpret_cast<ValidateReplay>(stock_replay_validation)(
        app, fiber, replay, message, result, load_map, draw, extra, options);
    task->inside_validation = false;
    if (has_ended(*fiber))
    {
        stop_recording(*task);
        recording.reset();
    }
}

const wchar_t* __fastcall
validation_success_text(void* localizer, void*, const wchar_t* key) noexcept
{
    Recording* task = recording.get();
    if (task == nullptr || task->thread != GetCurrentThreadId() || !task->inside_validation)
    {
        using Localize = const wchar_t*(__thiscall*)(void*, const wchar_t*);
        return at<Localize>(0x502460)(localizer, key);
    }
    // This call site is reached only after stock validation accepts the run.
    // Its result dialog displays our save result and waits for acknowledgement.
    if (task->completion == nullptr)
    {
        try
        {
            task->completion = task->error;
            if (task->completion == nullptr)
            {
                task->completion = save_replay(game, task->source, task->states, task->offsets);
            }
            if (task->completion == nullptr)
            {
                task->completion = L"HD replay saved beside the original.";
            }
        }
        catch (...)
        {
            task->completion = L"Could not write the HD replay.";
        }
    }
    return task->completion;
}

__declspec(naked) void validation_success_text_call()
{
    __asm
    {
        call validation_success_text
        jmp dword ptr [success_text_continuation]
    }
}

__declspec(naked) void replay_validation_call()
{
    __asm
    {
        call validate_replay
        jmp dword ptr [replay_call_continuation]
    }
}

__declspec(naked) void validation_cleanup()
{
    __asm
    {
        pushfd
        pushad
        push ebx
        push esi
        call defer_cleanup
        add esp, 8
        test al, al
        jnz yield
        popad
        popfd
        jmp dword ptr [original_cleanup]
    yield:
        popad
        popfd
        jmp dword ptr [yield_validation]
    }
}
} // namespace

bool prepare(unsigned char* image, std::span<EntryHook, kHookCount> entries) noexcept
{
    static constexpr std::array<unsigned char, 7> show_signature{
        0x56, 0x8b, 0x74, 0x24, 0x08, 0x85, 0xf6};
    static constexpr std::array<unsigned char, 5> call_signature{0xe8, 0x50, 0xc4, 0xfe, 0xff};
    static constexpr std::array<unsigned char, 5> text_signature{0xe8, 0x6b, 0x97, 0x47, 0};
    static constexpr std::array<unsigned char, 5> input_signature{0xe9, 0x7f, 0x05, 0, 0};
    static constexpr std::array<unsigned char, 8> brake_signature{
        0x8b, 0x8e, 0x38, 0x02, 0, 0, 0xd9, 0xee};
    static constexpr std::array<unsigned char, 8> cleanup_signature{
        0x83, 0xbc, 0x24, 0x8c, 0, 0, 0, 0};
    static std::array<unsigned char, 7> race_signature{0x6a, 0xff, 0x68};
    static std::array<unsigned char, 7> menu_signature{0x6a, 0xff, 0x68};
    static std::array<unsigned char, 7> continuation_signature{0x6a, 0xff, 0x68};
    const auto race_exception = reinterpret_cast<std::uintptr_t>(image + 0x6885eb);
    const auto menu_exception = reinterpret_cast<std::uintptr_t>(image + 0x68d218);
    const auto replay_exception = reinterpret_cast<std::uintptr_t>(image + 0x689abf);
    std::memcpy(race_signature.data() + 3, &race_exception, sizeof(race_exception));
    std::memcpy(menu_signature.data() + 3, &menu_exception, sizeof(menu_exception));
    std::memcpy(continuation_signature.data() + 3, &replay_exception, sizeof(replay_exception));
    if (std::memcmp(image + kReplayValidation + 6,
                    continuation_signature.data(),
                    continuation_signature.size()) != 0 ||
        std::memcmp(image + kAutomaticBraking, brake_signature.data(), brake_signature.size()) != 0)
    {
        return false;
    }
    const std::array<EntryHook, kHookCount> requested{{
        {image + kShowControl,
         reinterpret_cast<void*>(show_control),
         reinterpret_cast<void**>(&original_show),
         show_signature.data(),
         show_signature.size()},
        {image + kRaceValidation,
         reinterpret_cast<void*>(validate_race),
         reinterpret_cast<void**>(&original_race),
         race_signature.data(),
         race_signature.size()},
        {image + kReplayValidationCall,
         reinterpret_cast<void*>(replay_validation_call),
         &replay_call_trampoline,
         call_signature.data(),
         call_signature.size()},
        {image + kValidationSuccessTextCall,
         reinterpret_cast<void*>(validation_success_text_call),
         &success_text_trampoline,
         text_signature.data(),
         text_signature.size()},
        {image + kReplayMenu,
         reinterpret_cast<void*>(replay_menu),
         reinterpret_cast<void**>(&original_menu),
         menu_signature.data(),
         menu_signature.size()},
        {image + kBeforeValidationCleanup,
         reinterpret_cast<void*>(validation_cleanup),
         &original_cleanup,
         cleanup_signature.data(),
         cleanup_signature.size()},
        {image + kRaceInputFinish,
         reinterpret_cast<void*>(race_input_finish),
         &input_finish_trampoline,
         input_signature.data(),
         input_signature.size()},
    }};
    for (const auto& entry : requested)
    {
        if (std::memcmp(entry.target, entry.expected_bytes, entry.expected_size) != 0)
        {
            return false;
        }
    }
    game = image;
    native_validation_body = image + kReplayValidation + 6;
    replay_call_continuation = image + kReplayValidationCall + call_signature.size();
    success_text_continuation = image + kValidationSuccessTextCall + text_signature.size();
    yield_validation = image + kYieldRaceValidation;
    automatic_braking = image + kAutomaticBraking;
    race_input_continuation = image + kRaceInputContinuation;
    std::copy(requested.begin(), requested.end(), entries.begin());
    return true;
}
} // namespace cinemania::hd_replay
