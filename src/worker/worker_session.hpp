#pragma once

#include "native_runtime.hpp"

#include <cstdint>
#include <string>

namespace cinemania::worker
{
struct WorkerOptions
{
    std::wstring ffmpeg;
    HANDLE data = nullptr;
    HANDLE control = nullptr;
    HANDLE status = nullptr;
    std::uint64_t session = 0;
};

[[nodiscard]] int run_session(const WorkerOptions& options);
} // namespace cinemania::worker
