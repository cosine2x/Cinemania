#include "export_volume.hpp"

#include "openal_capture.hpp"
#include "video_export_internal.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include <intrin.h>
#include <windows.h>

extern "C"
{
    void* cinemania_sound_original = nullptr;
    void* cinemania_music_original = nullptr;
    void* cinemania_sound_continuation = nullptr;
    void* cinemania_sound_coefficient = nullptr;
    void cinemania_sound_bridge();
    void cinemania_music_bridge();
}

namespace cinemania::export_volume
{
namespace
{
constexpr int kGain = 0x100a;
constexpr std::size_t kCapacity = 4096;
using Sourcef = void(__cdecl*)(unsigned int, int, float);
using IsSource = char(__cdecl*)(unsigned int);
using GetSourcef = void(__cdecl*)(unsigned int, int, float*);
using CurrentContext = void*(__cdecl*)();
using DeleteSources = void(__cdecl*)(int, const unsigned int*);

struct Entry
{
    unsigned int source = 0;
    float applied = 0;
};

struct State
{
    SRWLOCK lock = SRWLOCK_INIT;
    std::shared_ptr<Session> session;
    void* port = nullptr;
    void* context = nullptr;
    DWORD owner = 0;
    std::array<Entry, kCapacity> entries{};
};

State& state() noexcept
{
    alignas(State) static std::byte storage[sizeof(State)];
    static State* instance = new (storage) State;
    return *instance;
}

Sourcef original_sourcef = nullptr;
IsSource is_source = nullptr;
GetSourcef get_sourcef = nullptr;
CurrentContext current_context = nullptr;
DeleteSources original_delete = nullptr;
unsigned char* game_base = nullptr;
thread_local void* classified_port = nullptr;
thread_local bool classified_sound = false;

bool readable_protection(DWORD protection) noexcept
{
    protection &= 0xff;
    return protection == PAGE_READONLY || protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool read_slider(void* port, float& value) noexcept
{
    const auto base = reinterpret_cast<std::uintptr_t>(port);
    if (!base || base > UINTPTR_MAX - 0x2c - sizeof(value))
    {
        return false;
    }
    const auto address = base + 0x2c;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
        !readable_protection(info.Protect))
    {
        return false;
    }
    const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (region > UINTPTR_MAX - info.RegionSize || address < region ||
        address + sizeof(value) > region + info.RegionSize)
    {
        return false;
    }
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

void failure(Session& session) noexcept
{
    audio_capture::record_failure(session, game_export::detail::CaptureFailure::volume);
}

void __cdecl sourcef_bridge(unsigned int source, int parameter, float value)
{
    source_gain(source, parameter, value, _ReturnAddress());
}

void __cdecl delete_bridge(int count, const unsigned int* sources)
{
    original_delete(count, sources);
    if (count <= 0 || !sources)
    {
        return;
    }

    State& route = state();
    std::array<Entry, kCapacity> snapshot{};
    AcquireSRWLockShared(&route.lock);
    void* expected_context =
        route.session && route.owner == GetCurrentThreadId() ? route.context : nullptr;
    if (expected_context)
    {
        snapshot = route.entries;
    }
    ReleaseSRWLockShared(&route.lock);
    if (!expected_context)
    {
        return;
    }
    void* context = current_context();
    if (context != expected_context)
    {
        return;
    }
    // Scan only our bounded tracked IDs. This also handles arbitrary deletion
    // counts and avoids borrowing the caller's array after the original call.
    for (const Entry& candidate : snapshot)
    {
        const unsigned int source = candidate.source;
        if (!source || is_source(source))
        {
            continue;
        }
        AcquireSRWLockExclusive(&route.lock);
        if (route.session && route.context == context && route.owner == GetCurrentThreadId())
        {
            for (Entry& entry : route.entries)
            {
                if (entry.source == source)
                {
                    entry = {};
                    break;
                }
            }
        }
        ReleaseSRWLockExclusive(&route.lock);
    }
}
} // namespace

bool activate(const std::shared_ptr<Session>& session, void* port, void* context) noexcept
{
    if (!session || !port || !context || !original_sourcef || !is_source || !get_sourcef ||
        !current_context || session->owner != GetCurrentThreadId())
    {
        return false;
    }

    float slider = 0;
    if (current_context() != context || !read_slider(port, slider))
    {
        return false;
    }

    State& route = state();
    AcquireSRWLockExclusive(&route.lock);
    const bool available = !route.session;
    if (available)
    {
        route.session = session;
        route.port = port;
        route.context = context;
        route.owner = session->owner;
        route.entries.fill({});
    }
    ReleaseSRWLockExclusive(&route.lock);
    return available;
}

void deactivate(const std::shared_ptr<Session>& session) noexcept
{
    std::array<Entry, kCapacity> entries{};
    void* port = nullptr;
    void* context = nullptr;
    State& route = state();
    AcquireSRWLockExclusive(&route.lock);
    if (route.session == session)
    {
        entries = route.entries;
        port = route.port;
        context = route.context;
        route.session.reset();
        route.port = nullptr;
        route.context = nullptr;
        route.owner = 0;
        route.entries.fill({});
    }
    ReleaseSRWLockExclusive(&route.lock);

    if (!port)
    {
        return;
    }
    float slider = 0;
    if (current_context() != context || !read_slider(port, slider))
    {
        failure(*session);
        return;
    }
    for (const Entry& entry : entries)
    {
        if (!entry.source || !is_source(entry.source))
        {
            continue;
        }
        float current = 0;
        get_sourcef(entry.source, kGain, &current);
        if (std::isfinite(current) && current == entry.applied)
        {
            const float restored = entry.applied * slider;
            if (std::isfinite(restored))
            {
                original_sourcef(entry.source, kGain, restored);
            }
        }
    }
}

int classify(void* port, bool sound) noexcept
{
    State& route = state();
    AcquireSRWLockShared(&route.lock);
    void* context =
        sound && route.session && route.port == port && route.owner == GetCurrentThreadId()
            ? route.context
            : nullptr;
    ReleaseSRWLockShared(&route.lock);
    const bool override = context && current_context() == context;
    classified_port = override ? port : nullptr;
    classified_sound = override;
    return override ? 1 : 0;
}

namespace
{
struct WritePlan
{
    std::shared_ptr<Session> session;
    void* context = nullptr;
    void* port = nullptr;
    bool owned = false;
    bool full = false;
};

WritePlan plan_write(unsigned int source, const void* caller) noexcept
{
    WritePlan plan;
    State& route = state();
    AcquireSRWLockShared(&route.lock);
    const bool active = static_cast<bool>(route.session);
    ReleaseSRWLockShared(&route.lock);

    if (!active)
    {
        return plan;
    }

    void* context = current_context();
    const bool native = reinterpret_cast<std::uintptr_t>(caller) ==
                        reinterpret_cast<std::uintptr_t>(game_base) + 0x3aef0d;

    AcquireSRWLockShared(&route.lock);
    if (route.session && route.context == context)
    {
        plan.session = route.session;
        plan.context = context;
        plan.port = route.port;
        plan.owned = native && classified_sound && classified_port == route.port &&
                     route.owner == GetCurrentThreadId();

        if (plan.owned)
        {
            plan.full = true;
            for (const Entry& entry : route.entries)
            {
                if (!entry.source || entry.source == source)
                {
                    plan.full = false;
                    break;
                }
            }
        }
    }

    ReleaseSRWLockShared(&route.lock);

    return plan;
}

float prepare_gain(const WritePlan& plan, float value) noexcept
{
    if (!plan.owned || !plan.full)
    {
        return value;
    }

    float slider = 0;
    if (!read_slider(plan.port, slider))
    {
        slider = 0;
    }

    failure(*plan.session);
    return value * slider;
}

bool accepted_gain(const WritePlan& plan, unsigned int source, float applied) noexcept
{
    if (!plan.session || (plan.owned && plan.full) || current_context() != plan.context ||
        !is_source(source))
    {
        return false;
    }

    float actual = 0;
    get_sourcef(source, kGain, &actual);

    return std::isfinite(actual) && actual == applied;
}

void commit_gain(const WritePlan& plan, unsigned int source, float applied) noexcept
{
    State& route = state();
    AcquireSRWLockExclusive(&route.lock);

    if (route.session == plan.session && route.context == plan.context)
    {
        Entry* slot = nullptr;
        Entry* empty = nullptr;
        for (Entry& entry : route.entries)
        {
            if (entry.source == source)
            {
                slot = &entry;
                break;
            }
            if (!entry.source && !empty)
            {
                empty = &entry;
            }
        }

        if (plan.owned && (slot || empty))
        {
            *(slot ? slot : empty) = {source, applied};
        }
        else if (!plan.owned && slot)
        {
            *slot = {};
        }
    }

    ReleaseSRWLockExclusive(&route.lock);
}
} // namespace

void source_gain(unsigned int source, int parameter, float value, const void* caller) noexcept
{
    if (parameter != kGain || !source || !std::isfinite(value) || value < 0.0f)
    {
        classified_port = nullptr;
        classified_sound = false;
        original_sourcef(source, parameter, value);
        return;
    }

    const WritePlan plan = plan_write(source, caller);
    classified_port = nullptr;
    classified_sound = false;
    const float applied = prepare_gain(plan, value);

    original_sourcef(source, parameter, applied);
    if (accepted_gain(plan, source, applied))
    {
        commit_gain(plan, source, applied);
    }
}

bool prepare(unsigned char* game,
             std::size_t size,
             std::array<EntryHook, 2>& entries,
             std::array<ImportHook, 2>& imports) noexcept
{
    // The exact image hash was already checked by bootstrap. Validate relocated
    // operands, complete instructions, constants, IAT owner and API exports.
    if (!game || size < 0x7283d4 || size < 0x741eb0)
    {
        return false;
    }
    HMODULE openal = GetModuleHandleW(L"OpenAL32.dll");
    if (!openal)
    {
        return false;
    }
    auto source = reinterpret_cast<Sourcef>(GetProcAddress(openal, "alSourcef"));
    auto is = reinterpret_cast<IsSource>(GetProcAddress(openal, "alIsSource"));
    auto get = reinterpret_cast<GetSourcef>(GetProcAddress(openal, "alGetSourcef"));
    auto current = reinterpret_cast<CurrentContext>(GetProcAddress(openal, "alcGetCurrentContext"));
    auto remove = reinterpret_cast<DeleteSources>(GetProcAddress(openal, "alDeleteSources"));
    auto** slot = reinterpret_cast<void**>(game + 0x7283d0);
    auto** delete_slot = reinterpret_cast<void**>(game + 0x728388);
    if (!source || !is || !get || !current || !remove || *slot != reinterpret_cast<void*>(source) ||
        *delete_slot != reinterpret_cast<void*>(remove))
    {
        return false;
    }
    static std::array<unsigned char, 9> music{0xd9, 0x45, 0x30, 0xdc, 0x0d};
    static std::array<unsigned char, 9> sound{0xd9, 0x45, 0x2c, 0xdc, 0x0d};
    const auto music_address = reinterpret_cast<std::uintptr_t>(game) + 0x741ea8;
    const auto sound_address = reinterpret_cast<std::uintptr_t>(game) + 0x7362a8;
    if (music_address > UINT32_MAX || sound_address > UINT32_MAX)
    {
        return false;
    }
    const auto music32 = static_cast<std::uint32_t>(music_address);
    const auto sound32 = static_cast<std::uint32_t>(sound_address);
    std::memcpy(music.data() + 5, &music32, 4);
    std::memcpy(sound.data() + 5, &sound32, 4);
    std::uint64_t music_bits = 0;
    std::uint64_t sound_bits = 0;
    std::memcpy(&music_bits, game + 0x741ea8, 8);
    std::memcpy(&sound_bits, game + 0x7362a8, 8);
    if (music_bits != UINT64_C(0x3fecccccc0000000) || sound_bits != UINT64_C(0x3fee666660000000) ||
        std::memcmp(game + 0x39d6d2, music.data(), music.size()) != 0 ||
        std::memcmp(game + 0x39d6dd, sound.data(), sound.size()) != 0)
    {
        return false;
    }
    original_sourcef = source;
    is_source = is;
    get_sourcef = get;
    current_context = current;
    original_delete = remove;
    game_base = game;
    cinemania_sound_continuation = game + 0x39d6e6;
    cinemania_sound_coefficient = game + 0x7362a8;
    entries[0] = {game + 0x39d6d2,
                  reinterpret_cast<void*>(cinemania_music_bridge),
                  &cinemania_music_original,
                  music.data(),
                  music.size()};
    entries[1] = {game + 0x39d6dd,
                  reinterpret_cast<void*>(cinemania_sound_bridge),
                  &cinemania_sound_original,
                  sound.data(),
                  sound.size()};
    imports[0] = {slot, reinterpret_cast<void*>(source), reinterpret_cast<void*>(sourcef_bridge)};
    imports[1] = {
        delete_slot, reinterpret_cast<void*>(remove), reinterpret_cast<void*>(delete_bridge)};
    return true;
}
} // namespace cinemania::export_volume
