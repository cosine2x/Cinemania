#include "video_export_internal.hpp"

#include "native_render_ahead.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace cinemania::game_export::detail
{
namespace
{
constexpr std::size_t kTokenCount = 8192;
constexpr std::size_t kSessionCount = 16;
} // namespace

struct Token
{
    std::weak_ptr<Session> session;
    Kind kind = Kind::file;
    bool released = false;
};

struct Registry
{
    std::mutex mutex;
    std::array<Token, kTokenCount> tokens;
    std::size_t next_token = 0;
    std::vector<std::shared_ptr<Session>> sessions;
};

Registry& registry()
{
    // The DLL is pinned. The registry intentionally has no CRT shutdown destructor;
    // sessions and their encoder clients are explicitly retired during DoShoot.
    alignas(Registry) static std::byte storage[sizeof(Registry)];
    static Registry* instance = new (storage) Registry;
    return *instance;
}

struct EmergencyRecord
{
    void* shooter = nullptr;
    void* fiber = nullptr;
};

SRWLOCK emergency_lock = SRWLOCK_INIT;
std::array<EmergencyRecord, 64> emergency_records{};
std::atomic<bool> emergency_overflow = false;

void record_emergency(void* shooter, void* fiber) noexcept
{
    AcquireSRWLockExclusive(&emergency_lock);
    for (const auto& record : emergency_records)
    {
        if (record.shooter == shooter && record.fiber == fiber)
        {
            ReleaseSRWLockExclusive(&emergency_lock);
            return;
        }
    }

    for (auto& record : emergency_records)
    {
        if (!record.fiber)
        {
            record = {shooter, fiber};
            ReleaseSRWLockExclusive(&emergency_lock);
            return;
        }
    }
    emergency_overflow.store(true);
    ReleaseSRWLockExclusive(&emergency_lock);
}

bool emergency_failure(void* shooter, void* fiber) noexcept
{
    bool found = false;
    AcquireSRWLockShared(&emergency_lock);
    for (const auto& record : emergency_records)
    {
        if (record.shooter == shooter && record.fiber == fiber)
        {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&emergency_lock);

    if (found || !emergency_overflow.load() || !shooter)
    {
        return found;
    }
    const auto settings_address =
        *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(shooter) + 0x4c);
    if (settings_address == 0 ||
        *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(shooter) + 0x14) != 0)
    {
        return false;
    }

    const auto* settings = reinterpret_cast<const std::byte*>(settings_address);
    return *reinterpret_cast<const std::uint32_t*>(settings + 0x34) == 0;
}

void clear_emergency(void* shooter, void* fiber) noexcept
{
    AcquireSRWLockExclusive(&emergency_lock);
    for (auto& record : emergency_records)
    {
        if (record.shooter == shooter && record.fiber == fiber)
        {
            record = {};
            break;
        }
    }
    ReleaseSRWLockExclusive(&emergency_lock);
}

std::shared_ptr<Session> find_session(void* shooter, void* fiber)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);
    for (const auto& session : state.sessions)
    {
        if (session->shooter == shooter && session->fiber == fiber)
        {
            return session;
        }
    }

    return {};
}

std::shared_ptr<Session> find_render_ahead_viewport(void* viewport)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);
    for (const auto& session : state.sessions)
    {
        if (session->render_ahead_viewport == viewport)
        {
            return session;
        }
    }
    return {};
}

bool bind_render_ahead_viewport(const std::shared_ptr<Session>& session, void* viewport)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);
    for (const auto& existing : state.sessions)
    {
        if (existing != session && existing->render_ahead_viewport == viewport)
        {
            return false;
        }
    }
    session->render_ahead_viewport = viewport;
    return true;
}

void unbind_render_ahead_viewport(const std::shared_ptr<Session>& session)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);
    session->render_ahead_viewport = nullptr;
}

bool owned(const std::shared_ptr<Session>& session);

void retire(void* shooter, void* fiber)
{
    auto session = find_session(shooter, fiber);
    if (!session || !owned(session))
    {
        return;
    }

    native_render_ahead::retire(session);
    Registry& state = registry();
    {
        std::lock_guard lock(state.mutex);
        const auto it = std::find(state.sessions.begin(), state.sessions.end(), session);
        if (it != state.sessions.end())
        {
            state.sessions.erase(it);
        }
    }

    if (session->encoder && session->state != State::success)
    {
        session->encoder->cancel();
    }
}

Token* token_for(void* handle)
{
    Registry& state = registry();
    const auto value = reinterpret_cast<std::uintptr_t>(handle);
    const auto begin = reinterpret_cast<std::uintptr_t>(state.tokens.data());
    const auto end = begin + sizeof(state.tokens);
    if (value < begin || value >= end || (value - begin) % sizeof(Token) != 0)
    {
        return nullptr;
    }

    return &state.tokens[(value - begin) / sizeof(Token)];
}

std::shared_ptr<Session> lookup(void* handle, Kind kind, bool& synthetic)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);

    Token* token = token_for(handle);
    synthetic = token != nullptr;
    if (!token || token->released || token->kind != kind)
    {
        return {};
    }

    return token->session.lock();
}

void* make_token(const std::shared_ptr<Session>& session, Kind kind)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);
    if (state.next_token == state.tokens.size())
    {
        return nullptr;
    }
    Token& token = state.tokens[state.next_token++];
    token.kind = kind;
    token.session = session;
    token.released = false;
    return &token;
}

bool release_token(void* handle, Kind kind, std::shared_ptr<Session>& session)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);

    Token* token = token_for(handle);
    if (!token)
    {
        return false;
    }

    if (!token->released && token->kind == kind)
    {
        session = token->session.lock();
        token->released = true;
        token->session.reset();
    }
    return true;
}

bool owned(const std::shared_ptr<Session>& session)
{
    if (!session)
    {
        return false;
    }
    if (session->owner != GetCurrentThreadId())
    {
        session->foreign_failure.store(true);
        return false;
    }

    return true;
}

void describe_failure(Session& session, const dll::EncoderResult& failure) noexcept
{
    wchar_t detail[320]{};
    if (!failure.diagnostic.empty())
    {
        const int count = static_cast<int>((std::min)(failure.diagnostic.size(), std::size_t{319}));
        MultiByteToWideChar(CP_UTF8, 0, failure.diagnostic.data(), count, detail, 319);
    }
    for (auto& character : detail)
    {
        if (character == L'%')
        {
            character = L' ';
        }
    }

    if (detail[0])
    {
        _snwprintf_s(session.failure_text,
                     _TRUNCATE,
                     L"Video export failed (%lu): %ls. Check the destination for output.",
                     static_cast<unsigned long>(failure.system_code),
                     detail);
    }
}

void fail(const std::shared_ptr<Session>& session)
{
    if (owned(session) && session->state != State::cancelled)
    {
        session->state = State::failed;
        if (session->encoder)
        {
            describe_failure(*session, session->encoder->failure());
            session->encoder->cancel();
        }
    }
}

FailureOnException::FailureOnException(const std::shared_ptr<Session>& session) noexcept
    : session_(session), exceptions_(std::uncaught_exceptions())
{
}

FailureOnException::~FailureOnException()
{
    if (std::uncaught_exceptions() <= exceptions_)
    {
        return;
    }
    try
    {
        fail(session_);
    }
    catch (...)
    {
        if (session_)
        {
            session_->foreign_failure.store(true);
        }
    }
}

bool valid_producer(const std::shared_ptr<Session>& session)
{
    return owned(session) && session->producer == GetCurrentThreadId();
}

bool register_session(const std::shared_ptr<Session>& session)
{
    Registry& state = registry();
    std::lock_guard lock(state.mutex);

    if (state.sessions.size() >= kSessionCount)
    {
        return false;
    }
    state.sessions.push_back(session);
    return true;
}
} // namespace cinemania::game_export::detail
