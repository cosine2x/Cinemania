#include "readback_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cinemania::dll
{
namespace
{
constexpr std::uint64_t kMaxStagingBytes = 64ULL * 1024 * 1024;

std::uint64_t ticks() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}
} // namespace

HRESULT
ReadbackRing::initialize(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format)
{
    reset();
    if (!device || !width || !height || width > 8192 || height > 8192 ||
        (format != D3DFMT_X8R8G8B8 && format != D3DFMT_A8R8G8B8))
    {
        return D3DERR_INVALIDCALL;
    }

    const std::uint64_t plane_bytes = static_cast<std::uint64_t>(width) * height * 4;
    if (plane_bytes > kMaxStagingBytes / 2)
    {
        return E_OUTOFMEMORY;
    }

    const std::size_t maximum_depth = plane_bytes <= kMaxStagingBytes / 3 ? 3 : 2;
    HRESULT result = E_OUTOFMEMORY;
    std::size_t depth = maximum_depth;
    for (; depth >= 2; --depth)
    {
        result = S_OK;
        for (std::size_t index = 0; index < depth; ++index)
        {
            result = device->CreateOffscreenPlainSurface(width,
                                                         height,
                                                         format,
                                                         D3DPOOL_SYSTEMMEM,
                                                         slots_[index].surface.GetAddressOf(),
                                                         nullptr);
            if (SUCCEEDED(result))
            {
                result =
                    device->CreateQuery(D3DQUERYTYPE_EVENT, slots_[index].query.GetAddressOf());
            }
            if (FAILED(result))
            {
                break;
            }
        }
        if (SUCCEEDED(result))
        {
            break;
        }
        reset();
    }

    if (FAILED(result))
    {
        return result;
    }

    stats_ = {};
    format_ = format;
    device_ = device;
    width_ = width;
    height_ = height;
    depth_ = depth;
    return S_OK;
}

HRESULT ReadbackRing::issue(IDirect3DSurface9* source, std::uint64_t frame_index)
{
    if (!device_ || !source || full())
    {
        return D3DERR_INVALIDCALL;
    }

    Slot& slot = slots_[(head_ + count_) % depth_];
    const auto transfer_start = ticks();
    HRESULT result = device_->GetRenderTargetData(source, slot.surface.Get());
    stats_.transfer_ticks += ticks() - transfer_start;
    ++stats_.transfers;
    if (FAILED(result))
    {
        return result;
    }
    result = slot.query->Issue(D3DISSUE_END);
    if (FAILED(result))
    {
        return result;
    }

    slot.index = frame_index;
    ++count_;
    return S_OK;
}

HRESULT ReadbackRing::retire(std::span<std::byte> output, std::uint64_t& frame_index, bool flush)
{
    if (empty())
    {
        return D3DERR_INVALIDCALL;
    }

    const std::uint64_t required = static_cast<std::uint64_t>(width_) * height_ * 3;
    if (required > output.size())
    {
        return D3DERR_INVALIDCALL;
    }

    Slot& slot = slots_[head_];
    HRESULT result = slot.query->GetData(nullptr, 0, flush ? D3DGETDATA_FLUSH : 0);
    if (result != S_OK)
    {
        return result;
    }

    D3DLOCKED_RECT locked{};
    const auto lock_start = ticks();
    result = slot.surface->LockRect(&locked, nullptr, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
    stats_.lock_ticks += ticks() - lock_start;
    ++stats_.locks;
    if (result == D3DERR_WASSTILLDRAWING)
    {
        return S_FALSE;
    }
    if (FAILED(result))
    {
        return result;
    }

    if (!locked.pBits || locked.Pitch < 0 ||
        static_cast<std::uint64_t>(locked.Pitch) < static_cast<std::uint64_t>(width_) * 4 ||
        static_cast<std::uint64_t>(locked.Pitch) * height_ >
            std::numeric_limits<std::ptrdiff_t>::max())
    {
        slot.surface->UnlockRect();
        return D3DERR_INVALIDCALL;
    }

    const auto conversion_start = ticks();
    const auto* source = static_cast<const std::byte*>(locked.pBits);
    const std::size_t destination_stride = static_cast<std::size_t>(width_) * 3;
    for (UINT row = 0; row < height_; ++row)
    {
        const auto* source_row =
            source + static_cast<std::size_t>(height_ - 1 - row) * locked.Pitch;
        auto* destination = output.data() + static_cast<std::size_t>(row) * destination_stride;
        for (UINT column = 0; column < width_; ++column)
        {
            destination[column * 3] = source_row[column * 4];
            destination[column * 3 + 1] = source_row[column * 4 + 1];
            destination[column * 3 + 2] = source_row[column * 4 + 2];
        }
    }

    stats_.conversion_ticks += ticks() - conversion_start;
    result = slot.surface->UnlockRect();
    if (FAILED(result))
    {
        return result;
    }
    frame_index = slot.index;
    head_ = (head_ + 1) % depth_;
    --count_;
    return S_OK;
}

void ReadbackRing::reset() noexcept
{
    for (Slot& slot : slots_)
    {
        slot.query.Reset();
        slot.surface.Reset();
        slot.index = 0;
    }
    device_.Reset();
    format_ = D3DFMT_UNKNOWN;
    width_ = 0;
    height_ = 0;
    depth_ = 0;
    head_ = 0;
    count_ = 0;
}

bool ReadbackRing::full() const noexcept
{
    return depth_ != 0 && count_ == depth_;
}

std::size_t ReadbackRing::depth() const noexcept
{
    return depth_;
}

const ReadbackStats& ReadbackRing::stats() const noexcept
{
    return stats_;
}

bool ReadbackRing::matches(IDirect3DDevice9* device,
                           UINT width,
                           UINT height,
                           D3DFORMAT format) const noexcept
{
    return device_.Get() == device && width_ == width && height_ == height && format_ == format;
}

bool ReadbackRing::empty() const noexcept
{
    return count_ == 0;
}
} // namespace cinemania::dll
