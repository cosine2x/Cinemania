#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d9.h>
#include <span>
#include <wrl/client.h>

namespace cinemania::dll
{
// All methods, including destruction, belong to the device and encoder producer thread.
struct ReadbackStats
{
    std::uint64_t transfer_ticks = 0;
    std::uint64_t lock_ticks = 0;
    std::uint64_t conversion_ticks = 0;
    std::uint64_t transfers = 0;
    std::uint64_t locks = 0;
};

class ReadbackRing
{
public:
    ReadbackRing() = default;
    ReadbackRing(const ReadbackRing&) = delete;
    ReadbackRing& operator=(const ReadbackRing&) = delete;

    [[nodiscard]] HRESULT
    initialize(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format);
    [[nodiscard]] HRESULT issue(IDirect3DSurface9* source, std::uint64_t frame_index);
    // S_FALSE means the oldest ticket is not ready. On S_OK output contains one frame.
    [[nodiscard]] HRESULT
    retire(std::span<std::byte> output, std::uint64_t& frame_index, bool flush);
    void reset() noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept;
    [[nodiscard]] const ReadbackStats& stats() const noexcept;
    [[nodiscard]] bool
    matches(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format) const noexcept;

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> surface;
        Microsoft::WRL::ComPtr<IDirect3DQuery9> query;
        std::uint64_t index = 0;
    };

    std::array<Slot, 3> slots_{};
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device_;
    ReadbackStats stats_{};
    D3DFORMAT format_ = D3DFMT_UNKNOWN;
    UINT width_ = 0;
    UINT height_ = 0;
    std::size_t depth_ = 0;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};
} // namespace cinemania::dll
