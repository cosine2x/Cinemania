#pragma once

#include "video/video_export_internal.hpp"

#include <vfw.h>

namespace cinemania::game_export::detail
{
bool create_audio_stream(const std::shared_ptr<Session>& session,
                         PAVISTREAM* stream,
                         AVISTREAMINFOW* info,
                         HRESULT& result);
bool set_audio_format(const std::shared_ptr<Session>& session,
                      LONG position,
                      LPVOID format,
                      LONG bytes,
                      HRESULT& result);
bool write_audio(const std::shared_ptr<Session>& session,
                 LONG start,
                 LONG samples,
                 LPVOID buffer,
                 LONG bytes,
                 DWORD flags,
                 LONG* samples_written,
                 LONG* bytes_written,
                 HRESULT& result);
void log_audio_outcome(const Session& session) noexcept;
} // namespace cinemania::game_export::detail
