#include "session_input.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace cinemania::worker
{
namespace
{
constexpr std::uint32_t kMaxAudioBytes = 1024U * 1024U;
constexpr std::uint32_t kMaxPathBytes = 32767;
constexpr std::array<std::uint32_t, 12> kSupportedSampleRates = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};

bool supported_sample_rate(std::uint32_t rate)
{
    return std::find(kSupportedSampleRates.begin(), kSupportedSampleRates.end(), rate) !=
           kSupportedSampleRates.end();
}
} // namespace

SessionInput::SessionInput(std::uint64_t session) : session_(session)
{
}

bool SessionInput::validate_header(const protocol::Header& header) const
{
    if (header.session != session_ || header.sequence != sequence_ ||
        header.payload_bytes > protocol::kMaxPayloadBytes)
    {
        return false;
    }

    switch (state_)
    {
        case State::begin:
            return header.type == protocol::MessageType::begin_session &&
                   header.payload_bytes >= 37 && header.payload_bytes <= 36 + kMaxPathBytes;
        case State::video:
            return (header.type == protocol::MessageType::video_frame &&
                    header.payload_bytes ==
                        static_cast<std::uint64_t>(format_.stride) * format_.height + 8) ||
                   (header.type == protocol::MessageType::end_video && header.payload_bytes == 8);
        case State::audio_format:
            return header.type == protocol::MessageType::audio_format && header.payload_bytes == 16;
        case State::audio:
            return (header.type == protocol::MessageType::audio_samples &&
                    header.payload_bytes > 12 && header.payload_bytes <= 12 + kMaxAudioBytes) ||
                   (header.type == protocol::MessageType::end_audio && header.payload_bytes == 8);
        case State::finish:
            return header.type == protocol::MessageType::finish && header.payload_bytes == 0;
        case State::complete:
            return false;
    }
    return false;
}

bool SessionInput::accept(const protocol::Header& header, std::span<const std::byte> payload)
{
    if (!validate_header(header) || payload.size() != header.payload_bytes ||
        sequence_ == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }

    bool accepted = false;
    switch (header.type)
    {
        case protocol::MessageType::begin_session:
            accepted = accept_begin(payload);
            break;
        case protocol::MessageType::video_frame:
            accepted = accept_frame(payload);
            break;
        case protocol::MessageType::end_video:
            accepted = accept_end_video(payload);
            break;
        case protocol::MessageType::audio_format:
            accepted = accept_audio_format(payload);
            break;
        case protocol::MessageType::audio_samples:
            accepted = accept_audio_samples(payload);
            break;
        case protocol::MessageType::end_audio:
            accepted = accept_end_audio(payload);
            break;
        case protocol::MessageType::finish:
            state_ = State::complete;
            accepted = true;
            break;
        default:
            break;
    }
    if (accepted)
    {
        ++sequence_;
    }
    return accepted;
}

bool SessionInput::accept_begin(std::span<const std::byte> payload)
{
    SessionFormat candidate;
    std::uint32_t audio = 0;
    std::uint32_t path_bytes = 0;
    std::size_t offset = 0;
    if (!protocol::read_u32(payload, offset, candidate.width) ||
        !protocol::read_u32(payload, offset, candidate.height) ||
        !protocol::read_u32(payload, offset, candidate.fps_numerator) ||
        !protocol::read_u32(payload, offset, candidate.fps_denominator) ||
        !protocol::read_u32(payload, offset, candidate.stride) ||
        !protocol::read_u32(payload, offset, candidate.crf) ||
        !protocol::read_u32(payload, offset, candidate.threads) ||
        !protocol::read_u32(payload, offset, audio) ||
        !protocol::read_u32(payload, offset, path_bytes))
    {
        return false;
    }
    if (candidate.width < 2 || candidate.width > 8192 || candidate.width % 2 != 0 ||
        candidate.height < 2 || candidate.height > 8192 || candidate.height % 2 != 0 ||
        candidate.stride != candidate.width * 3 ||
        static_cast<std::uint64_t>(candidate.stride) * candidate.height >
            protocol::kMaxPayloadBytes - 8 ||
        candidate.fps_numerator == 0 || candidate.fps_numerator > 1000000 ||
        candidate.fps_denominator == 0 || candidate.fps_denominator > 1000000 ||
        candidate.crf > 51 || candidate.threads == 0 || candidate.threads > 16 || audio > 1 ||
        path_bytes == 0 || path_bytes > kMaxPathBytes || path_bytes != payload.size() - offset)
    {
        return false;
    }
    candidate.audio_expected = audio != 0;
    candidate.destination.reserve(path_bytes);
    for (std::size_t index = offset; index < payload.size(); ++index)
    {
        const auto byte = std::to_integer<unsigned char>(payload[index]);
        if (byte == 0)
        {
            return false;
        }
        candidate.destination.push_back(static_cast<char>(byte));
    }
    format_ = std::move(candidate);
    state_ = State::video;
    return true;
}

bool SessionInput::accept_frame(std::span<const std::byte> payload)
{
    std::uint64_t index = 0;
    std::size_t offset = 0;
    if (!protocol::read_u64(payload, offset, index) || index != frames_ ||
        frames_ == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }
    ++frames_;
    return true;
}

bool SessionInput::accept_end_video(std::span<const std::byte> payload)
{
    std::uint64_t count = 0;
    std::size_t offset = 0;
    if (!protocol::read_u64(payload, offset, count) || count == 0 || count != frames_)
    {
        return false;
    }
    state_ = format_.audio_expected ? State::audio_format : State::finish;
    return true;
}

bool SessionInput::accept_audio_format(std::span<const std::byte> payload)
{
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;
    std::uint32_t alignment = 0;
    std::size_t offset = 0;
    if (!protocol::read_u32(payload, offset, rate) ||
        !protocol::read_u32(payload, offset, channels) ||
        !protocol::read_u32(payload, offset, bits) ||
        !protocol::read_u32(payload, offset, alignment) || !supported_sample_rate(rate) ||
        channels < 1 || channels > 2 || bits != 16 || alignment != channels * 2)
    {
        return false;
    }
    format_.sample_rate = rate;
    format_.channels = channels;
    format_.block_alignment = alignment;
    state_ = State::audio;
    return true;
}

bool SessionInput::accept_audio_samples(std::span<const std::byte> payload)
{
    std::uint64_t index = 0;
    std::uint32_t count = 0;
    std::size_t offset = 0;
    if (!protocol::read_u64(payload, offset, index) ||
        !protocol::read_u32(payload, offset, count) || count == 0 ||
        static_cast<std::uint64_t>(count) * format_.block_alignment != payload.size() - offset ||
        index > (std::uint64_t{1} << 40) - count ||
        samples_ > std::numeric_limits<std::uint64_t>::max() - count ||
        (samples_ != 0 && index != next_sample_))
    {
        return false;
    }
    if (samples_ == 0)
    {
        format_.audio_offset = index;
    }
    samples_ += count;
    next_sample_ = index + count;
    return true;
}

bool SessionInput::accept_end_audio(std::span<const std::byte> payload)
{
    std::uint64_t count = 0;
    std::size_t offset = 0;
    if (!protocol::read_u64(payload, offset, count) || count == 0 || count != samples_)
    {
        return false;
    }
    state_ = State::finish;
    return true;
}

const SessionFormat& SessionInput::format() const
{
    return format_;
}

std::uint64_t SessionInput::frames() const
{
    return frames_;
}

std::uint64_t SessionInput::samples() const
{
    return samples_;
}
} // namespace cinemania::worker
