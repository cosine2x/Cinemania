#include "openal_capture.hpp"

#include "export_volume.hpp"
#include "mixer_pcm_buffer.hpp"
#include "openal_capture_compat.hpp"
#include "video/video_export_internal.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>

#include <windows.h>

namespace cinemania::audio_capture
{
namespace
{
constexpr int kInvalidDevice = 0xa001;
constexpr int kInvalidValue = 0xa004;
constexpr int kCaptureSamples = 0x312;
constexpr int kStereo16 = 0x1103;
constexpr std::size_t kCapacity = 88200;

struct Handle
{
    // Identities are never reused. Detached handles permit only the owning
    // thread's final empty drain and close; closed handles retain error state.
    bool used = false;
    bool closed = false;
    bool detached = false;
    DWORD owner = 0;
    int error = 0;
};

struct Router
{
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<Handle, 1024> handles{};
    std::shared_ptr<Session> session;
    std::unique_ptr<MixerPcmBuffer> buffer;
    void* context = nullptr;
    std::uint32_t device = 0;
    Handle* active = nullptr;
    bool started = false;
    std::atomic_flag producer = ATOMIC_FLAG_INIT;
};

Router& router() noexcept
{
    alignas(Router) static std::byte storage[sizeof(Router)];
    static Router* instance = new (storage) Router;
    return *instance;
}

thread_local std::shared_ptr<Session> opening;
thread_local int opening_error = 0;

bool synthetic(void* device) noexcept
{
    const auto address = reinterpret_cast<std::uintptr_t>(device);
    const auto begin = reinterpret_cast<std::uintptr_t>(router().handles.data());
    return address >= begin && address < begin + sizeof(router().handles);
}

Handle* find_handle(void* device) noexcept
{
    const auto address = reinterpret_cast<std::uintptr_t>(device);
    const auto begin = reinterpret_cast<std::uintptr_t>(router().handles.data());
    const auto end = begin + sizeof(router().handles);
    if (address < begin || address >= end || (address - begin) % sizeof(Handle) != 0)
    {
        return nullptr;
    }

    return &router().handles[(address - begin) / sizeof(Handle)];
}

void failure(game_export::detail::CaptureFailure reason) noexcept
{
    if (router().session)
    {
        record_failure(*router().session, reason);
    }
}

bool valid_owner(Handle* handle) noexcept
{
    if (!handle->used || handle->closed || handle != router().active || !router().session ||
        router().session->owner != GetCurrentThreadId())
    {
        handle->error = kInvalidDevice;
        if (handle == router().active)
        {
            failure(game_export::detail::CaptureFailure::api);
        }

        return false;
    }

    return true;
}
} // namespace

bool is_synthetic(void* device) noexcept
{
    return synthetic(device);
}

void record_failure(Session& session, game_export::detail::CaptureFailure reason) noexcept
{
    auto expected = game_export::detail::CaptureFailure::none;
    session.capture_failure.compare_exchange_strong(expected, reason);
    session.foreign_failure.store(true);
}

bool bind(const std::shared_ptr<Session>& session, void* mixer_context) noexcept
{
    try
    {
        auto buffer = std::make_unique<MixerPcmBuffer>(kCapacity);

        Router& state = router();
        AcquireSRWLockExclusive(&state.lock);

        const bool available = !state.session && session && mixer_context;
        if (available)
        {
            state.session = session;
            state.buffer = std::move(buffer);
            state.context = mixer_context;
            std::memcpy(&state.device,
                        static_cast<std::byte*>(mixer_context) + compat::kMixContextDevice,
                        sizeof(state.device));
        }

        ReleaseSRWLockExclusive(&state.lock);

        return available;
    }
    catch (...)
    {
        return false;
    }
}

void detach(const std::shared_ptr<Session>& session) noexcept
{
    Router& state = router();
    AcquireSRWLockExclusive(&state.lock);

    if (state.session == session)
    {
        state.started = false;
        state.context = nullptr;
        if (state.active && !state.active->closed)
        {
            state.active->detached = true;
        }

        state.session.reset();
        state.buffer.reset();
        state.active = nullptr;
    }

    ReleaseSRWLockExclusive(&state.lock);
    export_volume::deactivate(session);
}

void* begin_mix(void* context) noexcept
{
    AcquireSRWLockShared(&router().lock);
    void* identity = nullptr;
    if (router().started && context == router().context)
    {
        identity = router().active;
    }

    ReleaseSRWLockShared(&router().lock);
    return identity;
}

void finish_mix(
    void* identity, void* context, const void* bytes, int byte_count, int format) noexcept
{
    if (!identity)
    {
        return;
    }

    AcquireSRWLockShared(&router().lock);

    if (router().started && identity == router().active && context == router().context)
    {
        // The wrapper's mixer caller retains this context until after the detour
        // returns; validate that the bound source did not change since setup.
        std::array<std::uint32_t, 3> source_format{};
        std::uint32_t source_device = 0;
        std::memcpy(source_format.data(),
                    static_cast<const std::byte*>(context) + compat::kMixContextFormat,
                    sizeof(source_format));
        std::memcpy(&source_device,
                    static_cast<const std::byte*>(context) + compat::kMixContextDevice,
                    sizeof(source_device));

        if (source_format != std::array<std::uint32_t, 3>{44100, 2, kStereo16} ||
            source_device != router().device)
        {
            failure(game_export::detail::CaptureFailure::source);
            ReleaseSRWLockShared(&router().lock);
            return;
        }

        if (router().producer.test_and_set())
        {
            failure(game_export::detail::CaptureFailure::overlap);
        }
        else
        {
            if (!bytes || byte_count <= 0 || byte_count % 4 != 0 || format != kStereo16 ||
                !router().buffer->push(std::span(static_cast<const std::byte*>(bytes),
                                                 static_cast<std::size_t>(byte_count))))
            {
                failure(game_export::detail::CaptureFailure::queue);
            }

            router().producer.clear();
        }
    }

    ReleaseSRWLockShared(&router().lock);
}

void enter_open(const std::shared_ptr<Session>& session) noexcept
{
    opening = session;
    opening_error = 0;
}

void leave_open() noexcept
{
    opening.reset();
}

void* capture_open(
    const char* name, unsigned int frequency, int format, int capacity, bool& handled) noexcept
{
    handled = static_cast<bool>(opening);
    if (!handled)
    {
        return nullptr;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = nullptr;
    if (router().session == opening && !router().active && !name && frequency == 44100 &&
        format == kStereo16 && capacity == static_cast<int>(kCapacity))
    {
        for (auto& candidate : router().handles)
        {
            if (!candidate.used)
            {
                candidate.used = true;
                candidate.owner = GetCurrentThreadId();
                handle = &candidate;
                router().active = handle;
                break;
            }
        }
    }

    if (!handle)
    {
        record_failure(*opening, game_export::detail::CaptureFailure::api);
        opening_error = kInvalidDevice;
    }

    ReleaseSRWLockExclusive(&router().lock);

    return handle;
}

bool capture_close(void* device, char& result) noexcept
{
    if (!synthetic(device))
    {
        return false;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = find_handle(device);
    if (handle)
    {
        result = 0;
        if (handle->detached && !handle->closed && handle->owner == GetCurrentThreadId())
        {
            result = 1;
            handle->closed = true;
        }
        else if (valid_owner(handle))
        {
            result = 1;
            handle->closed = true;
            router().started = false;
        }
    }

    ReleaseSRWLockExclusive(&router().lock);

    return synthetic(device);
}

bool capture_start(void* device) noexcept
{
    if (!synthetic(device))
    {
        return false;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = find_handle(device);
    if (handle && handle->detached && !handle->closed && handle->owner == GetCurrentThreadId())
    {
        // An aborted session has no buffered samples left to consume.
    }
    else if (handle && valid_owner(handle))
    {
        router().started = true;
    }

    ReleaseSRWLockExclusive(&router().lock);

    return synthetic(device);
}

bool capture_stop(void* device) noexcept
{
    if (!synthetic(device))
    {
        return false;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = find_handle(device);
    std::shared_ptr<Session> stopped_session;
    if (handle && handle->detached && !handle->closed && handle->owner == GetCurrentThreadId())
    {
        // The producer was already quiesced during cancellation.
    }
    else if (handle && valid_owner(handle))
    {
        router().started = false;
        stopped_session = router().session;
    }

    ReleaseSRWLockExclusive(&router().lock);

    if (stopped_session)
    {
        export_volume::deactivate(stopped_session);
    }

    return synthetic(device);
}

bool capture_samples(void* device, void* destination, int frames) noexcept
{
    if (!synthetic(device))
    {
        return false;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = find_handle(device);
    if (handle && valid_owner(handle))
    {
        if (!destination || frames <= 0 || frames > static_cast<int>(kCapacity) ||
            !router().buffer->read(std::span(static_cast<std::byte*>(destination),
                                             static_cast<std::size_t>(frames) * 4)))
        {
            handle->error = kInvalidValue;
            failure(game_export::detail::CaptureFailure::api);
        }
    }

    ReleaseSRWLockExclusive(&router().lock);

    return synthetic(device);
}

bool capture_integer(void* device, int parameter, int count, int* values) noexcept
{
    if (!synthetic(device))
    {
        return false;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = find_handle(device);
    if (handle && handle->detached && !handle->closed && handle->owner == GetCurrentThreadId() &&
        parameter == kCaptureSamples && count == 1 && values)
    {
        *values = 0;
    }
    else if (handle && valid_owner(handle))
    {
        if (parameter != kCaptureSamples || count != 1 || !values)
        {
            handle->error = kInvalidValue;
            failure(game_export::detail::CaptureFailure::api);
        }
        else
        {
            *values = static_cast<int>(router().buffer->available_frames());
        }
    }

    ReleaseSRWLockExclusive(&router().lock);

    return synthetic(device);
}

bool capture_error(void* device, int& result) noexcept
{
    if (!device && opening)
    {
        result = opening_error;
        opening_error = 0;
        return true;
    }

    if (!synthetic(device))
    {
        return false;
    }

    AcquireSRWLockExclusive(&router().lock);
    Handle* handle = find_handle(device);
    if (handle)
    {
        result = handle->used ? handle->error : kInvalidDevice;
        handle->error = 0;
    }
    else if (synthetic(device))
    {
        result = kInvalidDevice;
    }

    ReleaseSRWLockExclusive(&router().lock);

    return synthetic(device);
}
} // namespace cinemania::audio_capture
