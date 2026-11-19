#pragma once

#include <memory>

namespace cinemania::game_export::detail
{
struct Session;
enum class CaptureFailure;
} // namespace cinemania::game_export::detail

namespace cinemania::audio_capture
{
using Session = game_export::detail::Session;

// All control and fake ALC consumer calls run on the session owner thread.
// Mixer begin/finish calls may run concurrently; detach waits for bounded copies.
bool bind(const std::shared_ptr<Session>& session, void* mixer_context) noexcept;
void detach(const std::shared_ptr<Session>& session) noexcept;
void record_failure(Session& session, game_export::detail::CaptureFailure failure) noexcept;
bool is_synthetic(void* device) noexcept;
// The identity is an opaque, never-reused handle; it carries no borrowed
// session or buffer lifetime across the foreign mixer call.
void* begin_mix(void* context) noexcept;
void finish_mix(
    void* identity, void* context, const void* bytes, int byte_count, int format) noexcept;

// A scope covers only the original SetCapture call, preventing an owned open
// from ever falling through to the system input device.
void enter_open(const std::shared_ptr<Session>& session) noexcept;
void leave_open() noexcept;
void* capture_open(
    const char* name, unsigned int frequency, int format, int capacity, bool& handled) noexcept;
bool capture_close(void* device, char& result) noexcept;
bool capture_start(void* device) noexcept;
bool capture_stop(void* device) noexcept;
bool capture_samples(void* device, void* destination, int frames) noexcept;
bool capture_integer(void* device, int parameter, int count, int* values) noexcept;
bool capture_error(void* device, int& result) noexcept;
} // namespace cinemania::audio_capture
