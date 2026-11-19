#pragma once

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace cinemania::audio_capture
{
// One callback thread produces; one owner thread consumes. The producer must be
// quiescent before destruction, and neither thread may use the object afterward.
class MixerPcmBuffer
{
public:
    // Capacity must be nonzero and representable as stereo PCM16 bytes.
    explicit MixerPcmBuffer(std::size_t capacity_frames);

    // Copies complete stereo frames into owned storage. Invalid or empty input
    // fails; insufficient space also latches overflow without changing the queue.
    [[nodiscard]] bool push(std::span<const std::byte> source) noexcept;

    // Copies exactly the requested complete frames. On failure destination and
    // queue are unchanged. Only the consumer calls this and available_frames().
    [[nodiscard]] bool read(std::span<std::byte> destination) noexcept;
    [[nodiscard]] std::size_t available_frames() const noexcept;
    [[nodiscard]] bool has_overflowed() const noexcept;

private:
    [[nodiscard]] std::size_t distance(std::size_t write, std::size_t read) const noexcept;
    [[nodiscard]] std::size_t advance(std::size_t cursor, std::size_t frames) const noexcept;

    const std::size_t capacity_frames_;
    std::vector<std::byte> storage_;
    // Cursors wrap in [0, 2 * capacity), with distance at most capacity.
    // The second epoch distinguishes full from empty and avoids integer wrap
    // changing the storage offset when capacity is not a power of two.
    // Each cursor has one writer; the other thread acquires its published value.
    std::atomic<std::size_t> write_cursor_{0};
    std::atomic<std::size_t> read_cursor_{0};
    std::atomic<bool> overflowed_{false};
};
} // namespace cinemania::audio_capture
