#include "hook_callbacks.hpp"

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
    do_shoot_original(shooter, fiber);
}

unsigned int __fastcall abort_requested(void* shooter, void*)
{
    return abort_original(shooter);
}

unsigned int __fastcall open_export(void* writer, void*, void* path)
{
    return open_original(writer, path);
}

const wchar_t* __fastcall localize(void* localizer, void*, const wchar_t* key)
{
    return localize_original(localizer, key);
}

ULONG WINAPI file_release(PAVIFILE file)
{
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
    return stream_write_original(
        stream, start, samples, buffer, bytes, flags, samples_written, bytes_written);
}

HRESULT WINAPI stream_set_format(PAVISTREAM stream, LONG position, LPVOID format, LONG bytes)
{
    return stream_set_format_original(stream, position, format, bytes);
}

HRESULT WINAPI make_compressed(PAVISTREAM* output,
                               PAVISTREAM source,
                               AVICOMPRESSOPTIONS* options,
                               CLSID* handler)
{
    return make_compressed_original(output, source, options, handler);
}

INT_PTR WINAPI
save_options(HWND window, UINT flags, int count, PAVISTREAM* streams, LPAVICOMPRESSOPTIONS* options)
{
    return save_options_original(window, flags, count, streams, options);
}

HRESULT WINAPI create_stream(PAVIFILE file, PAVISTREAM* stream, AVISTREAMINFOW* info)
{
    return create_stream_original(file, stream, info);
}

ULONG WINAPI stream_release(PAVISTREAM stream)
{
    return stream_release_original(stream);
}
} // namespace cinemania::hook_callbacks
