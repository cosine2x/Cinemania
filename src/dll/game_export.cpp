#include "game_export.hpp"

#include "game_abi.hpp"
#include "openal_capture.hpp"
#include "openal_capture_hooks.hpp"
#include "video_export_internal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace cinemania::game_export
{
using game_abi::executable_image;
using game_abi::field;
using game_abi::function_at;

namespace detail
{
constexpr wchar_t kFailure[] = L"Video export failed. Check the destination for any output.";
constexpr wchar_t kCancelled[] = L"Video export cancelled. Check the destination for any output.";
thread_local Invocation* invocation = nullptr;
// Bootstrap publishes this only after the entire hook transaction succeeds.
std::atomic<bool> video_routing_ready = false;

std::shared_ptr<Session> current_session()
{
    if (!invocation || !invocation->key_)
    {
        return {};
    }
    return find_session(invocation->shooter_, invocation->key_);
}

enum class PathStatus
{
    available,
    occupied,
    inaccessible
};

PathStatus path_status(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return PathStatus::occupied;
    }
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        return PathStatus::available;
    }
    return PathStatus::inaccessible;
}

std::wstring choose_path(const wchar_t* input, std::size_t length)
{
    std::wstring stem(input, length);

    const auto slash = stem.find_last_of(L"\\/");
    const auto dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
    {
        stem.resize(dot);
    }

    for (unsigned int index = 0; index < 10000; ++index)
    {
        std::wstring candidate = stem;
        if (index != 0)
        {
            candidate += L"_" + std::to_wstring(index);
        }
        candidate += L".mp4";
        const PathStatus status = path_status(candidate);
        if (status == PathStatus::available)
        {
            return candidate;
        }
        if (status == PathStatus::inaccessible)
        {
            return {};
        }
    }
    return {};
}

struct WideString
{
    std::uint32_t length;
    wchar_t* text;
};

struct WideInput
{
    const wchar_t* text;
    std::uint32_t length;
    std::uint32_t convert;
};

constexpr std::size_t kAssignWideRva = 0x503280;
using AssignWide = void(__thiscall*)(WideString*, const WideInput*);

} // namespace detail

using detail::FailureOnException;
using detail::Session;
using detail::State;

Invocation::Invocation(void* shooter, void** fiber) noexcept
    : previous_(detail::invocation), shooter_(shooter), fiber_(fiber)
{
    if (fiber && *fiber && !game_abi::fiber_finished(*fiber))
    {
        key_ = *fiber;
    }

    detail::invocation = this;
}

Invocation::~Invocation()
{
    detail::invocation = static_cast<Invocation*>(previous_);

    if (fiber_ && game_abi::fiber_finished(*fiber_) && key_)
    {
        try
        {
            detail::clear_emergency(shooter_, key_);
            detail::retire(shooter_, key_);
        }
        catch (...)
        {
            OutputDebugStringW(L"Cinemania: session retirement failed.\n");
        }
    }
}

void enable_video_routing() noexcept
{
    detail::video_routing_ready.store(true);
}

bool open(void* writer, void* path, const void* caller, unsigned int& result)
{
    if (!detail::video_routing_ready.load())
    {
        return false;
    }

    if (!detail::invocation || !detail::invocation->shooter_ || !writer || !path ||
        !detail::invocation->fiber_ || caller != executable_image() + 0x2f4777)
    {
        return false;
    }

    void* fiber = *detail::invocation->fiber_;
    if (!fiber || game_abi::fiber_finished(fiber) ||
        path != static_cast<std::byte*>(fiber) + 0x34 ||
        field<std::uint32_t>(detail::invocation->shooter_, 0x14) != 0)
    {
        return false;
    }

    auto* settings = field<void*>(detail::invocation->shooter_, 0x4c);
    if (!settings)
    {
        return false;
    }

    detail::invocation->key_ = fiber;
    detail::record_emergency(detail::invocation->shooter_, fiber);
    auto session = std::make_shared<Session>();
    session->shooter = detail::invocation->shooter_;
    session->fiber = fiber;
    session->owner = GetCurrentThreadId();
    session->audio_expected = field<std::uint32_t>(settings, 0x34) != 0;
    if (!detail::register_session(session))
    {
        result = 0;
        return true;
    }

    detail::clear_emergency(detail::invocation->shooter_, fiber);
    FailureOnException failure_guard(session);
    result = 0;

    if (session->audio_expected && !openal_capture_hooks::prepare_mixer())
    {
        audio_capture::record_failure(*session, detail::CaptureFailure::source);
        detail::fail(session);
        return true;
    }

    auto* game_path = static_cast<detail::WideString*>(path);
    if (!game_path->text || game_path->length == 0 || game_path->length > 32760 ||
        wcsnlen_s(game_path->text, game_path->length) != game_path->length)
    {
        detail::fail(session);
        return true;
    }

    session->path = detail::choose_path(game_path->text, game_path->length);
    if (session->path.empty())
    {
        detail::fail(session);
        return true;
    }

    auto* context = field<void**>(writer, 4);
    constexpr unsigned char assign_prologue[] = {0x53, 0x8b, 0x5c, 0x24, 0x08};
    if (!context || *context ||
        std::memcmp(executable_image() + detail::kAssignWideRva,
                    assign_prologue,
                    sizeof(assign_prologue)) != 0)
    {
        detail::fail(session);
        return true;
    }

    void* file = detail::make_token(session, detail::Kind::file);
    if (!file)
    {
        detail::fail(session);
        return true;
    }

    detail::WideInput input{
        session->path.c_str(), static_cast<std::uint32_t>(session->path.size()), 0};
    function_at<detail::AssignWide>(executable_image(), detail::kAssignWideRva)(game_path, &input);
    *context = file;
    result = 1;
    return true;
}

unsigned int abort_result(void* shooter, unsigned int original)
{
    if (!detail::invocation || detail::invocation->shooter_ != shooter || !detail::invocation->key_)
    {
        return original;
    }

    if (detail::emergency_failure(shooter, detail::invocation->key_))
    {
        return 1;
    }

    auto session = detail::find_session(shooter, detail::invocation->key_);
    if (!session)
    {
        return original;
    }

    if (!detail::owned(session))
    {
        return 1;
    }

    FailureOnException failure_guard(session);
    if (session->foreign_failure.load())
    {
        detail::fail(session);
    }
    if (session->encoder && session->state == State::recording && session->encoder->has_failed())
    {
        detail::fail(session);
    }
    if (original && session->state != State::failed)
    {
        audio_capture::detach(session);
        session->state = State::cancelled;
        if (session->encoder)
        {
            session->encoder->cancel();
        }
    }
    return original || session->state == State::failed;
}

const wchar_t* message(const wchar_t* key, const void* caller, const wchar_t* original)
{
    if (!detail::invocation || !detail::invocation->key_)
    {
        return original;
    }

    const bool emergency =
        detail::emergency_failure(detail::invocation->shooter_, detail::invocation->key_);
    const auto image_base = reinterpret_cast<std::uintptr_t>(executable_image());
    const auto return_rva = reinterpret_cast<std::uintptr_t>(caller) - image_base;
    const auto literal = reinterpret_cast<std::uintptr_t>(key) - image_base;
    const bool final_message =
        (return_rva == 0x2f52f2 || return_rva == 0x2f540e) && literal == 0x78303c;
    const bool abort_message =
        (return_rva == 0x2f4c63 || return_rva == 0x2f5149) && literal == 0x7830e8;
    const bool setup_message =
        (return_rva == 0x2f4a6e || return_rva == 0x2f4b83) && literal == 0x783118;
    if (!final_message && !abort_message && !setup_message)
    {
        return original;
    }

    if (emergency)
    {
        return detail::kFailure;
    }

    auto session = detail::find_session(detail::invocation->shooter_, detail::invocation->key_);
    if (session && !detail::owned(session))
    {
        return detail::kFailure;
    }

    if (!session)
    {
        return original;
    }

    if (session->state == State::cancelled)
    {
        return detail::kCancelled;
    }
    if (session->state == State::failed || (final_message && session->state != State::success))
    {
        return session->failure_text;
    }
    return original;
}

void exception() noexcept
{
    try
    {
        if (detail::invocation && detail::invocation->key_)
        {
            detail::fail(
                detail::find_session(detail::invocation->shooter_, detail::invocation->key_));
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"Cinemania: could not record callback failure.\n");
    }
}
} // namespace cinemania::game_export
