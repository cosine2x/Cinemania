#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <windows.h>

namespace cinemania::game_abi
{
struct WideStringView
{
    std::uint32_t length;
    const wchar_t* text;
};

static_assert(sizeof(WideStringView) == 8);

inline unsigned char* executable_image() noexcept
{
    return reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
}

// Callers supply a verified address and the exact x86 calling convention.
template <typename Function>
Function function_at(const unsigned char* image, std::size_t rva) noexcept
{
    static_assert(std::is_function_v<std::remove_pointer_t<Function>>);
    const auto address = reinterpret_cast<std::uintptr_t>(image + rva);
    return reinterpret_cast<Function>(address);
}

// The caller knows the field's type and alignment in the pinned game layout.
template <typename T> const T& field(const void* object, std::size_t offset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

template <typename T> T& field(void* object, std::size_t offset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

inline bool has_vtable(const void* object, const void* vtable) noexcept
{
    return object != nullptr && field<const void*>(object, 0) == vtable;
}

inline bool fiber_finished(const void* fiber) noexcept
{
    return fiber == reinterpret_cast<const void*>(static_cast<std::uintptr_t>(-1));
}

using SehPrologue = std::array<unsigned char, 7>;

// The loader relocates the exception handler immediate in this MSVC x86 prologue.
inline SehPrologue seh_prologue(const void* handler) noexcept
{
    SehPrologue bytes{0x6a, 0xff, 0x68};
    const auto address = reinterpret_cast<std::uintptr_t>(handler);
    static_assert(sizeof(address) == 4);
    std::memcpy(bytes.data() + 3, &address, sizeof(address));
    return bytes;
}
} // namespace cinemania::game_abi
