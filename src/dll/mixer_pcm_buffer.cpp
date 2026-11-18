#include "mixer_pcm_buffer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cinemania::audio_capture
{
namespace
{
constexpr std::size_t kBytesPerFrame = 4;
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

std::size_t checked_storage_bytes(std::size_t capacity_frames)
{
    if (capacity_frames == 0 ||
        capacity_frames > std::numeric_limits<std::size_t>::max() / kBytesPerFrame)
    {
        throw std::invalid_argument("Invalid mixer PCM buffer capacity");
    }

    return capacity_frames * kBytesPerFrame;
}
} // namespace

MixerPcmBuffer::MixerPcmBuffer(std::size_t capacity_frames)
    : capacity_frames_(capacity_frames), storage_(checked_storage_bytes(capacity_frames))
{
}

bool MixerPcmBuffer::push(std::span<const std::byte> source) noexcept
{
    if (source.empty() || source.size() % kBytesPerFrame != 0)
    {
        return false;
    }

    const std::size_t frames = source.size() / kBytesPerFrame;
    const std::size_t write = write_cursor_.load(std::memory_order_relaxed);
    // Acquire pairs with the consumer's release, ensuring its copy finishes
    // before the producer reuses the corresponding storage.
    const std::size_t read = read_cursor_.load(std::memory_order_acquire);

    if (frames > capacity_frames_ - distance(write, read))
    {
        overflowed_.store(true, std::memory_order_relaxed);
        return false;
    }

    const std::size_t offset_bytes = (write % capacity_frames_) * kBytesPerFrame;
    const std::size_t first_bytes = std::min(source.size(), storage_.size() - offset_bytes);
    std::memcpy(storage_.data() + offset_bytes, source.data(), first_bytes);
    if (first_bytes != source.size())
    {
        std::memcpy(storage_.data(), source.data() + first_bytes, source.size() - first_bytes);
    }

    // Publish only after both parts of the source have been copied.
    write_cursor_.store(advance(write, frames), std::memory_order_release);
    return true;
}

bool MixerPcmBuffer::read(std::span<std::byte> destination) noexcept
{
    if (destination.empty() || destination.size() % kBytesPerFrame != 0)
    {
        return false;
    }

    const std::size_t frames = destination.size() / kBytesPerFrame;
    const std::size_t read = read_cursor_.load(std::memory_order_relaxed);
    // Acquire pairs with the producer's release, making its completed copy visible.
    const std::size_t write = write_cursor_.load(std::memory_order_acquire);

    if (frames > distance(write, read))
    {
        return false;
    }

    const std::size_t offset_bytes = (read % capacity_frames_) * kBytesPerFrame;
    const std::size_t first_bytes = std::min(destination.size(), storage_.size() - offset_bytes);
    std::memcpy(destination.data(), storage_.data() + offset_bytes, first_bytes);
    if (first_bytes != destination.size())
    {
        std::memcpy(
            destination.data() + first_bytes, storage_.data(), destination.size() - first_bytes);
    }

    read_cursor_.store(advance(read, frames), std::memory_order_release);
    return true;
}

std::size_t MixerPcmBuffer::available_frames() const noexcept
{
    const std::size_t read = read_cursor_.load(std::memory_order_relaxed);
    const std::size_t write = write_cursor_.load(std::memory_order_acquire);
    return distance(write, read);
}

bool MixerPcmBuffer::has_overflowed() const noexcept
{
    return overflowed_.load(std::memory_order_relaxed);
}

std::size_t MixerPcmBuffer::distance(std::size_t write, std::size_t read) const noexcept
{
    if (write >= read)
    {
        return write - read;
    }

    return capacity_frames_ * 2 - (read - write);
}

std::size_t MixerPcmBuffer::advance(std::size_t cursor, std::size_t frames) const noexcept
{
    const std::size_t remaining = capacity_frames_ * 2 - cursor;
    if (frames >= remaining)
    {
        return frames - remaining;
    }

    return cursor + frames;
}
} // namespace cinemania::audio_capture
