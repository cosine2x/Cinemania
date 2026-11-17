#include "game_export.hpp"

#include "video_export_internal.hpp"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace cinemania::game_export
{
using detail::FailureOnException;
using detail::Kind;
using detail::Session;
using detail::State;
using detail::describe_failure;
using detail::fail;
using detail::lookup;
using detail::make_token;
using detail::owned;
using detail::release_token;
using detail::valid_producer;

bool create_stream(PAVIFILE file, PAVISTREAM* stream, AVISTREAMINFOW* info, HRESULT& result)
{
    bool synthetic = false;
    auto session = lookup(file, Kind::file, synthetic);
    if (!synthetic)
    {
        return false;
    }

    FailureOnException failure_guard(session);
    result = AVIERR_BADHANDLE;
    if (!owned(session))
    {
        return true;
    }

    if (!stream || !info || info->fccType != streamtypeVIDEO || !info->dwRate || !info->dwScale ||
        session->state != State::preparing || session->streams != 0 || info->rcFrame.left != 0 ||
        info->rcFrame.top != 0 || info->rcFrame.right < 2 || info->rcFrame.bottom < 2 ||
        info->rcFrame.right > 8192 || info->rcFrame.bottom > 8192)
    {
        fail(session);
        result = AVIERR_BADPARAM;
        return true;
    }

    void* token = make_token(session, Kind::raw);
    if (!token)
    {
        fail(session);
        result = AVIERR_MEMORY;
        return true;
    }

    session->width = info->rcFrame.right;
    session->height = info->rcFrame.bottom;
    session->rate = info->dwRate;
    session->scale = info->dwScale;
    session->streams = 1;
    *stream = reinterpret_cast<PAVISTREAM>(token);
    result = AVIERR_OK;
    return true;
}

bool save_options(int count, PAVISTREAM* streams, LPAVICOMPRESSOPTIONS* options, INT_PTR& result)
{
    if (!streams || count < 1)
    {
        return false;
    }

    bool synthetic = false;
    std::shared_ptr<Session> session;
    for (int index = 0; index < count; ++index)
    {
        bool current_synthetic = false;
        auto current = lookup(streams[index], Kind::raw, current_synthetic);
        if (current_synthetic)
        {
            synthetic = true;
            session = std::move(current);
            break;
        }
    }
    if (!synthetic)
    {
        return false;
    }

    FailureOnException failure_guard(session);
    result = FALSE;
    if (!owned(session) || count != 1 || !options || !options[0])
    {
        fail(session);
        return true;
    }

    *options[0] = {};
    options[0]->fccType = streamtypeVIDEO;
    options[0]->dwFlags = AVICOMPRESSF_VALID;
    result = TRUE;
    return true;
}

bool make_compressed(PAVISTREAM* output,
                     PAVISTREAM source,
                     AVICOMPRESSOPTIONS* options,
                     CLSID* handler,
                     HRESULT& result)
{
    bool synthetic = false;
    auto session = lookup(source, Kind::raw, synthetic);
    if (!synthetic)
    {
        return false;
    }

    FailureOnException failure_guard(session);
    result = AVIERR_BADHANDLE;
    if (!owned(session) || !output || !options || handler || session->streams != 1 ||
        options->fccType != streamtypeVIDEO || options->dwFlags != AVICOMPRESSF_VALID ||
        options->lpFormat || options->cbFormat || options->lpParms || options->cbParms)
    {
        fail(session);
        return true;
    }

    void* token = make_token(session, Kind::compressed);
    if (!token)
    {
        fail(session);
        result = AVIERR_MEMORY;
        return true;
    }

    *output = reinterpret_cast<PAVISTREAM>(token);
    ++session->streams;
    result = AVIERR_OK;
    return true;
}

bool set_format(PAVISTREAM stream, LONG position, LPVOID format, LONG bytes, HRESULT& result)
{
    bool synthetic = false;
    auto session = lookup(stream, Kind::compressed, synthetic);
    if (!synthetic)
    {
        return false;
    }

    FailureOnException failure_guard(session);
    result = AVIERR_BADFORMAT;
    if (!owned(session) || !format || bytes < static_cast<LONG>(sizeof(BITMAPINFOHEADER)) ||
        position != 0 || session->state != State::preparing)
    {
        fail(session);
        return true;
    }

    const auto& bitmap = *static_cast<const BITMAPINFOHEADER*>(format);
    if (bitmap.biSize != sizeof(BITMAPINFOHEADER) || bitmap.biWidth != session->width ||
        bitmap.biHeight != session->height || bitmap.biWidth < 2 || bitmap.biWidth > 8192 ||
        bitmap.biHeight < 2 || bitmap.biHeight > 8192 || bitmap.biWidth % 8 != 0 ||
        bitmap.biHeight % 8 != 0 || bitmap.biPlanes != 1 || bitmap.biBitCount != 24 ||
        bitmap.biCompression != BI_RGB || session->rate > 1000000 || session->scale > 1000000)
    {
        fail(session);
        return true;
    }

    const auto stride = static_cast<std::uint32_t>(bitmap.biWidth) * 3;
    const auto size =
        static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(bitmap.biHeight);
    if (size > 64U * 1024U * 1024U - 8 || bitmap.biSizeImage != size)
    {
        fail(session);
        return true;
    }

    dll::EncoderConfig config;
    config.destination = session->path;
    config.width = static_cast<std::uint32_t>(bitmap.biWidth);
    config.height = static_cast<std::uint32_t>(bitmap.biHeight);
    config.rate = session->rate;
    config.scale = session->scale;
    config.stride = stride;
    session->encoder = std::make_unique<dll::EncoderClient>();
    session->producer = GetCurrentThreadId();
    if (!session->encoder->start(config))
    {
        fail(session);
        return true;
    }

    session->frame_bytes = static_cast<std::size_t>(size);
    session->state = State::recording;
    result = AVIERR_OK;
    return true;
}

bool write(PAVISTREAM stream,
           LONG start,
           LONG samples,
           LPVOID buffer,
           LONG bytes,
           LONG* samples_written,
           LONG* bytes_written,
           HRESULT& result)
{
    bool synthetic = false;
    auto session = lookup(stream, Kind::compressed, synthetic);
    if (!synthetic)
    {
        return false;
    }

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
    if (!valid_producer(session) || session->state != State::recording || !buffer || samples != 1 ||
        bytes < 0 || static_cast<std::size_t>(bytes) != session->frame_bytes ||
        start != session->next_frame || session->next_frame == LONG_MAX)
    {
        fail(session);
        return true;
    }

    auto pixels = std::span(static_cast<const std::byte*>(buffer), session->frame_bytes);
    if (!session->encoder->submit_frame(pixels))
    {
        fail(session);
        return true;
    }

    ++session->next_frame;
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

bool stream_release(PAVISTREAM stream, ULONG& result)
{
    std::shared_ptr<Session> session;
    if (release_token(stream, Kind::compressed, session))
    {
        if (!session)
        {
            // A raw token also belongs to the synthetic namespace.
            release_token(stream, Kind::raw, session);
        }

        if (owned(session) && session->streams > 0)
        {
            --session->streams;
        }
        result = 0;
        return true;
    }

    return false;
}

bool file_release(PAVIFILE file, ULONG& result)
{
    std::shared_ptr<Session> session;
    if (!release_token(file, Kind::file, session))
    {
        return false;
    }

    FailureOnException failure_guard(session);
    result = 0;
    if (!owned(session))
    {
        return true;
    }

    if (session->foreign_failure.load())
    {
        fail(session);
    }

    if (session->state == State::recording && session->streams == 0 && valid_producer(session))
    {
        const auto outcome = session->encoder->finish();
        if (outcome.outcome == dll::EncoderOutcome::success)
        {
            session->state = State::success;
        }
        else if (outcome.outcome == dll::EncoderOutcome::cancelled)
        {
            session->state = State::cancelled;
        }
        else
        {
            session->state = State::failed;
        }
        if (session->state == State::failed)
        {
            describe_failure(*session, outcome);
        }
    }
    else if (session->state != State::cancelled)
    {
        fail(session);
    }

    session->encoder.reset();
    return true;
}

} // namespace cinemania::game_export
