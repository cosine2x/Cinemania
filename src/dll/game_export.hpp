#pragma once

// clang-format off
// The VfW SDK header requires Windows types first.
#include <windows.h>
#include <vfw.h>
// clang-format on

namespace cinemania::game_export
{
// Per-call scope; the fiber reference is borrowed only until the original call returns.
struct Invocation
{
    Invocation(void* shooter, void** fiber) noexcept;
    ~Invocation();
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    void* previous_;
    void* shooter_;
    void* key_ = nullptr;
    void** fiber_;
};

// Called once after all hook redirects are installed and verified.
void enable_video_routing() noexcept;

bool open(void* writer, void* path, const void* caller, unsigned int& result);
unsigned int abort_result(void* shooter, unsigned int original);
const wchar_t* message(const wchar_t* key, const void* caller, const wchar_t* original);
bool file_release(PAVIFILE file, ULONG& result);
bool create_stream(PAVIFILE file, PAVISTREAM* stream, AVISTREAMINFOW* info, HRESULT& result);
bool save_options(int count, PAVISTREAM* streams, LPAVICOMPRESSOPTIONS* options, INT_PTR& result);
bool make_compressed(PAVISTREAM* output,
                     PAVISTREAM source,
                     AVICOMPRESSOPTIONS* options,
                     CLSID* handler,
                     HRESULT& result);
bool set_format(PAVISTREAM stream, LONG position, LPVOID format, LONG bytes, HRESULT& result);
bool write(PAVISTREAM stream,
           LONG start,
           LONG samples,
           LPVOID buffer,
           LONG bytes,
           LONG* samples_written,
           LONG* bytes_written,
           HRESULT& result);
bool stream_release(PAVISTREAM stream, ULONG& result);
void exception() noexcept;
} // namespace cinemania::game_export
