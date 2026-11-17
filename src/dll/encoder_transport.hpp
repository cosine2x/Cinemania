#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace cinemania::dll
{
class EncoderTransport
{
public:
    EncoderTransport();
    ~EncoderTransport();
    EncoderTransport(const EncoderTransport&) = delete;
    EncoderTransport& operator=(const EncoderTransport&) = delete;

    [[nodiscard]] DWORD launch(const std::wstring& scratch, std::uint64_t session);
    [[nodiscard]] DWORD write_data(std::span<const std::byte> bytes, DWORD timeout);
    [[nodiscard]] DWORD write_control(std::span<const std::byte> bytes, DWORD timeout);
    [[nodiscard]] DWORD read_status(std::span<std::byte> bytes, DWORD timeout);
    void stop();
    void cancel_if_unlaunched();
    [[nodiscard]] DWORD wait(DWORD timeout, DWORD& exit_code) const;
    [[nodiscard]] HANDLE stopped_event() const;
    [[nodiscard]] bool job_empty() const;

private:
    [[nodiscard]] DWORD
    transfer(HANDLE pipe, void* bytes, std::size_t size, bool write, DWORD timeout);

    // Handles are published to concurrent launch, I/O and cancellation paths.
    std::atomic<HANDLE> data_{nullptr};
    std::atomic<HANDLE> control_{nullptr};
    std::atomic<HANDLE> status_{nullptr};

    std::atomic<HANDLE> process_{nullptr};
    std::atomic<HANDLE> job_{nullptr};

    HANDLE completion_port_ = nullptr;
    HANDLE stopped_ = nullptr;

    HANDLE data_event_ = nullptr;
    HANDLE control_event_ = nullptr;
    HANDLE status_event_ = nullptr;
};
} // namespace cinemania::dll
