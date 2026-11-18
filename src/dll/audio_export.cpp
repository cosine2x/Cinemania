#include "audio_export.hpp"

#include "bootstrap_diagnostics.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <span>

namespace cinemania::game_export::detail
{
bool create_audio_stream(const std::shared_ptr<Session>& session,
                         PAVISTREAM* stream,
                         AVISTREAMINFOW* info,
                         HRESULT& result)
{
    FailureOnException failure_guard(session);
    result = AVIERR_BADPARAM;
    if (!owned(session) || !stream || !info || !session->audio_expected ||
        session->state != State::recording || session->audio_created || info->dwStart != 0 ||
        info->dwScale == 0 || info->dwRate == 0 || info->dwSampleSize != info->dwScale ||
        info->wPriority != 1 || info->dwQuality != 0xffffffff)
    {
        fail(session);
        return true;
    }

    if (!session->video_ended)
    {
        if (!valid_producer(session) || !session->encoder->end_video())
        {
            fail(session);
            return true;
        }
        session->video_ended = true;
    }

    void* token = make_token(session, Kind::audio);
    if (!token)
    {
        fail(session);
        result = AVIERR_MEMORY;
        return true;
    }

    session->audio_rate = info->dwRate;
    session->audio_scale = info->dwScale;
    session->audio_created = true;
    ++session->streams;
    *stream = reinterpret_cast<PAVISTREAM>(token);
    result = AVIERR_OK;
    return true;
}

bool set_audio_format(const std::shared_ptr<Session>& session,
                      LONG position,
                      LPVOID format,
                      LONG bytes,
                      HRESULT& result)
{
    FailureOnException failure_guard(session);
    result = AVIERR_BADFORMAT;
    if (!valid_producer(session) || session->state != State::recording || !session->video_ended ||
        !session->audio_created || session->audio_formatted || position != 0 || !format ||
        bytes != sizeof(WAVEFORMATEX))
    {
        fail(session);
        return true;
    }

    const auto& wave = *static_cast<const WAVEFORMATEX*>(format);
    const std::uint32_t align = static_cast<std::uint32_t>(wave.nChannels) * 2;
    const std::uint64_t average = static_cast<std::uint64_t>(wave.nSamplesPerSec) * align;
    if (wave.wFormatTag != WAVE_FORMAT_PCM || (wave.nChannels != 1 && wave.nChannels != 2) ||
        wave.wBitsPerSample != 16 || wave.cbSize != 0 || wave.nSamplesPerSec == 0 ||
        wave.nBlockAlign != align || wave.nAvgBytesPerSec != average ||
        session->audio_scale != align || session->audio_rate != average)
    {
        fail(session);
        return true;
    }

    dll::EncoderAudioFormat audio_format{wave.nSamplesPerSec, wave.nChannels};
    if (!session->encoder->set_audio_format(audio_format))
    {
        fail(session);
        return true;
    }

    session->audio_formatted = true;
    char message[160]{};
    std::snprintf(message,
                  sizeof(message),
                  "audio: format rate=%lu channels=%u align=%u\n",
                  static_cast<unsigned long>(wave.nSamplesPerSec),
                  wave.nChannels,
                  wave.nBlockAlign);
    append_diagnostic(message);

    result = AVIERR_OK;
    return true;
}

bool write_audio(const std::shared_ptr<Session>& session,
                 LONG start,
                 LONG samples,
                 LPVOID buffer,
                 LONG bytes,
                 DWORD flags,
                 LONG* samples_written,
                 LONG* bytes_written,
                 HRESULT& result)
{
    FailureOnException failure_guard(session);
    if (samples_written)
    {
        *samples_written = 0;
    }
    if (bytes_written)
    {
        *bytes_written = 0;
    }

    result = AVIERR_ERROR;
    if (!valid_producer(session) || session->state != State::recording ||
        !session->audio_formatted || !buffer || start < 0 || samples <= 0 || bytes < 0 ||
        flags != 0 || (session->audio_samples != 0 && start != session->next_audio) ||
        static_cast<std::uint64_t>(samples) * session->audio_scale !=
            static_cast<std::uint64_t>(bytes) ||
        samples > LONG_MAX - start)
    {
        fail(session);
        return true;
    }

    // The native block can exceed the encoder's bounded PCM message capacity.
    constexpr std::uint32_t max_audio_bytes = 1024U * 1024U;
    const std::uint32_t max_samples = max_audio_bytes / session->audio_scale;
    const auto* pcm = static_cast<const std::byte*>(buffer);
    std::uint32_t submitted = 0;
    const auto total_samples = static_cast<std::uint32_t>(samples);
    while (submitted < total_samples)
    {
        const std::uint32_t count = (std::min)(max_samples, total_samples - submitted);
        const std::size_t offset = static_cast<std::size_t>(submitted) * session->audio_scale;
        const std::size_t size = static_cast<std::size_t>(count) * session->audio_scale;
        if (!session->encoder->submit_audio(static_cast<std::uint64_t>(start) + submitted,
                                            count,
                                            std::span(pcm + offset, size)))
        {
            fail(session);
            return true;
        }
        submitted += count;
    }

    session->next_audio = start + samples;
    session->audio_samples += static_cast<std::uint32_t>(samples);
    if (samples_written)
    {
        *samples_written = samples;
    }
    if (bytes_written)
    {
        *bytes_written = bytes;
    }

    result = AVIERR_OK;
    return true;
}

void log_audio_outcome(const Session& session) noexcept
{
    if (!session.audio_expected)
    {
        return;
    }

    char message[192]{};
    std::snprintf(message,
                  sizeof(message),
                  "audio: final video_frames=%ld audio_samples=%llu state=%d\n",
                  session.next_frame,
                  static_cast<unsigned long long>(session.audio_samples),
                  static_cast<int>(session.state));
    append_diagnostic(message);
}
} // namespace cinemania::game_export::detail
