#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cinemania::protocol
{
// Wire integers are little-endian. The 32-byte header carries magic, version, type,
// nonzero session, per-channel sequence, payload length, and a zero reserved field.
constexpr std::uint32_t kMagic = 0x454e4943;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 32;
constexpr std::uint32_t kMaxPayloadBytes = 64U * 1024U * 1024U;

// Data payloads: Begin is nine u32 fields (width, height, FPS numerator and
// denominator, stride, CRF, threads, audio flag, path byte count) and UTF-8 path;
// VideoFrame is u64 index plus BGR24 pixels; EndVideo is u64 count; AudioFormat
// is four u32 fields (rate, channels, bits, alignment); AudioSamples is u64 start,
// u32 count, and PCM16LE; EndAudio is u64 count; Finish and Cancel are empty.
// Ready is empty; Progress is two u64 accepted counts. Sequences start at zero
// independently on each channel. SessionInput validates the message-specific limits and order.
enum class MessageType : std::uint16_t
{
    begin_session = 1,
    video_frame = 2,
    end_video = 3,
    audio_format = 4,
    audio_samples = 5,
    end_audio = 6,
    finish = 7,
    cancel = 8,
    ready = 0x100,
    progress = 0x101,
    result = 0x102
};

// Result payload begins with outcome, stage, and native/encoder code, followed by
// accepted video/audio counts and a length-prefixed bounded UTF-8 diagnostic.
enum class Outcome : std::uint32_t
{
    success = 0,
    cancelled = 1,
    failure = 2
};

enum class Stage : std::uint32_t
{
    protocol = 1,
    preflight = 2,
    video = 3,
    audio = 4,
    mux = 5,
    publish = 6,
    control = 7
};

struct Header
{
    MessageType type;
    std::uint64_t session;
    std::uint64_t sequence;
    std::uint32_t payload_bytes;
};

[[nodiscard]] bool decode_header(std::span<const std::byte> bytes, Header& header);
[[nodiscard]] bool
read_u32(std::span<const std::byte> bytes, std::size_t& offset, std::uint32_t& value);
[[nodiscard]] bool
read_u64(std::span<const std::byte> bytes, std::size_t& offset, std::uint64_t& value);
void append_u16(std::vector<std::byte>& bytes, std::uint16_t value);
void append_u32(std::vector<std::byte>& bytes, std::uint32_t value);
void append_u64(std::vector<std::byte>& bytes, std::uint64_t value);
[[nodiscard]] std::vector<std::byte> encode_header(const Header& header);
} // namespace cinemania::protocol
