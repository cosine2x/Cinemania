#pragma once

#include "../common/protocol.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace cinemania::worker
{
struct SessionFormat
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_numerator = 0;
    std::uint32_t fps_denominator = 0;
    std::uint32_t stride = 0;
    std::uint32_t crf = 0;
    std::uint32_t threads = 0;
    bool audio_expected = false;
    std::string destination;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t block_alignment = 0;
    std::uint64_t audio_offset = 0;
};

class SessionInput
{
public:
    explicit SessionInput(std::uint64_t session);

    [[nodiscard]] bool validate_header(const protocol::Header& header) const;
    [[nodiscard]] bool accept(const protocol::Header& header, std::span<const std::byte> payload);
    [[nodiscard]] const SessionFormat& format() const;
    [[nodiscard]] std::uint64_t frames() const;
    [[nodiscard]] std::uint64_t samples() const;

private:
    enum class State
    {
        begin,
        video,
        audio_format,
        audio,
        finish,
        complete
    };

    [[nodiscard]] bool accept_begin(std::span<const std::byte> payload);
    [[nodiscard]] bool accept_frame(std::span<const std::byte> payload);
    [[nodiscard]] bool accept_end_video(std::span<const std::byte> payload);
    [[nodiscard]] bool accept_audio_format(std::span<const std::byte> payload);
    [[nodiscard]] bool accept_audio_samples(std::span<const std::byte> payload);
    [[nodiscard]] bool accept_end_audio(std::span<const std::byte> payload);

    std::uint64_t session_;
    std::uint64_t sequence_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t samples_ = 0;
    std::uint64_t next_sample_ = 0;
    State state_ = State::begin;
    SessionFormat format_;
};
} // namespace cinemania::worker
