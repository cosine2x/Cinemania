#pragma once

#include "encoder_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <windows.h>

namespace cinemania::game_export::detail
{
enum class Kind
{
    file,
    raw,
    compressed
};
enum class State
{
    preparing,
    recording,
    failed,
    cancelled,
    success
};

// Mutable fields belong to the Open thread. Registry lookups retain shared leases;
// foreign callbacks signal failure only through the atomic flag.
struct Session
{
    void* shooter = nullptr;
    void* fiber = nullptr;
    State state = State::preparing;
    std::wstring path;
    std::unique_ptr<dll::EncoderClient> encoder;

    DWORD producer = 0;
    DWORD owner = 0;
    std::atomic<bool> foreign_failure = false;

    wchar_t failure_text[512] = L"Video export failed. Check the destination for any output.";

    // Producer-owned native ring state; viewport binding is protected by the registry mutex.
    void* render_ahead_viewport = nullptr;
    void* render_ahead_device = nullptr;
    std::uint32_t render_ahead_third = 0;
    std::uint64_t render_ahead_start_ticks = 0;
    LONG render_ahead_start_frame = 0;
    std::atomic<bool> render_ahead_active = false;
    std::atomic<bool> render_ahead_invalidated = false;
    bool render_ahead_attempted = false;

    LONG width = 0;
    LONG height = 0;
    std::uint32_t rate = 0;
    std::uint32_t scale = 0;
    std::size_t frame_bytes = 0;
    LONG next_frame = 0;
    unsigned int streams = 0;
};

void record_emergency(void* shooter, void* fiber) noexcept;
bool emergency_failure(void* shooter, void* fiber) noexcept;
void clear_emergency(void* shooter, void* fiber) noexcept;
std::shared_ptr<Session> find_session(void* shooter, void* fiber);
std::shared_ptr<Session> find_render_ahead_viewport(void* viewport);
bool bind_render_ahead_viewport(const std::shared_ptr<Session>& session, void* viewport);
void unbind_render_ahead_viewport(const std::shared_ptr<Session>& session);
void retire(void* shooter, void* fiber);
bool register_session(const std::shared_ptr<Session>& session);
std::shared_ptr<Session> lookup(void* handle, Kind kind, bool& synthetic);
void* make_token(const std::shared_ptr<Session>& session, Kind kind);
bool release_token(void* handle, Kind kind, std::shared_ptr<Session>& session);
bool owned(const std::shared_ptr<Session>& session);
bool valid_producer(const std::shared_ptr<Session>& session);
void describe_failure(Session& session, const dll::EncoderResult& failure) noexcept;
void fail(const std::shared_ptr<Session>& session);

class FailureOnException
{
public:
    explicit FailureOnException(const std::shared_ptr<Session>& session) noexcept;
    ~FailureOnException();

private:
    const std::shared_ptr<Session>& session_;
    int exceptions_;
};
} // namespace cinemania::game_export::detail
