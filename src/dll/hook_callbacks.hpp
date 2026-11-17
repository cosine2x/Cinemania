#pragma once

// clang-format off
// Windows types must precede VfW declarations.
#include <windows.h>
#include <vfw.h>
// clang-format on

namespace cinemania::hook_callbacks
{
using DoShoot = void(__thiscall*)(void*, void**);
using Abort = unsigned int(__thiscall*)(void*);
using Open = unsigned int(__thiscall*)(void*, void*);
using Localize = const wchar_t*(__thiscall*)(void*, const wchar_t*);

extern DoShoot do_shoot_original;
extern Abort abort_original;
extern Open open_original;
extern Localize localize_original;
extern decltype(&AVIFileRelease) file_release_original;
extern decltype(&AVIStreamWrite) stream_write_original;
extern decltype(&AVIStreamSetFormat) stream_set_format_original;
extern decltype(&AVIMakeCompressedStream) make_compressed_original;
extern decltype(&AVISaveOptions) save_options_original;
extern decltype(&AVIFileCreateStreamW) create_stream_original;
extern decltype(&AVIStreamRelease) stream_release_original;

void __fastcall do_shoot(void* shooter, void*, void** fiber);
unsigned int __fastcall abort_requested(void* shooter, void*);
unsigned int __fastcall open_export(void* writer, void*, void* path);
const wchar_t* __fastcall localize(void* localizer, void*, const wchar_t* key);
ULONG WINAPI file_release(PAVIFILE file);
HRESULT WINAPI stream_write(PAVISTREAM stream,
                            LONG start,
                            LONG samples,
                            LPVOID buffer,
                            LONG bytes,
                            DWORD flags,
                            LONG* samples_written,
                            LONG* bytes_written);
HRESULT WINAPI stream_set_format(PAVISTREAM stream, LONG position, LPVOID format, LONG bytes);
HRESULT WINAPI make_compressed(PAVISTREAM* output,
                               PAVISTREAM source,
                               AVICOMPRESSOPTIONS* options,
                               CLSID* handler);
INT_PTR WINAPI save_options(
    HWND window, UINT flags, int count, PAVISTREAM* streams, LPAVICOMPRESSOPTIONS* options);
HRESULT WINAPI create_stream(PAVIFILE file, PAVISTREAM* stream, AVISTREAMINFOW* info);
ULONG WINAPI stream_release(PAVISTREAM stream);
} // namespace cinemania::hook_callbacks
