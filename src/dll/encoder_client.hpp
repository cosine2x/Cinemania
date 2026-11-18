#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace cinemania::dll
{
struct EncoderConfig
{
    std::wstring destination;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rate = 0;
    std::uint32_t scale = 0;
    std::uint32_t stride = 0;
    std::uint32_t crf = 18;
    std::uint32_t threads = 6;
    bool audio_expected = false;
};

struct EncoderAudioFormat
{
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
};

enum class EncoderOutcome
{
    pending,
    success,
    cancelled,
    failed
};

struct EncoderResult
{
    EncoderOutcome outcome = EncoderOutcome::pending;
    std::wstring destination;
    std::string diagnostic;
    std::uint32_t system_code = 0;
};

// One producer owns start, submissions, phase changes and finish; finish is called once. cancel is
// thread-safe and idempotent, including before start. failure and has_failed may run
// concurrently. Input buffers are borrowed only during submission. Audio sessions require
// end_video, set_audio_format, and at least one contiguous PCM16 block before finish; each
// submit_audio payload is limited to 1 MiB. finish ends audio implicitly, or ends video
// implicitly for video-only sessions. A successful finish requires both the worker Result
// and its matching exit; a failed acknowledgment can leave a published destination.
// Destruction joins threads outside loader lock.
// Allocation exceptions from construction, finish or failure must be contained by the
// foreign callback boundary.
class EncoderClient
{
public:
    EncoderClient();
    ~EncoderClient();
    EncoderClient(const EncoderClient&) = delete;
    EncoderClient& operator=(const EncoderClient&) = delete;

    [[nodiscard]] bool start(const EncoderConfig& config);
    [[nodiscard]] bool submit_frame(std::span<const std::byte> pixels);
    [[nodiscard]] bool end_video();
    [[nodiscard]] bool set_audio_format(const EncoderAudioFormat& format);
    [[nodiscard]] bool submit_audio(std::uint64_t start_sample,
                                    std::uint32_t sample_count,
                                    std::span<const std::byte> pcm);
    [[nodiscard]] EncoderResult finish();
    void cancel();
    [[nodiscard]] EncoderResult failure() const;
    [[nodiscard]] bool has_failed() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cinemania::dll
