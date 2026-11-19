#include "deferred_readback.hpp"

#include "bootstrap_diagnostics.hpp"
#include "game/game_abi.hpp"
#include "video_export_internal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <windows.h>

namespace cinemania::readback
{
using game_abi::executable_image;
using game_abi::field;
using game_abi::function_at;

namespace
{
using game_export::detail::Kind;
using game_export::detail::Session;
using game_export::detail::State;
std::atomic<bool> enabled = false;

std::uint64_t ticks() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::shared_ptr<Session> session_for(void* viewport)
{
    if (!viewport)
    {
        return {};
    }

    auto bound = game_export::detail::find_readback_viewport(viewport);
    if (bound && !bound->readback_active.load())
    {
        return {};
    }
    if (bound && !game_export::detail::valid_producer(bound))
    {
        return bound;
    }

    void* writer = field<void*>(viewport, 0x160);
    if (!writer)
    {
        if (bound)
        {
            bound->foreign_failure.store(true);
        }
        return bound;
    }

    void* context = field<void*>(writer, 4);
    if (!context)
    {
        if (bound)
        {
            bound->foreign_failure.store(true);
        }
        return bound;
    }

    bool synthetic = false;
    auto session =
        game_export::detail::lookup(field<void*>(context, 8), Kind::compressed, synthetic);
    if (!session || !synthetic || (bound && bound != session))
    {
        if (bound)
        {
            bound->foreign_failure.store(true);
        }
        return bound;
    }

    bool raw_synthetic = false;
    bool file_synthetic = false;
    auto raw = game_export::detail::lookup(field<void*>(context, 4), Kind::raw, raw_synthetic);
    auto file = game_export::detail::lookup(field<void*>(context, 0), Kind::file, file_synthetic);
    if (!raw_synthetic || !file_synthetic || raw != session || file != session)
    {
        session->foreign_failure.store(true);
        return bound ? bound : session;
    }

    return session;
}

bool deliver(Session& session, void* viewport, bool wait)
{
    const ULONGLONG deadline = GetTickCount64() + 10000;
    bool flush = wait;
    for (;;)
    {
        if (session.foreign_failure.load() || session.state != State::recording ||
            session.encoder->has_failed())
        {
            session.readback_failure_stage = "delivery-state";
            return false;
        }

        auto* pixels = static_cast<std::byte*>(field<void*>(viewport, 0x1610));
        if (!pixels)
        {
            session.readback_failure_stage = "delivery-buffer";
            return false;
        }

        std::uint64_t index = 0;
        const HRESULT result =
            session.readback.retire(std::span(pixels, session.frame_bytes), index, flush);
        flush = false;
        if (result == S_OK)
        {
            if (index != session.retired || index != static_cast<std::uint64_t>(session.next_frame))
            {
                session.readback_failure_stage = "delivery-index";
                return false;
            }

            using Write = unsigned int(__thiscall*)(void*, void*);

            const auto writer = function_at<Write>(executable_image(), 0x55a0f0);
            if (session_for(viewport).get() != &session || session.foreign_failure.load() ||
                field<std::uint32_t>(field<void*>(session.readback_writer, 4), 0x40) != index)
            {
                session.readback_failure_stage = "delivery-context";
                return false;
            }

            const auto start = ticks();
            const unsigned int written = writer(session.readback_writer, pixels);
            session.submit_ticks += ticks() - start;
            if (written == 0 || session.next_frame != static_cast<LONG>(index + 1))
            {
                session.readback_failure_stage = "writer";
                return false;
            }

            ++session.retired;
            return true;
        }
        if (result != S_FALSE || !wait || GetTickCount64() >= deadline)
        {
            if (result != S_FALSE || wait)
            {
                session.readback_failure_stage = result == S_FALSE ? "delivery-timeout" : "retire";
                session.readback_failure_code = result;
            }
            return result == S_FALSE && !wait;
        }
        Sleep(1);
    }
}

void fail(Session& session, const std::shared_ptr<Session>& lease)
{
    game_export::detail::fail(lease);
    session.readback.reset();
}

struct CaptureSource
{
    IDirect3DDevice9* device;
    IDirect3DSurface9* surface;
    IDirect3DSurface9* resolve;
    D3DSURFACE_DESC description;
};

bool activate(const std::shared_ptr<Session>& session, void* viewport, const CaptureSource& source)
{
    const auto setup_start = ticks();
    if (session->next_frame != 0 || session->captured != 0 || session->retired != 0)
    {
        return false;
    }

    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(source.device->GetCreationParameters(&creation)) ||
        FAILED(session->readback.initialize(source.device,
                                            source.description.Width,
                                            source.description.Height,
                                            source.description.Format)))
    {
        return false;
    }

    if (!game_export::detail::bind_readback_viewport(session, viewport))
    {
        session->readback.reset();
        return false;
    }

    session->readback_writer = field<void*>(viewport, 0x160);
    session->readback_context = field<void*>(session->readback_writer, 4);
    session->readback_source = source.surface;
    session->readback_resolve = source.resolve;
    session->readback_format = source.description.Format;
    session->readback_depth = session->readback.depth();
    session->device_flags = creation.BehaviorFlags;
    session->start_ticks = setup_start;
    session->setup_ticks = ticks() - setup_start;
    session->readback_active = true;
    return true;
}

bool query_source(const Session& session, void* viewport, CaptureSource& source)
{
    source.device = static_cast<IDirect3DDevice9*>(field<void*>(viewport, 0x9f8));
    using StackEntry = void*(__thiscall*)(void*, unsigned int);

    const auto stack_entry = function_at<StackEntry>(executable_image(), 0x3ac1e0);
    void* entry = stack_entry(static_cast<std::byte*>(viewport) + 0x9fc, 0);
    source.surface = entry ? static_cast<IDirect3DSurface9*>(field<void*>(entry, 0x10)) : nullptr;
    source.resolve = static_cast<IDirect3DSurface9*>(field<void*>(viewport, 0x160c));

    if (session.readback_active &&
        (!session.readback.matches(source.device,
                                   static_cast<UINT>(session.width),
                                   static_cast<UINT>(session.height),
                                   session.readback_format) ||
         session.readback_source != source.surface || session.readback_resolve != source.resolve))
    {
        return false;
    }

    auto& description = source.description;
    if (!source.device || !source.surface || FAILED(source.surface->GetDesc(&description)) ||
        description.Width != static_cast<UINT>(session.width) ||
        description.Height != static_cast<UINT>(session.height) ||
        (description.Format != D3DFMT_X8R8G8B8 && description.Format != D3DFMT_A8R8G8B8) ||
        (!source.resolve && description.MultiSampleType != D3DMULTISAMPLE_NONE))
    {
        return false;
    }

    if (source.resolve)
    {
        D3DSURFACE_DESC resolved{};
        if (FAILED(source.resolve->GetDesc(&resolved)) || resolved.Width != description.Width ||
            resolved.Height != description.Height || resolved.Format != description.Format ||
            resolved.MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            return false;
        }
    }
    return true;
}

bool issue_frame(Session& session, void* viewport, const CaptureSource& source)
{
    if (!session.readback.matches(source.device,
                                  source.description.Width,
                                  source.description.Height,
                                  source.description.Format))
    {
        return false;
    }
    if (session.readback.full())
    {
        ++session.pressure_count;
        if (!deliver(session, viewport, true))
        {
            return false;
        }
    }

    IDirect3DSurface9* transfer_source = source.surface;
    if (source.resolve)
    {
        const HRESULT result = source.device->StretchRect(
            source.surface, nullptr, source.resolve, nullptr, D3DTEXF_NONE);
        if (FAILED(result))
        {
            session.readback_failure_stage = "resolve";
            session.readback_failure_code = result;
            return false;
        }
        transfer_source = source.resolve;
    }

    const HRESULT result = session.readback.issue(transfer_source, session.captured);
    if (FAILED(result))
    {
        session.readback_failure_stage = "transfer-or-query";
        session.readback_failure_code = result;
        return false;
    }

    ++session.captured;
    const auto pending = session.captured - session.retired;
    if (pending > session.max_pending)
    {
        session.max_pending = pending;
    }
    if (pending > 1 && !deliver(session, viewport, false))
    {
        return false;
    }
    return true;
}

bool run_capture(void* viewport, const std::shared_ptr<Session>& session)
{
    if (!game_export::detail::valid_producer(session))
    {
        return session->readback_active;
    }
    if (session->readback_active &&
        (session->readback_writer != field<void*>(viewport, 0x160) ||
         session->readback_context != field<void*>(session->readback_writer, 4)))
    {
        fail(*session, session);
        return true;
    }
    if (session->state != State::recording || session->foreign_failure.load())
    {
        if (session->readback_active)
        {
            fail(*session, session);
            return true;
        }
        return false;
    }
    if (session->readback_disabled)
    {
        return false;
    }
    if (field<std::uint32_t>(viewport, 0x150) != 0 || field<std::uint32_t>(viewport, 0x558) != 0)
    {
        if (session->readback_active)
        {
            fail(*session, session);
            return true;
        }
        session->readback_disabled = true;
        return false;
    }

    CaptureSource source{};
    if (!query_source(*session, viewport, source))
    {
        if (session->readback_active)
        {
            fail(*session, session);
            return true;
        }

        session->readback_disabled = true;
        return false;
    }

    if (!session->readback_active && !activate(session, viewport, source))
    {
        session->readback_disabled = true;
        return false;
    }

    if (!issue_frame(*session, viewport, source))
    {
        fail(*session, session);
    }
    return true;
}
} // namespace

void enable() noexcept
{
    wchar_t value[2]{};
    const DWORD length = GetEnvironmentVariableW(L"CINEMANIA_DEFERRED_READBACK", value, 2);
    enabled.store(length != 1 || value[0] != L'0');
}

bool capture(void* viewport) noexcept
{
    if (!enabled.load())
    {
        return false;
    }
    std::shared_ptr<Session> session;
    try
    {
        session = session_for(viewport);
        return session && run_capture(viewport, session);
    }
    catch (...)
    {
        if (session)
        {
            session->foreign_failure.store(true);
            return session->readback_active;
        }
        return false;
    }
}

void cleanup(void* viewport) noexcept
{
    if (!enabled.load() || !viewport || field<std::uint32_t>(viewport, 0x550) == 0)
    {
        return;
    }
    std::shared_ptr<Session> session;
    try
    {
        session = session_for(viewport);
        if (!session || !session->readback_active || !game_export::detail::valid_producer(session))
        {
            return;
        }
        const auto drain_start = ticks();
        if (session->state == State::recording)
        {
            while (!session->readback.empty())
            {
                if (!deliver(*session, viewport, true))
                {
                    game_export::detail::fail(session);
                    break;
                }
            }
            if (session->captured != session->retired)
            {
                game_export::detail::fail(session);
            }
        }
        session->drain_ticks += ticks() - drain_start;
        const auto& stats = session->readback.stats();
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        char message[512]{};
        std::snprintf(
            message,
            sizeof(message),
            "readback size=%ldx%ld format=%u device_flags=%lu state=%u qpc_hz=%lld "
            "elapsed_ticks=%llu setup_ticks=%llu frames=%llu retired=%llu transfers=%llu "
            "locks=%llu "
            "transfer_ticks=%llu lock_ticks=%llu convert_ticks=%llu submit_ticks=%llu "
            "pressure=%llu max_pending=%llu depth=%zu drain_ticks=%llu stage=%s hr=%08lx\n",
            session->width,
            session->height,
            static_cast<unsigned int>(session->readback_format),
            session->device_flags,
            static_cast<unsigned int>(session->state),
            frequency.QuadPart,
            ticks() - session->start_ticks,
            session->setup_ticks,
            session->captured,
            session->retired,
            stats.transfers,
            stats.locks,
            stats.transfer_ticks,
            stats.lock_ticks,
            stats.conversion_ticks,
            session->submit_ticks,
            session->pressure_count,
            session->max_pending,
            session->readback_depth,
            session->drain_ticks,
            session->readback_failure_stage ? session->readback_failure_stage : "none",
            static_cast<unsigned long>(session->readback_failure_code));
        cinemania::append_diagnostic(message);
        session->readback.reset();
        session->readback_active.store(false);
        session->readback_disabled = true;
        game_export::detail::unbind_readback_viewport(session);
        session->readback_writer = nullptr;
        session->readback_context = nullptr;
        session->readback_source = nullptr;
        session->readback_resolve = nullptr;
    }
    catch (...)
    {
        if (session)
        {
            session->foreign_failure.store(true);
            if (game_export::detail::valid_producer(session))
            {
                try
                {
                    fail(*session, session);
                }
                catch (...)
                {
                    session->readback.reset();
                }
            }
        }
        OutputDebugStringW(L"Cinemania: deferred readback cleanup exception.\n");
    }
}

void device_change(void* viewport) noexcept
{
    if (!enabled.load())
    {
        return;
    }
    std::shared_ptr<Session> session;
    try
    {
        session = session_for(viewport);
        if (session && session->readback_active && game_export::detail::valid_producer(session) &&
            session->state != State::success)
        {
            fail(*session, session);
        }
    }
    catch (...)
    {
        if (session)
        {
            session->foreign_failure.store(true);
            if (game_export::detail::valid_producer(session))
            {
                session->readback.reset();
            }
        }
        OutputDebugStringW(L"Cinemania: deferred readback lifecycle exception.\n");
    }
}
} // namespace cinemania::readback
