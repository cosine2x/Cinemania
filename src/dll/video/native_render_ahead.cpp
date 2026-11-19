#include "native_render_ahead.hpp"

#include "bootstrap_diagnostics.hpp"
#include "game/game_abi.hpp"
#include "video_export_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <windows.h>

namespace cinemania::native_render_ahead
{
using game_abi::executable_image;
using game_abi::field;
using game_abi::function_at;

namespace
{
using game_export::detail::Kind;
using game_export::detail::Session;
using game_export::detail::State;

// TMUF 2.12.0 x86 viewport ABI; the startup transaction validates the target entries.
constexpr std::size_t kViewportVtable = 0x7d10bc;
constexpr std::size_t kCapability = 0x9777ac;
constexpr std::size_t kConfigure = 0x58bab0;
constexpr std::size_t kRelease = 0x58b6e0;
constexpr std::size_t kInitialize = 0x58b6d0;
constexpr std::size_t kCount = 0x930;
constexpr std::size_t kEntries = 0x934;
constexpr std::size_t kIssue = 0x93c;
constexpr std::size_t kRetire = 0x940;
constexpr std::size_t kPolicy = 0x944;
constexpr std::size_t kThird = 0x948;
constexpr std::size_t kDevice = 0x9f8;
constexpr std::size_t kCaptureStop = 0x550;
constexpr std::size_t kCaptureActive = 0x554;
constexpr std::size_t kWriter = 0x160;
constexpr std::size_t kMirrorCount = 0x2c4;
constexpr std::size_t kMirrorPolicy = 0x2d4;
constexpr std::size_t kMirrorThird = 0x2d8;

struct Entry
{
    void* query;
    void* surface;
    std::uint32_t pending;
    std::uint32_t unknown;
    std::uint32_t timestamp_low;
    std::uint32_t timestamp_high;
};

static_assert(sizeof(Entry) == 24);

using Configure = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
using NoArguments = void(__fastcall*)(void*);
enum class RestoreCause
{
    normal,
    partial_apply
};
thread_local bool changing = false;

bool settings_match(void* viewport, std::uint32_t count, std::uint32_t third)
{
    return field<std::uint32_t>(viewport, kCount) == count &&
           field<std::uint32_t>(viewport, kPolicy) == 1 &&
           field<std::uint32_t>(viewport, kThird) == third &&
           field<std::uint32_t>(viewport, kMirrorCount) == count &&
           field<std::uint32_t>(viewport, kMirrorPolicy) == 1 &&
           field<std::uint32_t>(viewport, kMirrorThird) == third;
}

std::shared_ptr<Session> capture_session(void* viewport)
{
    void* writer = field<void*>(viewport, kWriter);
    if (!writer)
    {
        return {};
    }

    void* context = field<void*>(writer, 4);
    if (!context)
    {
        return {};
    }

    bool synthetic = false;
    auto session =
        game_export::detail::lookup(field<void*>(context, 8), Kind::compressed, synthetic);
    if (!session || !synthetic)
    {
        return {};
    }

    bool raw_synthetic = false;
    bool file_synthetic = false;
    auto raw = game_export::detail::lookup(field<void*>(context, 4), Kind::raw, raw_synthetic);
    auto file = game_export::detail::lookup(field<void*>(context, 0), Kind::file, file_synthetic);
    if (!raw_synthetic || !file_synthetic || raw != session || file != session)
    {
        return {};
    }

    return session;
}

void report(const Session& session, const char* outcome) noexcept
{
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const auto ticks = static_cast<std::uint64_t>(now.QuadPart) - session.render_ahead_start_ticks;
    char message[256]{};
    std::snprintf(message,
                  sizeof(message),
                  "native render-ahead: %s count=1->2 thread=%lu frames=%ld ticks=%llu hz=%lld\n",
                  outcome,
                  static_cast<unsigned long>(GetCurrentThreadId()),
                  session.next_frame - session.render_ahead_start_frame,
                  static_cast<unsigned long long>(ticks),
                  frequency.QuadPart);
    append_diagnostic(message);
}

void clear(const std::shared_ptr<Session>& session)
{
    session->render_ahead_active = false;
    session->render_ahead_device = nullptr;
    game_export::detail::unbind_render_ahead_viewport(session);
}

void fail_session(const std::shared_ptr<Session>& session) noexcept
{
    session->foreign_failure.store(true);
    try
    {
        game_export::detail::fail(session);
    }
    catch (...)
    {
        OutputDebugStringW(L"Cinemania: native render-ahead failure reporting exception.\n");
    }
}

void restore(const std::shared_ptr<Session>& session, void* viewport, RestoreCause cause)
{
    if (!session->render_ahead_active)
    {
        return;
    }

    if (!game_export::detail::valid_producer(session))
    {
        // The owner must disarm without dereferencing a possibly destroyed viewport.
        session->render_ahead_invalidated.store(true);
        session->foreign_failure.store(true);
        return;
    }

    if (session->render_ahead_invalidated.load())
    {
        clear(session);
        append_diagnostic("native render-ahead: foreign lifecycle invalidated ownership; "
                          "restoration unavailable\n");
        return;
    }

    if (cause == RestoreCause::normal && !settings_match(viewport, 2, session->render_ahead_third))
    {
        // The engine or user has taken ownership of the settings.
        clear(session);
        append_diagnostic("native render-ahead: settings changed externally; ownership released\n");
        return;
    }

    if (field<void*>(viewport, kDevice) != session->render_ahead_device ||
        (field<std::uint32_t>(executable_image(), kCapability) & 2) == 0)
    {
        fail_session(session);
        clear(session);
        append_diagnostic("native render-ahead: device changed before restoration\n");
        return;
    }

    changing = true;
    function_at<NoArguments>(executable_image(), kRelease)(viewport);
    function_at<Configure>(executable_image(),
                           kConfigure)(viewport, 0, 1, session->render_ahead_third);
    function_at<NoArguments>(executable_image(), kInitialize)(viewport);
    changing = false;

    if (!settings_match(viewport, 1, session->render_ahead_third) ||
        field<std::uint32_t>(viewport, kIssue) != 0 || field<std::uint32_t>(viewport, kRetire) != 0)
    {
        fail_session(session);
        append_diagnostic("native render-ahead: restoration validation failed\n");
    }
    else
    {
        report(*session, "restored one-slot ring");
    }

    clear(session);
}

void capture_impl(void* viewport, std::shared_ptr<Session>& session)
{
    if (changing || !viewport || field<std::uint32_t>(viewport, kCaptureActive) != 1 ||
        !game_abi::has_vtable(viewport, executable_image() + kViewportVtable) ||
        !field<void*>(viewport, kDevice) ||
        (field<std::uint32_t>(executable_image(), kCapability) & 2) == 0)
    {
        return;
    }

    session = game_export::detail::find_render_ahead_viewport(viewport);
    if (session)
    {
        if (session->render_ahead_invalidated.load() &&
            game_export::detail::valid_producer(session))
        {
            clear(session);
        }

        return;
    }

    session = capture_session(viewport);
    if (!session || !game_export::detail::valid_producer(session) ||
        session->state != State::recording || session->foreign_failure.load() ||
        session->render_ahead_attempted)
    {
        return;
    }

    const auto third = field<std::uint32_t>(viewport, kThird);
    auto* entries = static_cast<Entry*>(field<void*>(viewport, kEntries));
    if (!settings_match(viewport, 1, third))
    {
        session->render_ahead_attempted = true;
        append_diagnostic("native render-ahead: skipped nonstandard ring settings\n");
        return;
    }

    if (!entries || field<std::uint32_t>(viewport, kIssue) != 0 ||
        field<std::uint32_t>(viewport, kRetire) != 0 || entries[0].pending != 0 ||
        !game_export::detail::bind_render_ahead_viewport(session, viewport))
    {
        return;
    }

    session->render_ahead_attempted = true;
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    session->render_ahead_start_ticks = static_cast<std::uint64_t>(start.QuadPart);
    session->render_ahead_start_frame = session->next_frame;
    session->render_ahead_device = field<void*>(viewport, kDevice);
    session->render_ahead_third = third;
    session->render_ahead_active = true;

    changing = true;
    function_at<Configure>(executable_image(), kConfigure)(viewport, 1, 1, third);
    changing = false;

    if (!settings_match(viewport, 2, third) || field<std::uint32_t>(viewport, kIssue) != 0 ||
        field<std::uint32_t>(viewport, kRetire) != 0)
    {
        fail_session(session);
        append_diagnostic(
            "native render-ahead: apply validation failed; restoring original ring\n");
        restore(session, viewport, RestoreCause::partial_apply);
        return;
    }

    report(*session, "activated two-slot ring");
}
} // namespace

void capture(void* viewport) noexcept
{
    std::shared_ptr<Session> session;
    try
    {
        capture_impl(viewport, session);
    }
    catch (...)
    {
        changing = false;
        if (session)
        {
            fail_session(session);
            if (session->render_ahead_active.load())
            {
                try
                {
                    restore(session, viewport, RestoreCause::partial_apply);
                }
                catch (...)
                {
                    changing = false;
                    session->render_ahead_invalidated.store(true);
                    append_diagnostic(
                        "native render-ahead: exception left restoration unresolved\n");
                }
            }
        }

        OutputDebugStringW(L"Cinemania: native render-ahead capture exception.\n");
    }
}

void lifecycle(void* viewport, bool device_lost) noexcept
{
    std::shared_ptr<Session> session;
    try
    {
        session = game_export::detail::find_render_ahead_viewport(viewport);
        if (session)
        {
            restore(session, viewport, RestoreCause::normal);
            if (device_lost)
            {
                fail_session(session);
            }
        }
    }
    catch (...)
    {
        changing = false;
        if (session)
        {
            fail_session(session);
            try
            {
                restore(session, viewport, RestoreCause::partial_apply);
            }
            catch (...)
            {
                changing = false;
                session->render_ahead_invalidated.store(true);
                append_diagnostic(
                    "native render-ahead: cleanup exception left restoration unresolved\n");
            }
        }

        OutputDebugStringW(L"Cinemania: native render-ahead cleanup exception.\n");
    }
}

void cleanup(void* viewport) noexcept
{
    if (!viewport || changing || field<std::uint32_t>(viewport, kCaptureStop) == 0)
    {
        return;
    }

    lifecycle(viewport, false);
}

void device_change(void* viewport) noexcept
{
    if (!viewport || changing)
    {
        return;
    }

    lifecycle(viewport, true);
}

void retire(const std::shared_ptr<Session>& session) noexcept
{
    if (!session || !session->render_ahead_active)
    {
        return;
    }

    try
    {
        restore(session, session->render_ahead_viewport, RestoreCause::normal);
    }
    catch (...)
    {
        changing = false;
        session->foreign_failure.store(true);
        OutputDebugStringW(L"Cinemania: native render-ahead retirement exception.\n");
    }
}
} // namespace cinemania::native_render_ahead
