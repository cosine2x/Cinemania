#include "hd_replay.hpp"

#include "game/game_abi.hpp"
#include "game/game_ui.hpp"
#include "hd_replay_file.hpp"
#include "hd_replay_menu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

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
constexpr std::size_t kReplayValidationRva = 0x88760;
constexpr std::size_t kReplayValidationCallRva = 0x9c30b;
constexpr std::size_t kValidationSuccessTextCallRva = 0x88cf0;
constexpr std::size_t kRaceValidationRva = 0x82760;
constexpr std::size_t kBeforeValidationCleanupRva = 0x82fa6;
constexpr std::size_t kYieldRaceValidationRva = 0x831b8;
constexpr std::size_t kRaceInputFinishRva = 0x7d3e3;
constexpr std::size_t kAutomaticBrakingRva = 0x7d841;
constexpr std::size_t kRaceInputContinuationRva = 0x7d967;
constexpr std::size_t kStockPrologueBytes = 6;
constexpr std::size_t kGhostRecorderSize = 0x198;
constexpr auto kTailTimeout = std::chrono::seconds(6);
constexpr std::size_t kValidationHookCount = kHookCount - hd_replay_menu::kHookCount;

constexpr std::size_t kSelectValidateRva = 0xc6a40;
constexpr std::size_t kSourceGhostVtableRva = 0x7660e4;
constexpr std::size_t kCommandCoreRva = 0x9731e0;
constexpr std::size_t kAllocateRva = 0x3079;
constexpr std::size_t kConstructGhostRva = 0x1e0ad0;
constexpr std::size_t kStartGhostRva = 0x1dfe90;
constexpr std::size_t kStopGhostRva = 0x276190;
constexpr std::size_t kAddRefRva = 0x523e60;
constexpr std::size_t kReleaseRva = 0x524910;
constexpr std::size_t kRaceExceptionRva = 0x6885eb;
constexpr std::size_t kReplayExceptionRva = 0x689abf;

constexpr std::size_t kAppOffset = 0x784;
constexpr std::size_t kOperationOffset = 0x700;
constexpr std::size_t kLoadedReplayOffset = 0x1d0;
constexpr std::size_t kGhostCountOffset = 0x18;
constexpr std::size_t kGhostsOffset = 0x1c;
constexpr std::size_t kInputCountOffset = 0x10c;
constexpr std::size_t kGhostFinishMsOffset = 0xe8;
constexpr std::size_t kPlayerOffset = 0x238;
constexpr std::size_t kVehicleOffset = 0x28;
constexpr std::size_t kVehicleSceneOffset = 0x14;
constexpr std::size_t kRecorderPeriodMsOffset = 0xd0;
constexpr std::size_t kRaceStartMsOffset = 0x314;
constexpr std::size_t kPlayerInfoOffset = 0x330;
constexpr std::size_t kRaceStatusOffset = 0x314;
constexpr std::size_t kPlayerFinishMsOffset = 0x2a8;
constexpr std::size_t kValidationVerdictOffset = 0x1c;
constexpr std::size_t kFiberPhaseOffset = 4;
constexpr std::size_t kPackedStatesOffset = 0x30;
constexpr std::size_t kPackedSizeOffset = 0x34;
constexpr std::size_t kSampleOffsetsOffset = 0x54;
constexpr std::size_t kRecordingFlagsOffset = 0x68;
constexpr std::size_t kVehicleClassOffset = 0x4c;
constexpr std::size_t kStateVersionOffset = 0x48;
constexpr std::size_t kSamplePeriodMsOffset = 0x44;
constexpr std::size_t kSampleCountOffset = 0x50;

using ValidateReplay =
    void(__thiscall*)(void*, void**, void*, void*, unsigned int*, int, int, void*, int);
using ValidateRace = void(__thiscall*)(void*, void**, void*, unsigned int*, void*, int, int, int);
unsigned char* game_image = nullptr;
ValidateRace original_race = nullptr;
void* original_cleanup = nullptr;
const void* replay_call_continuation = nullptr;
const void* success_text_continuation = nullptr;
const void* native_validation_body = nullptr;
const void* yield_validation = nullptr;
const void* automatic_braking = nullptr;
const void* race_input_continuation = nullptr;
using game_abi::field;
using game_abi::function_at;

struct Recording
{
    SourceFile source;
    SampleLayout layout{};
    std::uint32_t expected_finish_ms = kUnsetTime;
    std::uint32_t verified_finish_ms = kUnsetTime;
    DWORD thread = 0;
    void* replay = nullptr;
    void** replay_fiber = nullptr;
    void** race_fiber = nullptr;
    void* race = nullptr;
    // The race must stop the recorder before native validation tears down its car.
    void* ghost = nullptr;
    std::uint32_t* period = nullptr;
    std::uint32_t previous_period = 0;
    std::chrono::steady_clock::time_point tail_deadline;
    bool inside_validation = false;
    const wchar_t* error = nullptr;
    const wchar_t* completion = nullptr;
    std::vector<std::uint8_t> states;
    std::vector<std::uint32_t> offsets;
};

// Native validation, race, and menu callbacks run on the game thread.
std::unique_ptr<Recording> recording;

void discard_pending_recording() noexcept
{
    if (recording && recording->replay_fiber == nullptr)
    {
        recording.reset();
    }
}

std::uint32_t outro_sample_count(std::uint32_t finish) noexcept
{
    const auto end_ms = static_cast<std::uint64_t>(finish) + kOutroDurationMs;
    const auto count = (end_ms + kSamplePeriodMs - 1) / kSamplePeriodMs + 1;
    // The native recorder caps the packed vehicle state buffer.
    constexpr unsigned int min_state_bytes = 35;
    return count <= kMaxStateBytes / min_state_bytes ? static_cast<std::uint32_t>(count) : 0;
}

std::uint32_t phase(void* fiber)
{
    return fiber == nullptr || game_abi::fiber_finished(fiber)
               ? kUnsetTime
               : field<std::uint32_t>(fiber, kFiberPhaseOffset);
}

[[nodiscard]] const wchar_t* prepare_recording(void* menu, void* app, Recording& task)
{
    task.replay = app != nullptr ? field<void*>(app, kLoadedReplayOffset) : nullptr;
    if (task.replay == nullptr || field<unsigned int>(task.replay, kGhostCountOffset) != 1)
    {
        return L"Select a replay containing one recorded car.";
    }

    void** ghosts = field<void**>(task.replay, kGhostsOffset);
    void* source_ghost = ghosts != nullptr ? *ghosts : nullptr;
    if (!game_abi::has_vtable(source_ghost, game_image + kSourceGhostVtableRva) ||
        field<std::uint32_t>(source_ghost, kInputCountOffset) == 0)
    {
        return L"This replay has no recorded inputs to validate.";
    }

    const auto path = hd_replay_menu::selected_path(menu);
    if (path.empty())
    {
        return L"Select a single replay saved on a local drive.";
    }

    task.expected_finish_ms = field<std::uint32_t>(source_ghost, kGhostFinishMsOffset);
    if (task.expected_finish_ms != kUnsetTime && outro_sample_count(task.expected_finish_ms) == 0)
    {
        return L"This replay is too long for the game's HD ghost recorder.";
    }

    task.layout = {field<std::uint32_t>(source_ghost, kVehicleClassOffset),
                   field<std::uint32_t>(source_ghost, kStateVersionOffset),
                   field<std::uint32_t>(source_ghost, kSamplePeriodMsOffset),
                   field<std::uint32_t>(source_ghost, kSampleCountOffset)};
    return open_source(game_image, path, task.layout, task.source);
}

void __fastcall record_selected(void* menu, void*) noexcept
{
    if (recording || field<unsigned int>(menu, kOperationOffset) != 0)
    {
        return;
    }

    void* app = field<void*>(menu, kAppOffset);
    try
    {
        auto task = std::make_unique<Recording>();
        task->thread = GetCurrentThreadId();
        if (const wchar_t* error = prepare_recording(menu, app, *task))
        {
            game_ui::show_message(game_image, app, error);
            return;
        }

        recording = std::move(task);
        // Dispatch the stock event so TMInterface selects its validation mode.
        using SelectValidate = void(__thiscall*)(void*);
        function_at<SelectValidate>(game_image, kSelectValidateRva)(menu);
        if (field<unsigned int>(menu, kOperationOffset) != kValidateOperation)
        {
            recording.reset();
            game_ui::show_message(game_image, app, L"The game could not start replay validation.");
        }
    }
    catch (...)
    {
        game_ui::show_message(game_image, app, L"Could not prepare the selected replay.");
    }
}

void stop_recording(Recording& task) noexcept
{
    using Method = void(__thiscall*)(void*);
    if (task.ghost != nullptr)
    {
        function_at<Method>(game_image, kStopGhostRva)(task.ghost);
        function_at<Method>(game_image, kReleaseRva)(task.ghost);
        task.ghost = nullptr;
    }
    if (task.period != nullptr)
    {
        if (*task.period == kSamplePeriodMs)
        {
            *task.period = task.previous_period;
        }
        task.period = nullptr;
    }
}

void start_recording(Recording& task, void* player_info)
{
    void* player = player_info != nullptr ? field<void*>(player_info, kPlayerOffset) : nullptr;
    void* vehicle = player != nullptr ? field<void*>(player, kVehicleOffset) : nullptr;
    if (vehicle == nullptr || field<void*>(vehicle, kVehicleSceneOffset) == nullptr)
    {
        task.error = L"Validation did not create a car to record.";
        return;
    }
    void* command_core = field<void*>(game_image, kCommandCoreRva);
    if (command_core == nullptr)
    {
        task.error = L"The replay sample clock is unavailable.";
        return;
    }
    using Allocate = void*(__cdecl*)(std::size_t);
    using Construct = void*(__thiscall*)(void*);
    using AddRef = int(__thiscall*)(void*);
    using Start = void(__thiscall*)(void*, void*, std::uint32_t);
    void* memory = function_at<Allocate>(game_image, kAllocateRva)(kGhostRecorderSize);
    if (memory == nullptr)
    {
        task.error = L"Could not allocate a car sample recorder.";
        return;
    }
    task.ghost = function_at<Construct>(game_image, kConstructGhostRva)(memory);
    function_at<AddRef>(game_image, kAddRefRva)(task.ghost);
    task.period = &field<std::uint32_t>(command_core, kRecorderPeriodMsOffset);
    task.previous_period = *task.period;
    *task.period = kSamplePeriodMs;
    function_at<Start>(game_image, kStartGhostRva)(
        task.ghost, vehicle, field<std::uint32_t>(task.race, kRaceStartMsOffset));
}

void copy_samples(Recording& task, std::uint32_t actual_finish)
{
    void* ghost = task.ghost;
    const std::uint32_t count = field<std::uint32_t>(ghost, kSampleCountOffset);
    const std::uint32_t size = field<std::uint32_t>(ghost, kPackedSizeOffset);
    const auto* states = field<const std::uint8_t*>(ghost, kPackedStatesOffset);
    const auto* offsets = field<const std::uint32_t*>(ghost, kSampleOffsetsOffset);
    if (actual_finish == kUnsetTime ||
        (task.expected_finish_ms != kUnsetTime && actual_finish != task.expected_finish_ms) ||
        field<std::uint32_t>(ghost, kSamplePeriodMsOffset) != kSamplePeriodMs ||
        field<std::uint32_t>(ghost, kStateVersionOffset) != task.layout.version ||
        field<std::uint32_t>(ghost, kVehicleClassOffset) != task.layout.vehicle_class ||
        field<std::uint32_t>(ghost, kRecordingFlagsOffset) != 1 ||
        count < task.source.target_samples || count > kMaxStateBytes || size > kMaxStateBytes ||
        size == 0 || states == nullptr || offsets == nullptr)
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
    if (task->verified_finish_ms == kUnsetTime)
    {
        // Preserve the native verdict at the finish. The race can transition
        // away from its finished state while we collect the playback tail.
        void* player = field<void*>(race, kPlayerInfoOffset);
        if (field<unsigned int>(*fiber, kValidationVerdictOffset) != kValidReplay ||
            player == nullptr || field<unsigned int>(player, kRaceStatusOffset) != kRaceFinished)
        {
            return false;
        }
        const auto finish = field<std::uint32_t>(player, kPlayerFinishMsOffset);
        if (finish == kUnsetTime ||
            (task->expected_finish_ms != kUnsetTime && finish != task->expected_finish_ms))
        {
            return false;
        }
        task->verified_finish_ms = finish;
        task->source.target_samples = outro_sample_count(finish);
        if (task->source.target_samples == 0)
        {
            task->error = L"This replay is too long for the game's HD ghost recorder.";
            stop_recording(*task);
            return false;
        }
        task->tail_deadline = std::chrono::steady_clock::now() + kTailTimeout;
    }
    if (field<std::uint32_t>(task->ghost, kSampleCountOffset) < task->source.target_samples &&
        std::chrono::steady_clock::now() < task->tail_deadline)
    {
        field<unsigned int>(*fiber, kFiberPhaseOffset) = kValidationRunning;
        return true;
    }
    try
    {
        copy_samples(*task, task->verified_finish_ms);
    }
    catch (...)
    {
        task->error = L"Could not copy the recorded car samples.";
    }
    stop_recording(*task);
    field<unsigned int>(*fiber, kValidationVerdictOffset) = kValidReplay;
    return false;
}

bool __cdecl use_automatic_braking(void* race, void* player) noexcept
{
    const Recording* task = recording.get();
    if (task == nullptr || task->thread != GetCurrentThreadId() || task->race != race ||
        task->ghost == nullptr || field<void*>(race, kPlayerInfoOffset) != player ||
        player == nullptr)
    {
        return false;
    }
    return task->verified_finish_ms != kUnsetTime ||
           field<unsigned int>(player, kRaceStatusOffset) == kRaceFinished;
}

// Stock validation has just restored the normal clock and zeroed EDI. Use
// its saved input time to run the ordinary race's automatic braking block.
// EBP is the race, ESI the player info. Preserve registers and flags except
// for the saved time in EDI and the brake branch's cleared input flag.
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
        start_recording(*task, field<void*>(race, kPlayerInfoOffset));
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
        function_at<ValidateReplay>(game_image, kReplayValidationRva)(
            app, fiber, replay, message, result, load_map, draw, extra, options);
        return;
    }
    task->replay_fiber = fiber;
    task->inside_validation = true;

    // The naked bridge preserves ECX and the stack. The stock thiscall body
    // consumes the arguments and returns directly to this caller.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-calling-convention"
#endif
    reinterpret_cast<ValidateReplay>(stock_replay_validation)(
        app, fiber, replay, message, result, load_map, draw, extra, options);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    task->inside_validation = false;
    if (game_abi::fiber_finished(*fiber))
    {
        stop_recording(*task);
        recording.reset();
    }
}

const wchar_t* finish_recording(const Recording& task) noexcept
{
    if (task.error != nullptr)
    {
        return task.error;
    }

    try
    {
        if (const wchar_t* error = save_replay(game_image, task.source, task.states, task.offsets))
        {
            return error;
        }

        return L"HD replay saved beside the original.";
    }
    catch (...)
    {
        return L"Could not write the HD replay.";
    }
}

const wchar_t* __fastcall
validation_success_text(void* localizer, void*, const wchar_t* key) noexcept
{
    Recording* task = recording.get();
    if (task == nullptr || task->thread != GetCurrentThreadId() || !task->inside_validation)
    {
        return function_at<game_ui::Localize>(game_image, game_ui::kLocalizerRva)(localizer, key);
    }

    // Stock validation accepted the run. Its result dialog owns acknowledgement.
    if (task->completion == nullptr)
    {
        task->completion = finish_recording(*task);
    }
    return task->completion;
}

// Replace the native CALL at each site. ECX and the stacked arguments match
// the original thiscall target; fastcall ignores EDX and pops those arguments.
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

// ESI is the race and EBX points to its fiber slot. Preserve the original
// registers and flags for either the native cleanup trampoline or the yield.
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
    static constexpr std::array<unsigned char, 5> call_signature{0xe8, 0x50, 0xc4, 0xfe, 0xff};
    static constexpr std::array<unsigned char, 5> text_signature{0xe8, 0x6b, 0x97, 0x47, 0};
    static constexpr std::array<unsigned char, 5> input_signature{0xe9, 0x7f, 0x05, 0, 0};
    static constexpr std::array<unsigned char, 8> brake_signature{
        0x8b, 0x8e, 0x38, 0x02, 0, 0, 0xd9, 0xee};
    static constexpr std::array<unsigned char, 8> cleanup_signature{
        0x83, 0xbc, 0x24, 0x8c, 0, 0, 0, 0};
    static game_abi::SehPrologue race_signature;
    race_signature = game_abi::seh_prologue(image + kRaceExceptionRva);
    const auto continuation_signature = game_abi::seh_prologue(image + kReplayExceptionRva);

    if (std::memcmp(image + kReplayValidationRva + kStockPrologueBytes,
                    continuation_signature.data(),
                    continuation_signature.size()) != 0 ||
        std::memcmp(image + kAutomaticBrakingRva, brake_signature.data(), brake_signature.size()) !=
            0)
    {
        return false;
    }
    const std::array requested{
        make_entry_hook(image + kRaceValidationRva,
                        validate_race,
                        race_signature,
                        original_address(original_race)),
        make_entry_hook(image + kReplayValidationCallRva, replay_validation_call, call_signature),
        make_entry_hook(
            image + kValidationSuccessTextCallRva, validation_success_text_call, text_signature),
        make_entry_hook(image + kBeforeValidationCleanupRva,
                        validation_cleanup,
                        cleanup_signature,
                        &original_cleanup),
        make_entry_hook(image + kRaceInputFinishRva, race_input_finish, input_signature),
    };
    static_assert(requested.size() == kValidationHookCount);
    std::copy(requested.begin(), requested.end(), entries.begin());
    hd_replay_menu::prepare(image,
                            entries.last<hd_replay_menu::kHookCount>(),
                            record_selected,
                            discard_pending_recording);

    for (const EntryHook& entry : entries)
    {
        if (std::memcmp(entry.target, entry.expected_bytes, entry.expected_size) != 0)
        {
            return false;
        }
    }
    game_image = image;
    native_validation_body = image + kReplayValidationRva + kStockPrologueBytes;
    replay_call_continuation = image + kReplayValidationCallRva + call_signature.size();
    success_text_continuation = image + kValidationSuccessTextCallRva + text_signature.size();
    yield_validation = image + kYieldRaceValidationRva;
    automatic_braking = image + kAutomaticBrakingRva;
    race_input_continuation = image + kRaceInputContinuationRva;
    return true;
}
} // namespace cinemania::hd_replay
