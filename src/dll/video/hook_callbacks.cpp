#include "hook_callbacks.hpp"

#include "game/game_abi.hpp"
#include "game/game_ui.hpp"
#include "game_export.hpp"

#include <array>
#include <intrin.h>

namespace cinemania::hook_callbacks
{
DoShoot do_shoot_original = nullptr;
Abort abort_original = nullptr;
Open open_original = nullptr;
decltype(&AVIFileRelease) file_release_original = nullptr;
decltype(&AVIStreamWrite) stream_write_original = nullptr;
decltype(&AVIStreamSetFormat) stream_set_format_original = nullptr;
decltype(&AVIMakeCompressedStream) make_compressed_original = nullptr;
decltype(&AVISaveOptions) save_options_original = nullptr;
decltype(&AVIFileCreateStreamW) create_stream_original = nullptr;
decltype(&AVIStreamRelease) stream_release_original = nullptr;

void __fastcall do_shoot(void* shooter, void*, void** fiber)
{
    game_export::Invocation invocation(shooter, fiber);
    do_shoot_original(shooter, fiber);
}

unsigned int __fastcall abort_requested(void* shooter, void*)
{
    const unsigned int original = abort_original(shooter);

    try
    {
        return game_export::abort_result(shooter, original);
    }
    catch (...)
    {
        game_export::exception();
        return 1;
    }
}

unsigned int __fastcall open_export(void* writer, void*, void* path)
{
    const void* caller = _ReturnAddress();
    unsigned int result = 0;

    try
    {
        if (game_export::open(writer, path, caller, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return 0;
    }

    return open_original(writer, path);
}

namespace
{
game_ui::Localize game_localize = nullptr;

const wchar_t* __fastcall
localize_export_message(void* localizer, const void* caller, const wchar_t* key)
{
    const wchar_t* original = game_localize(localizer, key);

    try
    {
        return game_export::message(key, caller, original);
    }
    catch (...)
    {
        game_export::exception();
        return L"Video export failed. Check the destination for any output.";
    }
}

// MinHook jumps here in place of the five-byte CALL. ECX and the stacked key
// already match the localizer's ABI. Pass the native return address in EDX,
// then resume the shooter with the localized result in EAX and the key popped.
#define CINEMANIA_LOCALIZATION_BRIDGE(name)                                                        \
    const void* name##_continuation = nullptr;                                                     \
    __declspec(naked) void name()                                                                  \
    {                                                                                              \
        __asm { mov edx, dword ptr [name##_continuation] }                                              \
        __asm                                                                                      \
        {                                                                                          \
            call localize_export_message                                                           \
        }                                                                                          \
        __asm { jmp dword ptr [name##_continuation] }                                                  \
    }

CINEMANIA_LOCALIZATION_BRIDGE(localize_setup_dialog)
CINEMANIA_LOCALIZATION_BRIDGE(localize_setup_status)
CINEMANIA_LOCALIZATION_BRIDGE(localize_early_abort)
CINEMANIA_LOCALIZATION_BRIDGE(localize_late_abort)
CINEMANIA_LOCALIZATION_BRIDGE(localize_result_dialog)
CINEMANIA_LOCALIZATION_BRIDGE(localize_result_status)

#undef CINEMANIA_LOCALIZATION_BRIDGE
} // namespace

void prepare_localization_hooks(unsigned char* image,
                                std::span<EntryHook, kLocalizationHookCount> entries) noexcept
{
    struct CallSite
    {
        std::size_t rva;
        std::array<unsigned char, 5> bytes;
        void (*bridge)();
        const void** continuation;
    };

    // Shooter calls to 00902460 in TMUF 2.12.0. Calling the public game entry
    // from each bridge lets other mods hook localization independently.
    static constexpr std::array<CallSite, kLocalizationHookCount> sites{{
        {0x2f4a69,
         {0xe8, 0xf2, 0xd9, 0x20, 0},
         localize_setup_dialog,
         &localize_setup_dialog_continuation},
        {0x2f4b7e,
         {0xe8, 0xdd, 0xd8, 0x20, 0},
         localize_setup_status,
         &localize_setup_status_continuation},
        {0x2f4c5e,
         {0xe8, 0xfd, 0xd7, 0x20, 0},
         localize_early_abort,
         &localize_early_abort_continuation},
        {0x2f5144,
         {0xe8, 0x17, 0xd3, 0x20, 0},
         localize_late_abort,
         &localize_late_abort_continuation},
        {0x2f52ed,
         {0xe8, 0x6e, 0xd1, 0x20, 0},
         localize_result_dialog,
         &localize_result_dialog_continuation},
        {0x2f5409,
         {0xe8, 0x52, 0xd0, 0x20, 0},
         localize_result_status,
         &localize_result_status_continuation},
    }};

    game_localize = game_abi::function_at<game_ui::Localize>(image, game_ui::kLocalizerRva);
    for (std::size_t index = 0; index < sites.size(); ++index)
    {
        const CallSite& site = sites[index];
        auto* target = image + site.rva;
        *site.continuation = target + site.bytes.size();
        entries[index] = make_entry_hook(target, site.bridge, site.bytes);
    }
}

ULONG WINAPI file_release(PAVIFILE file)
{
    ULONG result = 0;

    try
    {
        if (game_export::file_release(file, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return 0;
    }

    return file_release_original(file);
}

HRESULT WINAPI stream_write(PAVISTREAM stream,
                            LONG start,
                            LONG samples,
                            LPVOID buffer,
                            LONG bytes,
                            DWORD flags,
                            LONG* samples_written,
                            LONG* bytes_written)
{
    HRESULT result = AVIERR_ERROR;

    try
    {
        if (game_export::write(stream,
                               start,
                               samples,
                               buffer,
                               bytes,
                               flags,
                               samples_written,
                               bytes_written,
                               result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return AVIERR_ERROR;
    }

    return stream_write_original(
        stream, start, samples, buffer, bytes, flags, samples_written, bytes_written);
}

HRESULT WINAPI stream_set_format(PAVISTREAM stream, LONG position, LPVOID format, LONG bytes)
{
    HRESULT result = AVIERR_ERROR;

    try
    {
        if (game_export::set_format(stream, position, format, bytes, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return AVIERR_ERROR;
    }

    return stream_set_format_original(stream, position, format, bytes);
}

HRESULT WINAPI make_compressed(PAVISTREAM* output,
                               PAVISTREAM source,
                               AVICOMPRESSOPTIONS* options,
                               CLSID* handler)
{
    HRESULT result = AVIERR_ERROR;

    try
    {
        if (game_export::make_compressed(output, source, options, handler, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return AVIERR_ERROR;
    }

    return make_compressed_original(output, source, options, handler);
}

INT_PTR WINAPI
save_options(HWND window, UINT flags, int count, PAVISTREAM* streams, LPAVICOMPRESSOPTIONS* options)
{
    INT_PTR result = FALSE;

    try
    {
        if (game_export::save_options(count, streams, options, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return FALSE;
    }

    return save_options_original(window, flags, count, streams, options);
}

HRESULT WINAPI create_stream(PAVIFILE file, PAVISTREAM* stream, AVISTREAMINFOW* info)
{
    HRESULT result = AVIERR_ERROR;

    try
    {
        if (game_export::create_stream(file, stream, info, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return AVIERR_ERROR;
    }

    return create_stream_original(file, stream, info);
}

ULONG WINAPI stream_release(PAVISTREAM stream)
{
    ULONG result = 0;

    try
    {
        if (game_export::stream_release(stream, result))
        {
            return result;
        }
    }
    catch (...)
    {
        game_export::exception();
        return 0;
    }

    return stream_release_original(stream);
}
} // namespace cinemania::hook_callbacks
