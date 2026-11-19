#include "encoder_client.hpp"

#include "../../common/protocol.hpp"
#include "encoder_scratch.hpp"
#include "encoder_transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <objbase.h>

namespace cinemania::dll
{
namespace
{
constexpr std::size_t kQueueBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxAudioBytes = 1024U * 1024U;
constexpr DWORD kDeadlineMs = 30000;

std::array<std::byte, protocol::kHeaderBytes> header(protocol::MessageType type,
                                                     std::uint64_t session,
                                                     std::uint64_t sequence,
                                                     std::uint32_t size)
{
    std::array<std::byte, protocol::kHeaderBytes> result{};
    const auto put = [&result](std::size_t offset, std::uint64_t value, std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            result[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xffU);
        }
    };

    put(0, protocol::kMagic, 4);
    put(4, protocol::kVersion, 2);
    put(6, static_cast<std::uint16_t>(type), 2);
    put(8, session, 8);
    put(16, sequence, 8);
    put(24, size, 4);
    return result;
}

DWORD utf8(const std::wstring& input, std::string& output)
{
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX))
    {
        return ERROR_INVALID_NAME;
    }

    const int count = WideCharToMultiByte(CP_UTF8,
                                          WC_ERR_INVALID_CHARS,
                                          input.data(),
                                          static_cast<int>(input.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (!count)
    {
        return GetLastError();
    }

    output.resize(static_cast<std::size_t>(count));
    if (!WideCharToMultiByte(CP_UTF8,
                             WC_ERR_INVALID_CHARS,
                             input.data(),
                             static_cast<int>(input.size()),
                             output.data(),
                             count,
                             nullptr,
                             nullptr))
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

} // namespace

class EncoderClient::Impl
{
public:
    ~Impl()
    {
        try
        {
            cancel();
            join();
            cleanup();
        }
        catch (...)
        {
            transport_.stop();
            if (sender_.joinable())
            {
                sender_.join();
            }
            if (status_.joinable())
            {
                status_.join();
            }
        }
    }

    bool start(const EncoderConfig& config);
    bool submit(std::span<const std::byte> pixels);
    bool end_video();
    bool set_audio_format(const EncoderAudioFormat& format);
    bool
    submit_audio(std::uint64_t start_sample, std::uint32_t count, std::span<const std::byte> pcm);
    EncoderResult finish();
    void cancel();
    EncoderResult snapshot() const;

    bool failed() const noexcept
    {
        return failed_.load();
    }

    void exception_failure()
    {
        fail(ERROR_NOT_ENOUGH_MEMORY, "encoder client exception");
    }

private:
    void fail(DWORD code, const char* text) noexcept;
    void send_loop();
    void status_loop();
    DWORD
    read_status(protocol::Header& message, std::span<std::byte> storage, std::uint64_t expected);
    bool accept_ready();
    bool accept_progress(std::span<const std::byte> payload,
                         std::uint64_t& previous_video,
                         std::uint64_t& previous_audio);
    bool enqueue(protocol::MessageType type,
                 std::span<const std::byte> prefix,
                 std::span<const std::byte> payload = {});
    void accept_result(std::span<const std::byte> payload,
                       std::uint64_t previous_video,
                       std::uint64_t previous_audio);
    DWORD send(protocol::MessageType type, std::span<const std::byte> payload);
    void join();
    void cleanup();
    bool cancellation_requested() const;

    EncoderTransport transport_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;

    // Queue and lifecycle state shared by the producer and worker threads.
    struct Slot
    {
        std::vector<std::byte> bytes;
        std::size_t size = 0;
        protocol::MessageType type = protocol::MessageType::video_frame;
    };
    enum class Phase
    {
        video,
        audio_format,
        audio,
        finishing
    };
    std::vector<Slot> slots_;
    std::size_t head_ = 0;
    std::size_t queued_ = 0;
    std::size_t frame_bytes_ = 0;
    std::uint64_t session_ = 0;
    std::uint64_t sequence_ = 0;
    // Counts and progress are shared with the status thread under mutex_.
    std::uint64_t frames_ = 0;
    std::uint64_t samples_ = 0;
    std::uint64_t progress_generation_ = 0;

    // The producer alone changes the phase, format and next sample position.
    std::uint64_t next_sample_ = 0;
    std::uint32_t audio_alignment_ = 0;
    bool audio_expected_ = false;
    Phase phase_ = Phase::video;

    bool ready_ = false;
    bool closing_ = false;
    bool cancel_requested_ = false;
    bool terminal_ = false;
    bool started_ = false;
    std::atomic<bool> failed_{false};
    EncoderResult result_;

    std::thread sender_;
    std::thread status_;

    EncoderScratch scratch_;
};

void EncoderClient::Impl::fail(DWORD code, const char* text) noexcept
{
    try
    {
        std::lock_guard lock(mutex_);
        if (terminal_ || cancel_requested_)
        {
            return;
        }

        terminal_ = true;
        failed_.store(true);
        result_.outcome = EncoderOutcome::failed;
        result_.system_code = code;

        try
        {
            result_.diagnostic = text;
            result_.diagnostic += "; destination may exist if acknowledgment was lost";
        }
        catch (...)
        {
            // Preserve the error state even if diagnostic allocation fails.
            result_.diagnostic.clear();
        }
    }
    catch (...)
    {
        failed_.store(true);
    }

    changed_.notify_all();
    transport_.stop();
}

bool EncoderClient::Impl::cancellation_requested() const
{
    std::lock_guard lock(mutex_);
    return cancel_requested_;
}

DWORD EncoderClient::Impl::send(protocol::MessageType type, std::span<const std::byte> payload)
{
    const auto bytes =
        header(type, session_, sequence_, static_cast<std::uint32_t>(payload.size()));
    DWORD error = transport_.write_data(bytes, kDeadlineMs);
    if (error == ERROR_SUCCESS)
    {
        error = transport_.write_data(payload, kDeadlineMs);
    }

    if (error == ERROR_SUCCESS)
    {
        ++sequence_;
    }

    return error;
}

bool EncoderClient::Impl::start(const EncoderConfig& config)
{
    {
        std::lock_guard lock(mutex_);
        if (started_ || terminal_ || cancel_requested_)
        {
            return false;
        }

        started_ = true;
        result_.destination = config.destination;
    }

    const std::uint64_t bytes = static_cast<std::uint64_t>(config.stride) * config.height;
    if (config.width < 2 || config.width > 8192 || (config.width & 1) || config.height < 2 ||
        config.height > 8192 || (config.height & 1) || config.stride != config.width * 3 ||
        !config.rate || config.rate > 1000000 || !config.scale || config.scale > 1000000 ||
        config.crf > 51 || !config.threads || config.threads > 16 || bytes > kQueueBytes - 8)
    {
        fail(ERROR_INVALID_PARAMETER, "invalid video format");
        return false;
    }

    frame_bytes_ = static_cast<std::size_t>(bytes);
    audio_expected_ = config.audio_expected;
    const std::size_t slot_bytes =
        audio_expected_ ? (std::max)(frame_bytes_ + 8, kMaxAudioBytes + 12) : frame_bytes_ + 8;
    slots_.resize((std::min)(std::size_t{8}, kQueueBytes / slot_bytes));
    for (auto& slot : slots_)
    {
        slot.bytes.resize(slot_bytes);
    }

    std::string destination;
    DWORD error = utf8(config.destination, destination);
    if (error == ERROR_SUCCESS)
    {
        error = scratch_.prepare(config.destination);
    }
    if (error != ERROR_SUCCESS)
    {
        fail(error, "cannot prepare encoder session");
        return false;
    }

    GUID identifier{};
    const HRESULT guid_error = CoCreateGuid(&identifier);
    if (FAILED(guid_error))
    {
        fail(static_cast<DWORD>(guid_error), "cannot create session identifier");
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        std::memcpy(&session_, &identifier, sizeof(session_));
        if (!session_)
        {
            session_ = 1;
        }

        if (terminal_ || cancel_requested_)
        {
            return false;
        }
    }

    error = transport_.launch(scratch_.path(), session_);
    if (error != ERROR_SUCCESS)
    {
        fail(error, "cannot launch encoder worker");
        return false;
    }

    std::vector<std::byte> begin;
    protocol::append_u32(begin, config.width);
    protocol::append_u32(begin, config.height);
    protocol::append_u32(begin, config.rate);
    protocol::append_u32(begin, config.scale);
    protocol::append_u32(begin, config.stride);
    protocol::append_u32(begin, config.crf);
    protocol::append_u32(begin, config.threads);
    protocol::append_u32(begin, config.audio_expected ? 1U : 0U);
    protocol::append_u32(begin, static_cast<std::uint32_t>(destination.size()));
    for (const char character : destination)
    {
        begin.push_back(static_cast<std::byte>(character));
    }

    status_ = std::thread(
        [this]
        {
            status_loop();
        });
    error = send(protocol::MessageType::begin_session, begin);
    if (error != ERROR_SUCCESS)
    {
        fail(error, "cannot send session declaration");
    }

    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock,
                           std::chrono::milliseconds(kDeadlineMs),
                           [this]
                           {
                               return ready_ || terminal_ || cancel_requested_;
                           }))
    {
        lock.unlock();
        fail(ERROR_TIMEOUT, "encoder preflight timed out");
        return false;
    }
    if (!ready_ || terminal_ || cancel_requested_)
    {
        return false;
    }

    lock.unlock();
    sender_ = std::thread(
        [this]
        {
            send_loop();
        });

    return true;
}

bool EncoderClient::Impl::enqueue(protocol::MessageType type,
                                  std::span<const std::byte> prefix,
                                  std::span<const std::byte> payload)
{
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock,
                           std::chrono::milliseconds(kDeadlineMs),
                           [this]
                           {
                               return queued_ < slots_.size() || terminal_ || closing_ ||
                                      cancel_requested_;
                           }))
    {
        lock.unlock();
        fail(ERROR_TIMEOUT, "encoder input queue stalled");
        return false;
    }
    if (terminal_ || closing_ || cancel_requested_ || !ready_)
    {
        return false;
    }

    auto& slot = slots_[(head_ + queued_) % slots_.size()];
    slot.type = type;
    slot.size = prefix.size() + payload.size();
    std::memcpy(slot.bytes.data(), prefix.data(), prefix.size());
    if (!payload.empty())
    {
        std::memcpy(slot.bytes.data() + prefix.size(), payload.data(), payload.size());
    }

    ++queued_;
    lock.unlock();
    changed_.notify_all();

    return true;
}

bool EncoderClient::Impl::submit(std::span<const std::byte> pixels)
{
    if (phase_ != Phase::video || pixels.size() != frame_bytes_ ||
        frames_ == (std::numeric_limits<std::uint64_t>::max)())
    {
        fail(ERROR_INVALID_PARAMETER, "invalid video frame or input phase");
        return false;
    }

    std::array<std::byte, 8> index{};
    for (std::size_t i = 0; i < index.size(); ++i)
    {
        index[i] = static_cast<std::byte>((frames_ >> (i * 8)) & 0xffU);
    }

    // Publish the count before enqueueing so status validation sees every queued item.
    {
        std::lock_guard lock(mutex_);
        ++frames_;
    }

    return enqueue(protocol::MessageType::video_frame, index, pixels);
}

bool EncoderClient::Impl::end_video()
{
    if (phase_ != Phase::video || frames_ == 0)
    {
        fail(ERROR_INVALID_STATE, "video input cannot end in this phase");
        return false;
    }

    std::vector<std::byte> count;
    protocol::append_u64(count, frames_);
    if (!enqueue(protocol::MessageType::end_video, count))
    {
        return false;
    }

    phase_ = audio_expected_ ? Phase::audio_format : Phase::finishing;
    return true;
}

bool EncoderClient::Impl::set_audio_format(const EncoderAudioFormat& format)
{
    constexpr std::array<std::uint32_t, 12> rates = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
    if (phase_ != Phase::audio_format || format.channels < 1 || format.channels > 2 ||
        std::find(rates.begin(), rates.end(), format.sample_rate) == rates.end())
    {
        fail(ERROR_INVALID_PARAMETER, "invalid audio format or input phase");
        return false;
    }

    audio_alignment_ = format.channels * 2;
    std::vector<std::byte> bytes;
    protocol::append_u32(bytes, format.sample_rate);
    protocol::append_u32(bytes, format.channels);
    protocol::append_u32(bytes, 16);
    protocol::append_u32(bytes, audio_alignment_);

    if (!enqueue(protocol::MessageType::audio_format, bytes))
    {
        return false;
    }

    phase_ = Phase::audio;
    return true;
}

bool EncoderClient::Impl::submit_audio(std::uint64_t start_sample,
                                       std::uint32_t count,
                                       std::span<const std::byte> pcm)
{
    if (phase_ != Phase::audio || count == 0 ||
        static_cast<std::uint64_t>(count) * audio_alignment_ != pcm.size() ||
        pcm.size() > kMaxAudioBytes || start_sample > (std::uint64_t{1} << 40) - count ||
        samples_ > (std::numeric_limits<std::uint64_t>::max)() - count ||
        (samples_ != 0 && start_sample != next_sample_))
    {
        fail(ERROR_INVALID_PARAMETER, "invalid audio samples or input phase");
        return false;
    }

    std::array<std::byte, 12> prefix{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        prefix[i] = static_cast<std::byte>((start_sample >> (i * 8)) & 0xffU);
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        prefix[8 + i] = static_cast<std::byte>((count >> (i * 8)) & 0xffU);
    }

    {
        std::lock_guard lock(mutex_);
        samples_ += count;
        next_sample_ = start_sample + count;
    }

    return enqueue(protocol::MessageType::audio_samples, prefix, pcm);
}

void EncoderClient::Impl::send_loop()
{
    try
    {
        for (;;)
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,
                          [this]
                          {
                              return queued_ || closing_ || terminal_ || cancel_requested_;
                          });
            if (terminal_ || cancel_requested_)
            {
                return;
            }
            if (!queued_ && closing_)
            {
                break;
            }

            auto& slot = slots_[head_];
            lock.unlock();

            const DWORD error =
                send(slot.type, std::span<const std::byte>(slot.bytes.data(), slot.size));
            if (error != ERROR_SUCCESS)
            {
                if (cancellation_requested())
                {
                    return;
                }
                fail(error, "cannot send encoder input");
                return;
            }

            lock.lock();
            head_ = (head_ + 1) % slots_.size();
            --queued_;

            lock.unlock();
            changed_.notify_all();
        }

        const DWORD error = send(protocol::MessageType::finish, {});
        if (error != ERROR_SUCCESS && !cancellation_requested())
        {
            fail(error, "cannot finish video input");
        }
    }
    catch (...)
    {
        fail(ERROR_NOT_ENOUGH_MEMORY, "encoder sender failed");
    }
}

DWORD EncoderClient::Impl::read_status(protocol::Header& message,
                                       std::span<std::byte> storage,
                                       std::uint64_t expected)
{
    std::array<std::byte, protocol::kHeaderBytes> bytes{};
    DWORD error = transport_.read_status(bytes, kDeadlineMs);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }

    if (!protocol::decode_header(bytes, message) || message.session != session_ ||
        message.sequence != expected || message.payload_bytes > storage.size())
    {
        return ERROR_INVALID_DATA;
    }

    return transport_.read_status(storage.first(message.payload_bytes), kDeadlineMs);
}

bool EncoderClient::Impl::accept_ready()
{
    {
        std::lock_guard lock(mutex_);
        if (ready_ || terminal_)
        {
            return false;
        }

        ready_ = true;
    }

    changed_.notify_all();

    return true;
}

bool EncoderClient::Impl::accept_progress(std::span<const std::byte> payload,
                                          std::uint64_t& previous_video,
                                          std::uint64_t& previous_audio)
{
    std::size_t offset = 0;
    std::uint64_t video = 0;
    std::uint64_t audio = 0;
    if (payload.size() != 16 || !protocol::read_u64(payload, offset, video) ||
        !protocol::read_u64(payload, offset, audio) || video < previous_video ||
        audio < previous_audio)
    {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (!ready_ || video > frames_ || audio > samples_)
        {
            return false;
        }
        ++progress_generation_;
    }
    changed_.notify_all();

    previous_video = video;
    previous_audio = audio;
    return true;
}

void EncoderClient::Impl::accept_result(std::span<const std::byte> payload,
                                        std::uint64_t previous_video,
                                        std::uint64_t previous_audio)
{
    std::size_t offset = 0;
    std::uint32_t outcome = 0;
    std::uint32_t stage = 0;
    std::uint32_t code = 0;
    std::uint32_t length = 0;
    std::uint64_t video = 0;
    std::uint64_t audio = 0;
    if (!protocol::read_u32(payload, offset, outcome) ||
        !protocol::read_u32(payload, offset, stage) || !protocol::read_u32(payload, offset, code) ||
        !protocol::read_u64(payload, offset, video) ||
        !protocol::read_u64(payload, offset, audio) ||
        !protocol::read_u32(payload, offset, length) || length > 4096 ||
        offset + length != payload.size() || outcome > 2 || stage < 1 || stage > 7)
    {
        fail(ERROR_INVALID_DATA, "invalid worker result");
        return;
    }

    DWORD exit_code = 0;
    const DWORD error = transport_.wait(kDeadlineMs, exit_code);
    const DWORD expected_exit = outcome == 1 ? 2U : (outcome == 2 ? 1U : 0U);
    if (error != ERROR_SUCCESS || exit_code != expected_exit)
    {
        fail(error ? error : ERROR_PROCESS_ABORTED, "worker result was not acknowledged");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        if (terminal_)
        {
            return;
        }

        if (video < previous_video || audio < previous_audio || video > frames_ || audio > samples_)
        {
            result_.outcome = EncoderOutcome::failed;
            result_.system_code = ERROR_INVALID_DATA;
            result_.diagnostic = "worker reported invalid accepted counts; destination may exist";
            terminal_ = true;
            failed_.store(true);
            changed_.notify_all();
            return;
        }

        result_.outcome = outcome == 0
                              ? EncoderOutcome::success
                              : (outcome == 1 ? EncoderOutcome::cancelled : EncoderOutcome::failed);
        result_.system_code = code;
        result_.diagnostic.assign(reinterpret_cast<const char*>(payload.data() + offset), length);

        if (outcome == 0 &&
            (!ready_ || video == 0 || video != frames_ || audio != samples_ || !closing_))
        {
            result_.outcome = EncoderOutcome::failed;
            result_.diagnostic = "worker reported inconsistent output; destination may exist";
        }

        terminal_ = true;
        failed_.store(result_.outcome == EncoderOutcome::failed);
    }

    changed_.notify_all();
}

void EncoderClient::Impl::status_loop()
{
    try
    {
        std::uint64_t expected = 0;
        std::uint64_t progress_frames = 0;
        std::uint64_t progress_samples = 0;
        std::array<std::byte, 4128> storage{};

        for (;;)
        {
            protocol::Header message{};
            const DWORD error = read_status(message, storage, expected++);
            if (error != ERROR_SUCCESS)
            {
                if (!cancellation_requested())
                {
                    fail(error, "worker status was lost or invalid");
                }
                return;
            }

            const auto payload = std::span<const std::byte>(storage.data(), message.payload_bytes);
            if (message.type == protocol::MessageType::result)
            {
                accept_result(payload, progress_frames, progress_samples);
                return;
            }

            const bool valid_ready =
                message.type == protocol::MessageType::ready && payload.empty() && accept_ready();
            const bool valid_progress = message.type == protocol::MessageType::progress &&
                                        accept_progress(payload, progress_frames, progress_samples);
            if (!valid_ready && !valid_progress)
            {
                fail(ERROR_INVALID_DATA, "unexpected or invalid worker status");
                return;
            }
        }
    }
    catch (...)
    {
        fail(ERROR_NOT_ENOUGH_MEMORY, "encoder status reader failed");
    }
}

void EncoderClient::Impl::join()
{
    if (sender_.joinable())
    {
        sender_.join();
    }
    if (status_.joinable())
    {
        status_.join();
    }
}

void EncoderClient::Impl::cleanup()
{
    DWORD error = ERROR_BUSY;
    if (transport_.job_empty())
    {
        error = scratch_.cleanup();
    }

    if (error != ERROR_SUCCESS)
    {
        std::lock_guard lock(mutex_);
        result_.diagnostic += "; scratch cleanup failed (" + std::to_string(error) + ")";
        if (result_.outcome == EncoderOutcome::success)
        {
            result_.outcome = EncoderOutcome::failed;
            result_.system_code = error;
            failed_.store(true);
        }
    }
}

EncoderResult EncoderClient::Impl::finish()
{
    if (phase_ == Phase::video && !audio_expected_)
    {
        static_cast<void>(end_video());
    }
    if (phase_ == Phase::audio && samples_ != 0)
    {
        std::vector<std::byte> count;
        protocol::append_u64(count, samples_);
        if (enqueue(protocol::MessageType::end_audio, count))
        {
            phase_ = Phase::finishing;
        }
    }
    if (phase_ != Phase::finishing)
    {
        fail(ERROR_INVALID_STATE, "required encoder input is incomplete");
    }
    {
        std::unique_lock lock(mutex_);
        if (!started_)
        {
            lock.unlock();
            fail(ERROR_INVALID_STATE, "encoder session was not started");
            return snapshot();
        }

        closing_ = true;
    }

    changed_.notify_all();
    std::unique_lock lock(mutex_);
    bool completed = terminal_;
    while (!completed)
    {
        const auto generation = progress_generation_;
        if (!changed_.wait_for(lock,
                               std::chrono::seconds(60),
                               [this, generation]
                               {
                                   return terminal_ || progress_generation_ != generation;
                               }))
        {
            break;
        }
        completed = terminal_;
    }
    lock.unlock();

    if (!completed)
    {
        fail(ERROR_TIMEOUT, "encoder finalization timed out");
    }

    join();
    cleanup();
    return snapshot();
}

void EncoderClient::Impl::cancel()
{
    std::uint64_t session = 0;
    {
        std::lock_guard lock(mutex_);
        if (terminal_ || cancel_requested_)
        {
            return;
        }

        cancel_requested_ = true;
        if (!started_)
        {
            result_.outcome = EncoderOutcome::cancelled;
            terminal_ = true;
        }
        session = session_;
    }

    changed_.notify_all();
    transport_.cancel_if_unlaunched();

    if (session == 0)
    {
        std::lock_guard lock(mutex_);
        if (terminal_)
        {
            return;
        }
    }

    if (session != 0)
    {
        const auto bytes = header(protocol::MessageType::cancel, session, 0, 0);
        static_cast<void>(transport_.write_control(bytes, 1000));
    }

    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock,
                           std::chrono::seconds(2),
                           [this]
                           {
                               return terminal_;
                           }))
    {
        result_.outcome = EncoderOutcome::cancelled;
        result_.diagnostic =
            "capture cancelled; destination may exist if publication already committed";
        terminal_ = true;
    }
    lock.unlock();

    changed_.notify_all();
    transport_.stop();
}

EncoderResult EncoderClient::Impl::snapshot() const
{
    std::lock_guard lock(mutex_);
    return result_;
}

EncoderClient::EncoderClient() : impl_(std::make_unique<Impl>())
{
}

EncoderClient::~EncoderClient() = default;

bool EncoderClient::start(const EncoderConfig& config)
{
    try
    {
        return impl_->start(config);
    }
    catch (...)
    {
        impl_->exception_failure();
        return false;
    }
}

bool EncoderClient::submit_frame(std::span<const std::byte> pixels)
{
    try
    {
        return impl_->submit(pixels);
    }
    catch (...)
    {
        impl_->exception_failure();
        return false;
    }
}

bool EncoderClient::end_video()
{
    try
    {
        return impl_->end_video();
    }
    catch (...)
    {
        impl_->exception_failure();
        return false;
    }
}

bool EncoderClient::set_audio_format(const EncoderAudioFormat& format)
{
    try
    {
        return impl_->set_audio_format(format);
    }
    catch (...)
    {
        impl_->exception_failure();
        return false;
    }
}

bool EncoderClient::submit_audio(std::uint64_t start_sample,
                                 std::uint32_t sample_count,
                                 std::span<const std::byte> pcm)
{
    try
    {
        return impl_->submit_audio(start_sample, sample_count, pcm);
    }
    catch (...)
    {
        impl_->exception_failure();
        return false;
    }
}

EncoderResult EncoderClient::finish()
{
    return impl_->finish();
}

void EncoderClient::cancel()
{
    impl_->cancel();
}

EncoderResult EncoderClient::failure() const
{
    return impl_->snapshot();
}

bool EncoderClient::has_failed() const noexcept
{
    return impl_->failed();
}
} // namespace cinemania::dll
