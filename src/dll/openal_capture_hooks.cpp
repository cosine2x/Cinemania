#include "openal_capture_hooks.hpp"

#include "export_volume.hpp"
#include "game_export.hpp"
#include "openal_capture.hpp"
#include "openal_capture_compat.hpp"
#include "video_export_internal.hpp"

#include <MinHook.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <vector>

#include <bcrypt.h>
#include <windows.h>

namespace cinemania::openal_capture_hooks
{
namespace
{
using game_export::detail::Session;
using SetCapture = void(__thiscall*)(void*, unsigned int);
using Mix = void(__cdecl*)(void*, void*, int, int);
using Open = void*(__cdecl*)(const char*, unsigned int, int, int);
using Close = char(__cdecl*)(void*);
using Action = void(__cdecl*)(void*);
using Samples = void(__cdecl*)(void*, void*, int);
using Integer = void(__cdecl*)(void*, int, int, int*);
using Error = int(__cdecl*)(void*);

SetCapture original_set_capture = nullptr;
Mix original_mix = nullptr;
Open original_open = nullptr;
Close original_close = nullptr;
Action original_start = nullptr;
Action original_stop = nullptr;
Samples original_samples = nullptr;
Integer original_integer = nullptr;
Error original_error = nullptr;
unsigned char* game_base = nullptr;
std::mutex install_mutex;
bool mixer_installed = false;
bool mixer_attempted = false;
std::array<unsigned char, 8> set_capture_bytes{0x6a, 0xff, 0x68, 0, 0, 0, 0, 0};
constexpr std::array<std::size_t, 7> kImportRvas{
    0x728398, 0x728394, 0x7283bc, 0x7283c0, 0x728390, 0x728384, 0x728400};
constexpr std::array<const char*, 7> kNames{"alcCaptureOpenDevice",
                                            "alcCaptureCloseDevice",
                                            "alcCaptureStart",
                                            "alcCaptureStop",
                                            "alcCaptureSamples",
                                            "alcGetIntegerv",
                                            "alcGetError"};
constexpr std::array<unsigned char, 32> kRouterHash{
    0x26, 0xba, 0x69, 0x5d, 0x73, 0xfd, 0xf7, 0x8f, 0xf1, 0x33, 0xab, 0x9c, 0x0e, 0x12, 0x30, 0x0c,
    0x26, 0x07, 0x64, 0x41, 0x62, 0x7a, 0x25, 0xff, 0xba, 0x80, 0xc8, 0xb8, 0xae, 0xdb, 0x10, 0xec};
constexpr std::array<unsigned char, 32> kWrapperHash{
    0xc2, 0xef, 0x86, 0xcb, 0xdb, 0x7f, 0xa0, 0x7a, 0x9d, 0xa8, 0xf5, 0x6d, 0x5f, 0xfa, 0x54, 0x8c,
    0x57, 0xc5, 0xea, 0xaf, 0xd1, 0xce, 0x54, 0x62, 0xed, 0x39, 0x7d, 0xe7, 0xb8, 0xc8, 0x23, 0xa3};
constexpr std::array<unsigned char, 16> kMixBytes{
    0x8b, 0x44, 0x24, 0x04, 0x83, 0xec, 0x64, 0x85, 0xc0, 0x0f, 0x84, 0x6e, 0x0f, 0x00, 0x00, 0x50};

bool readable(const void* pointer, std::size_t bytes, const void* allocation = nullptr) noexcept
{
    auto cursor = reinterpret_cast<std::uintptr_t>(pointer);
    if (!cursor || bytes == 0 || cursor > UINTPTR_MAX - bytes)
    {
        return false;
    }

    const auto end = cursor + bytes;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            (allocation && info.AllocationBase != allocation))
        {
            return false;
        }

        const DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
        {
            return false;
        }

        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor)
        {
            return false;
        }

        cursor = next;
    }

    return true;
}

bool hash_module(HMODULE module, const std::array<unsigned char, 32>& expected)
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
    {
        return false;
    }
    HANDLE file = CreateFileW(path.data(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0)
    {
        std::array<unsigned char, 65536> block{};
        DWORD count = 0;
        bool valid = true;
        for (;;)
        {
            if (!ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &count, nullptr))
            {
                valid = false;
                break;
            }

            if (!count)
            {
                break;
            }

            if (BCryptHashData(hash, block.data(), count, 0) < 0)
            {
                valid = false;
                break;
            }
        }

        std::array<unsigned char, 32> digest{};
        success =
            valid && count == 0 &&
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
            digest == expected;
    }

    if (hash)
    {
        BCryptDestroyHash(hash);
    }

    if (algorithm)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }

    CloseHandle(file);
    return success;
}

bool valid_image(HMODULE module, std::size_t minimum_size) noexcept
{
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (!readable(base, sizeof(IMAGE_DOS_HEADER), module))
    {
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000)
    {
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return readable(nt, sizeof(*nt), module) && nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
           nt->FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32) &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
           nt->OptionalHeader.SizeOfImage >= minimum_size;
}

bool word(void* object, std::size_t offset, std::uint32_t& value) noexcept
{
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (address > UINTPTR_MAX - offset || !readable(reinterpret_cast<void*>(address + offset), 4))
    {
        return false;
    }

    std::memcpy(&value, reinterpret_cast<void*>(address + offset), 4);
    return true;
}

void __cdecl mix(void* context, void* destination, int bytes, int format)
{
    void* identity = audio_capture::begin_mix(context);
    original_mix(context, destination, bytes, format);
    audio_capture::finish_mix(identity, context, destination, bytes, format);
}

bool install_mixer()
{
    std::lock_guard lock(install_mutex);
    if (mixer_attempted)
    {
        return mixer_installed;
    }

    mixer_attempted = true;
    HMODULE router = GetModuleHandleW(L"OpenAL32.dll");
    HMODULE wrapper = GetModuleHandleW(L"wrap_oal.dll");
    HMODULE pinned = nullptr;
    if (!router || !wrapper ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(wrapper),
                            &pinned) ||
        pinned != wrapper || !valid_image(router, 1) || !valid_image(wrapper, 0x47580) ||
        !hash_module(router, kRouterHash) || !hash_module(wrapper, kWrapperHash))
    {
        return false;
    }
    auto* target = reinterpret_cast<unsigned char*>(wrapper) + 0x47570;
    if (!readable(target, kMixBytes.size(), wrapper) ||
        std::memcmp(target, kMixBytes.data(), kMixBytes.size()) != 0)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION target_info{};
    if (VirtualQuery(target, &target_info, sizeof(target_info)) != sizeof(target_info) ||
        (target_info.Protect & 0xff) != PAGE_EXECUTE_READ)
    {
        return false;
    }

    if (MH_CreateHook(
            target, reinterpret_cast<void*>(mix), reinterpret_cast<void**>(&original_mix)) != MH_OK)
    {
        return false;
    }
    // Retain the trampoline after any activation attempt, even on uncertainty.
    mixer_installed = MH_EnableHook(target) == MH_OK;
    return mixer_installed;
}

void* playback_context(void* port) noexcept
{
    std::uint32_t device = 0;
    std::uint32_t proxy = 0;
    std::uint32_t proxy_device = 0;
    std::uint32_t context = 0;
    std::uint32_t backend_device = 0;
    std::uint32_t context_device = 0;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t format = 0;
    if (!word(port, audio_capture::compat::kPortDevice, device) ||
        !word(port, audio_capture::compat::kPortContext, proxy) || !device || !proxy ||
        !word(reinterpret_cast<void*>(proxy),
              audio_capture::compat::kProxyContextDevice,
              proxy_device) ||
        proxy_device != device ||
        !word(
            reinterpret_cast<void*>(proxy), audio_capture::compat::kProxyContextBackend, context) ||
        !context ||
        !word(reinterpret_cast<void*>(device),
              audio_capture::compat::kProxyDeviceBackend,
              backend_device) ||
        !backend_device ||
        !word(reinterpret_cast<void*>(context),
              audio_capture::compat::kMixContextDevice,
              context_device) ||
        context_device != backend_device ||
        !word(reinterpret_cast<void*>(context), audio_capture::compat::kMixContextFormat, rate) ||
        !word(reinterpret_cast<void*>(context),
              audio_capture::compat::kMixContextFormat + 4,
              channels) ||
        !word(reinterpret_cast<void*>(context),
              audio_capture::compat::kMixContextFormat + 8,
              format) ||
        rate != 44100 || channels != 2 || format != 0x1103)
    {
        return nullptr;
    }

    return reinterpret_cast<void*>(context);
}

void start_owned_capture(void* port, const std::shared_ptr<Session>& session)
{
    std::uint32_t existing_capture = 0;
    if (!word(port, audio_capture::compat::kPortCapture, existing_capture) || existing_capture != 0)
    {
        audio_capture::record_failure(*session, game_export::detail::CaptureFailure::source);
        return;
    }

    bool ready = false;
    try
    {
        ready = prepare_mixer();
        void* context = ready ? playback_context(port) : nullptr;
        ready = context && audio_capture::bind(session, context);

        std::uint32_t proxy_context = 0;
        if (ready &&
            (!word(port, audio_capture::compat::kPortContext, proxy_context) ||
             !export_volume::activate(session, port, reinterpret_cast<void*>(proxy_context))))
        {
            audio_capture::record_failure(*session, game_export::detail::CaptureFailure::volume);
            ready = false;
        }

        if (!ready)
        {
            audio_capture::detach(session);
        }
    }
    catch (...)
    {
        ready = false;
    }

    if (!ready)
    {
        audio_capture::record_failure(*session, game_export::detail::CaptureFailure::source);
    }

    struct OpenScope
    {
        explicit OpenScope(const std::shared_ptr<Session>& active) noexcept
        {
            audio_capture::enter_open(active);
        }

        ~OpenScope()
        {
            audio_capture::leave_open();
        }
    } scope(session);

    original_set_capture(port, 1);
}

void stop_owned_capture(void* port, const std::shared_ptr<Session>& session)
{
    std::uint32_t existing_capture = 0;
    if (!word(port, audio_capture::compat::kPortCapture, existing_capture) ||
        (existing_capture &&
         !audio_capture::is_synthetic(reinterpret_cast<void*>(existing_capture))))
    {
        audio_capture::record_failure(*session, game_export::detail::CaptureFailure::source);
        audio_capture::detach(session);
        return;
    }

    original_set_capture(port, 0);
    audio_capture::detach(session);
}

void set_capture_impl(void* port, unsigned int enabled, const void* return_address)
{
    auto session = game_export::detail::current_session();
    const auto caller = reinterpret_cast<std::uintptr_t>(return_address);
    const auto base = reinterpret_cast<std::uintptr_t>(game_base);
    if (!session || !session->audio_expected ||
        !((enabled == 1 && caller == base + 0x2f509c) ||
          (enabled == 0 && caller == base + 0x2f51c8)))
    {
        original_set_capture(port, enabled);
        return;
    }

    if (enabled)
    {
        start_owned_capture(port, session);
    }
    else
    {
        stop_owned_capture(port, session);
    }
}

void __fastcall set_capture(void* port, void*, unsigned int enabled)
{
    try
    {
        set_capture_impl(port, enabled, _ReturnAddress());
    }
    catch (...)
    {
        game_export::exception();
    }
}

void* __cdecl capture_open(const char* name, unsigned int frequency, int format, int capacity)
{
    bool handled = false;
    void* result = audio_capture::capture_open(name, frequency, format, capacity, handled);
    return handled ? result : original_open(name, frequency, format, capacity);
}

char __cdecl capture_close(void* device)
{
    char result = 0;
    return audio_capture::capture_close(device, result) ? result : original_close(device);
}

void __cdecl capture_start(void* device)
{
    if (!audio_capture::capture_start(device))
    {
        original_start(device);
    }
}

void __cdecl capture_stop(void* device)
{
    if (!audio_capture::capture_stop(device))
    {
        original_stop(device);
    }
}

void __cdecl capture_samples(void* device, void* destination, int frames)
{
    if (!audio_capture::capture_samples(device, destination, frames))
    {
        original_samples(device, destination, frames);
    }
}

void __cdecl capture_integer(void* device, int parameter, int count, int* values)
{
    if (!audio_capture::capture_integer(device, parameter, count, values))
    {
        original_integer(device, parameter, count, values);
    }
}

int __cdecl capture_error(void* device)
{
    int result = 0;
    return audio_capture::capture_error(device, result) ? result : original_error(device);
}
} // namespace

bool prepare_mixer() noexcept
{
    try
    {
        return install_mixer();
    }
    catch (...)
    {
        return false;
    }
}

bool prepare(unsigned char* game,
             std::size_t image_size,
             EntryHook& entry,
             std::array<ImportHook, 7>& imports) noexcept
{
    HMODULE router = GetModuleHandleW(L"OpenAL32.dll");
    if (!router || image_size < 0x728404 || image_size < 0x6cb2a1 ||
        !readable(game + 0x3a6530, 7, game))
    {
        return false;
    }

    const auto handler = reinterpret_cast<std::uintptr_t>(game) + 0x6cb2a0;
    if (handler > UINT32_MAX)
    {
        return false;
    }

    const auto handler32 = static_cast<std::uint32_t>(handler);
    std::memcpy(set_capture_bytes.data() + 3, &handler32, sizeof(handler32));
    if (std::memcmp(game + 0x3a6530, set_capture_bytes.data(), 7) != 0)
    {
        return false;
    }

    std::array<void*, 7> originals{};
    const std::array<void*, 7> replacements{reinterpret_cast<void*>(capture_open),
                                            reinterpret_cast<void*>(capture_close),
                                            reinterpret_cast<void*>(capture_start),
                                            reinterpret_cast<void*>(capture_stop),
                                            reinterpret_cast<void*>(capture_samples),
                                            reinterpret_cast<void*>(capture_integer),
                                            reinterpret_cast<void*>(capture_error)};
    for (std::size_t index = 0; index < imports.size(); ++index)
    {
        auto** slot = reinterpret_cast<void**>(game + kImportRvas[index]);
        if (!readable(slot, sizeof(void*), game))
        {
            return false;
        }

        originals[index] = reinterpret_cast<void*>(GetProcAddress(router, kNames[index]));
        if (!originals[index] || *slot != originals[index])
        {
            return false;
        }

        imports[index] = {slot, originals[index], replacements[index]};
    }

    original_open = reinterpret_cast<Open>(originals[0]);
    original_close = reinterpret_cast<Close>(originals[1]);
    original_start = reinterpret_cast<Action>(originals[2]);
    original_stop = reinterpret_cast<Action>(originals[3]);
    original_samples = reinterpret_cast<Samples>(originals[4]);
    original_integer = reinterpret_cast<Integer>(originals[5]);
    original_error = reinterpret_cast<Error>(originals[6]);
    game_base = game;
    entry = {game + 0x3a6530,
             reinterpret_cast<void*>(set_capture),
             reinterpret_cast<void**>(&original_set_capture),
             set_capture_bytes.data(),
             7};
    return true;
}
} // namespace cinemania::openal_capture_hooks
