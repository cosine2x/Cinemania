#include "passthrough_hooks.hpp"

#include "export_volume.hpp"
#include "hook_callbacks.hpp"
#include "hook_transaction.hpp"
#include "openal_capture_hooks.hpp"

#include "viewport_capture_hooks.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

namespace cinemania
{
namespace
{
struct EntrySpec
{
    std::size_t rva;
    std::array<unsigned char, 8> bytes;
    std::size_t length;
};

constexpr std::array<EntrySpec, 3> kEntries{{
    {0x2f4510, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}, 6},
    {0x2f4480, {0x8b, 0x49, 0x50, 0x8b, 0x49, 0x14}, 6},
    {0x55a150, {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x04}, 6},
}};
// Static bytes from the pinned 2.12.0 image: capture and stop inside
// 009a4070, Reset at 0095c430, and resource teardown at 0095dba0.
constexpr std::array<EntrySpec, 4> kViewportEntries{{
    {0x5a44bc, {0x55, 0x8d, 0x8e, 0xfc, 0x09, 0x00, 0x00}, 7},
    {0x5a4649, {0x39, 0xae, 0x50, 0x05, 0x00, 0x00}, 6},
    {0x55c430, {0x6a, 0xff, 0x68, 0xa0, 0x96, 0xae, 0x00}, 7},
    {0x55dba0, {0x83, 0xec, 0x10, 0x56, 0x8b, 0xf1}, 6},
}};
constexpr std::size_t kEntryCount =
    kEntries.size() + kViewportEntries.size() + 3 + hook_callbacks::kLocalizationHookCount;
// Native EVENT ring functions in the pinned 2.12.0 image.
constexpr std::array<EntrySpec, 3> kRenderAheadFunctions{{
    {0x58bab0, {0x51, 0x57, 0x8b, 0xf9, 0x83, 0xbf, 0xf8, 0x09}, 8},
    {0x58b6e0, {0x83, 0xec, 0x08, 0x53, 0x55, 0x56, 0x8b, 0xf1}, 8},
    {0x58b6d0, {0x33, 0xc0, 0x89, 0x81, 0x3c, 0x09, 0x00, 0x00}, 8},
}};

constexpr std::array<std::size_t, 7> kImportRvas{
    0x728050, 0x728080, 0x728084, 0x728088, 0x72808c, 0x728090, 0x728094};
constexpr std::array<const char*, 7> kImportNames{"AVIFileRelease",
                                                  "AVIStreamWrite",
                                                  "AVIStreamSetFormat",
                                                  "AVIMakeCompressedStream",
                                                  "AVISaveOptions",
                                                  "AVIFileCreateStreamW",
                                                  "AVIStreamRelease"};

bool mapped_range(const void* address, std::size_t length, const void* image, bool executable)
{
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (length == 0 || begin > UINTPTR_MAX - length)
    {
        return false;
    }
    const auto end = begin + length;
    auto cursor = begin;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) !=
                sizeof(region) ||
            region.AllocationBase != image || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        const DWORD protection = region.Protect & 0xff;
        const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                              protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                              protection == PAGE_EXECUTE_READWRITE ||
                              protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable ||
            (executable && protection != PAGE_EXECUTE_READ &&
             protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY))
        {
            return false;
        }
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= cursor)
        {
            return false;
        }
        cursor = next;
    }
    return true;
}

bool image_size(unsigned char* base, std::size_t& size)
{
    if (!mapped_range(base, sizeof(IMAGE_DOS_HEADER), base, false))
    {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000)
    {
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(base) >
        UINTPTR_MAX - static_cast<std::size_t>(dos->e_lfanew))
    {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (!mapped_range(nt, sizeof(*nt), base, false) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        return false;
    }
    size = nt->OptionalHeader.SizeOfImage;
    return size != 0 && reinterpret_cast<std::uintptr_t>(base) <= UINTPTR_MAX - size;
}

bool in_image(std::size_t rva, std::size_t length, std::size_t size)
{
    return rva <= size && length <= size - rva;
}

template <typename Function> void* function_address(Function function)
{
    return reinterpret_cast<void*>(function);
}

template <typename Function> void** original_address(Function& function)
{
    // MinHook writes the executable trampoline address to this x86 function pointer slot.
    return reinterpret_cast<void**>(&function);
}
} // namespace

HookResult install_passthrough_hooks()
{
    HookResult failure{};
    failure.domain = HookErrorDomain::validation;
    failure.stage = "image validation";
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    std::size_t size = 0;
    if (base == nullptr || !image_size(base, size))
    {
        return failure;
    }
    for (const EntrySpec& entry : kEntries)
    {
        if (!in_image(entry.rva, entry.length, size) ||
            !mapped_range(base + entry.rva, entry.length, base, true) ||
            std::memcmp(base + entry.rva, entry.bytes.data(), entry.length) != 0)
        {
            failure.stage = "entry validation";
            return failure;
        }
    }

    for (const EntrySpec& entry : kRenderAheadFunctions)
    {
        if (!in_image(entry.rva, entry.length, size) ||
            !mapped_range(base + entry.rva, entry.length, base, true) ||
            std::memcmp(base + entry.rva, entry.bytes.data(), entry.length) != 0)
        {
            failure.stage = "native ring function validation";
            return failure;
        }
    }
    if (!in_image(0x9777ac, sizeof(std::uint32_t), size) ||
        !mapped_range(base + 0x9777ac, sizeof(std::uint32_t), base, false) ||
        !in_image(0x7d10bc, sizeof(void*), size) ||
        !mapped_range(base + 0x7d10bc, sizeof(void*), base, false))
    {
        failure.stage = "native ring data validation";
        return failure;
    }

    auto viewport_entries = kViewportEntries;
    // Reset's PUSH immediate references image data and is relocated by the
    // loader. Compare against the current image base, not the preferred base.
    const auto reset_handler = reinterpret_cast<std::uintptr_t>(base) + 0x6e96a0;
    if (!in_image(0x6e96a0, 1, size) || reset_handler > UINT32_MAX ||
        !in_image(0x5a4631, 1, size) || !mapped_range(base + 0x5a4631, 1, base, true))
    {
        failure.stage = "viewport reset handler validation";
        return failure;
    }
    const auto reset_handler32 = static_cast<std::uint32_t>(reset_handler);
    std::memcpy(viewport_entries[2].bytes.data() + 3, &reset_handler32, sizeof(reset_handler32));
    for (const EntrySpec& entry : viewport_entries)
    {
        if (!in_image(entry.rva, entry.length, size) ||
            !mapped_range(base + entry.rva, entry.length, base, true) ||
            std::memcmp(base + entry.rva, entry.bytes.data(), entry.length) != 0)
        {
            failure.stage = "viewport entry validation";
            return failure;
        }
    }
    viewport_capture_hooks::prepare(base);

    HMODULE avifile = GetModuleHandleW(L"avifil32.dll");
    if (avifile == nullptr)
    {
        failure.domain = HookErrorDomain::win32;
        failure.stage = "avifil32 module";
        failure.code = GetLastError();
        return failure;
    }
    std::array<void*, 7> originals{};
    for (std::size_t index = 0; index < originals.size(); ++index)
    {
        const std::size_t rva = kImportRvas[index];
        if (!in_image(rva, sizeof(void*), size) ||
            !mapped_range(base + rva, sizeof(void*), base, false))
        {
            failure.stage = "import validation";
            return failure;
        }
        originals[index] = reinterpret_cast<void*>(GetProcAddress(avifile, kImportNames[index]));
        if (originals[index] == nullptr ||
            *reinterpret_cast<void**>(base + rva) != originals[index])
        {
            failure.stage = "import owner";
            return failure;
        }
    }

    namespace callbacks = hook_callbacks;
    callbacks::file_release_original =
        reinterpret_cast<decltype(callbacks::file_release_original)>(originals[0]);
    callbacks::stream_write_original =
        reinterpret_cast<decltype(callbacks::stream_write_original)>(originals[1]);
    callbacks::stream_set_format_original =
        reinterpret_cast<decltype(callbacks::stream_set_format_original)>(originals[2]);
    callbacks::make_compressed_original =
        reinterpret_cast<decltype(callbacks::make_compressed_original)>(originals[3]);
    callbacks::save_options_original =
        reinterpret_cast<decltype(callbacks::save_options_original)>(originals[4]);
    callbacks::create_stream_original =
        reinterpret_cast<decltype(callbacks::create_stream_original)>(originals[5]);
    callbacks::stream_release_original =
        reinterpret_cast<decltype(callbacks::stream_release_original)>(originals[6]);

    std::array<EntryHook, kEntryCount> entries{{
        {base + kEntries[0].rva,
         function_address(callbacks::do_shoot),
         original_address(callbacks::do_shoot_original),
         kEntries[0].bytes.data(),
         kEntries[0].length},
        {base + kEntries[1].rva,
         function_address(callbacks::abort_requested),
         original_address(callbacks::abort_original),
         kEntries[1].bytes.data(),
         kEntries[1].length},
        {base + kEntries[2].rva,
         function_address(callbacks::open_export),
         original_address(callbacks::open_original),
         kEntries[2].bytes.data(),
         kEntries[2].length},
        {base + viewport_entries[0].rva,
         function_address(cinemania_capture_bridge),
         &cinemania_capture_original,
         viewport_entries[0].bytes.data(),
         viewport_entries[0].length},
        {base + viewport_entries[1].rva,
         function_address(cinemania_cleanup_bridge),
         &cinemania_cleanup_original,
         viewport_entries[1].bytes.data(),
         viewport_entries[1].length},
        {base + viewport_entries[2].rva,
         function_address(cinemania_reset_bridge),
         &cinemania_reset_original,
         viewport_entries[2].bytes.data(),
         viewport_entries[2].length},
        {base + viewport_entries[3].rva,
         function_address(cinemania_teardown_bridge),
         &cinemania_teardown_original,
         viewport_entries[3].bytes.data(),
         viewport_entries[3].length},
    }};
    if (!in_image(callbacks::kLocalizerRva, 1, size) ||
        !mapped_range(base + callbacks::kLocalizerRva, 1, base, true))
    {
        failure.stage = "localizer mapping";
        return failure;
    }
    const auto localization_entries = std::span(entries).last<callbacks::kLocalizationHookCount>();
    callbacks::prepare_localization_hooks(base, localization_entries);
    for (const EntryHook& entry : localization_entries)
    {
        if (!mapped_range(entry.target, entry.expected_size, base, true) ||
            std::memcmp(entry.target, entry.expected_bytes, entry.expected_size) != 0)
        {
            failure.stage = "export localization call validation";
            return failure;
        }
    }

    const std::array<void*, 7> replacements{function_address(callbacks::file_release),
                                            function_address(callbacks::stream_write),
                                            function_address(callbacks::stream_set_format),
                                            function_address(callbacks::make_compressed),
                                            function_address(callbacks::save_options),
                                            function_address(callbacks::create_stream),
                                            function_address(callbacks::stream_release)};
    std::array<ImportHook, 16> imports{};
    for (std::size_t index = 0; index < replacements.size(); ++index)
    {
        imports[index] = {reinterpret_cast<void**>(base + kImportRvas[index]),
                          originals[index],
                          replacements[index]};
    }
    std::array<ImportHook, 7> audio_imports{};
    if (!openal_capture_hooks::prepare(base, size, entries[7], audio_imports))
    {
        failure.stage = "OpenAL capture hook validation";
        return failure;
    }
    for (std::size_t index = 0; index < audio_imports.size(); ++index)
    {
        imports[index + replacements.size()] = audio_imports[index];
    }

    if (!in_image(0x39d6d2, 20, size) || !mapped_range(base + 0x39d6d2, 20, base, true) ||
        !in_image(0x7362a8, 8, size) || !mapped_range(base + 0x7362a8, 8, base, false) ||
        !in_image(0x741ea8, 8, size) || !mapped_range(base + 0x741ea8, 8, base, false) ||
        !in_image(0x7283d0, sizeof(void*), size) ||
        !mapped_range(base + 0x7283d0, sizeof(void*), base, false) ||
        !in_image(0x728388, sizeof(void*), size) ||
        !mapped_range(base + 0x728388, sizeof(void*), base, false))
    {
        failure.stage = "OpenAL gain image validation";
        return failure;
    }

    std::array<EntryHook, 2> gain_entries{};
    std::array<ImportHook, 2> gain_imports{};
    if (!export_volume::prepare(base, size, gain_entries, gain_imports))
    {
        failure.stage = "OpenAL gain hook validation";
        return failure;
    }
    entries[8] = gain_entries[0];
    entries[9] = gain_entries[1];
    imports[14] = gain_imports[0];
    imports[15] = gain_imports[1];
    return install_hooks(entries, imports);
}
} // namespace cinemania
