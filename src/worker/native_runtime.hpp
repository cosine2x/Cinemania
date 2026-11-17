#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace cinemania::worker
{
struct NativeError
{
    DWORD code = 0;
    std::string message;

    explicit operator bool() const
    {
        return code != 0;
    }
};

class OwnedHandle
{
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle);
    ~OwnedHandle();
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept;
    OwnedHandle& operator=(OwnedHandle&& other) noexcept;
    HANDLE get() const;
    HANDLE release();
    void reset(HANDLE handle = nullptr);

private:
    HANDLE handle_ = nullptr;
};

enum class StopReason
{
    failure,
    cancelled
};

enum class StopOrigin
{
    operation,
    control,
    encoder
};

class StopState
{
public:
    StopState();
    // The first request wins. Callers may safely request concurrently.
    void request(NativeError error,
                 StopReason reason = StopReason::failure,
                 StopOrigin origin = StopOrigin::operation);
    void wake();
    bool stopped() const;
    HANDLE event() const;
    NativeError error() const;
    StopReason reason() const;
    StopOrigin origin() const;
    [[nodiscard]] NativeError commit_if_running(const std::function<NativeError()>& operation);

private:
    OwnedHandle event_;
    mutable std::mutex mutex_;
    std::optional<NativeError> error_;
    StopReason reason_ = StopReason::failure;
    StopOrigin origin_ = StopOrigin::operation;
    bool committed_ = false;
};

using Deadline = std::chrono::steady_clock::time_point;

// Owns an endpoint opened with FILE_FLAG_OVERLAPPED. One operation per endpoint at a time.
class PipeChannel
{
public:
    explicit PipeChannel(HANDLE handle = nullptr);
    void close();
    HANDLE get() const;
    [[nodiscard]] NativeError read_exact(std::span<std::byte> bytes,
                                         StopState& stop,
                                         Deadline deadline,
                                         HANDLE process = nullptr);
    [[nodiscard]] NativeError write_all(std::span<const std::byte> bytes,
                                        StopState& stop,
                                        Deadline deadline,
                                        HANDLE process = nullptr);

private:
    OwnedHandle handle_;
};

[[nodiscard]] NativeError file_write_all(HANDLE file,
                                         std::span<const std::byte> bytes,
                                         std::uint64_t offset,
                                         StopState& stop,
                                         Deadline deadline);

struct EncoderResult
{
    NativeError error;
    DWORD exit_code = 0;
    std::optional<std::uint64_t> frame_count;
    std::string stderr_tail;
};

class EncoderProcess
{
public:
    EncoderProcess();
    ~EncoderProcess();
    EncoderProcess(const EncoderProcess&) = delete;
    EncoderProcess& operator=(const EncoderProcess&) = delete;
    [[nodiscard]] NativeError start(const std::wstring& executable,
                                    const std::vector<std::wstring>& arguments,
                                    StopState& stop);
    [[nodiscard]] NativeError
    write(std::span<const std::byte> bytes, StopState& stop, Deadline deadline);
    void close_stdin();
    [[nodiscard]] NativeError close();
    HANDLE process_handle() const;
    [[nodiscard]] EncoderResult finish(StopState& stop, Deadline deadline);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cinemania::worker
