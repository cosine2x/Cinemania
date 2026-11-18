#include "hook_callbacks.hpp"

#include "game_export.hpp"

#include <intrin.h>

namespace cinemania::hook_callbacks
{
DoShoot do_shoot_original = nullptr;
Abort abort_original = nullptr;
Open open_original = nullptr;
Localize localize_original = nullptr;
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

const wchar_t* __fastcall localize(void* localizer, void*, const wchar_t* key)
{
    const void* caller = _ReturnAddress();
    const wchar_t* original = localize_original(localizer, key);

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
