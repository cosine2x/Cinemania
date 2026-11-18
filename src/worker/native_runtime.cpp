#include "native_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace cinemania::worker
{
namespace
{
NativeError failure(DWORD code, const char* message)
{
    return {code, message};
}

DWORD wait_milliseconds(Deadline deadline)
{
    if (deadline == Deadline::max())
    {
        return INFINITE;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<DWORD>(
        (std::min)(remaining, static_cast<decltype(remaining)>(INFINITE - 1)));
}

NativeError transfer(HANDLE pipe,
                     void* buffer,
                     std::size_t size,
                     bool writing,
                     StopState& stop,
                     Deadline deadline,
                     HANDLE process,
                     std::uint64_t* file_offset = nullptr)
{
    OwnedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get())
    {
        return failure(GetLastError(), "CreateEvent for pipe I/O failed");
    }
    auto* position = static_cast<std::byte*>(buffer);
    while (size != 0)
    {
        if (stop.stopped())
        {
            return stop.error();
        }
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        if (file_offset)
        {
            operation.Offset = static_cast<DWORD>(*file_offset);
            operation.OffsetHigh = static_cast<DWORD>(*file_offset >> 32);
        }
        ResetEvent(event.get());
        const DWORD amount =
            static_cast<DWORD>((std::min)(size, static_cast<std::size_t>(MAXDWORD)));
        DWORD transferred = 0;
        const BOOL immediate = writing ? WriteFile(pipe, position, amount, &transferred, &operation)
                                       : ReadFile(pipe, position, amount, &transferred, &operation);
        if (!immediate)
        {
            const DWORD code = GetLastError();
            if (code != ERROR_IO_PENDING)
            {
                return failure(code, writing ? "pipe write failed" : "pipe read failed");
            }
            HANDLE waits[] = {event.get(), stop.event(), process};
            const DWORD count = process ? 3 : 2;
            const DWORD result =
                WaitForMultipleObjects(count, waits, FALSE, wait_milliseconds(deadline));
            if (result != WAIT_OBJECT_0)
            {
                DWORD wait_error = ERROR_OPERATION_ABORTED;
                if (result == WAIT_TIMEOUT)
                {
                    wait_error = ERROR_TIMEOUT;
                }
                else if (result == WAIT_FAILED)
                {
                    wait_error = GetLastError();
                }
                else if (result == WAIT_OBJECT_0 + 2)
                {
                    wait_error = ERROR_PROCESS_ABORTED;
                }
                CancelIoEx(pipe, &operation);
                // Cancellation is only a request; observe completion before releasing the buffer.
                GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                if (result == WAIT_OBJECT_0 + 1)
                {
                    NativeError stopped = stop.error();
                    return stopped ? stopped : failure(ERROR_OPERATION_ABORTED, "pipe I/O stopped");
                }
                return failure(wait_error, "pipe I/O interrupted");
            }
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
            {
                return failure(GetLastError(), "pipe I/O completion failed");
            }
        }
        if (transferred == 0)
        {
            return failure(ERROR_BROKEN_PIPE, "pipe closed");
        }
        if (file_offset)
        {
            *file_offset += transferred;
        }
        position += transferred;
        size -= transferred;
    }
    return {};
}

std::wstring quote_argument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++slashes;
        }
        else if (character == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(character);
            slashes = 0;
        }
        else
        {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

struct EnvironmentDeleter
{
    void operator()(wchar_t* environment) const noexcept
    {
        FreeEnvironmentStringsW(environment);
    }
};

struct AttributeDeleter
{
    void operator()(PPROC_THREAD_ATTRIBUTE_LIST attributes) const noexcept
    {
        DeleteProcThreadAttributeList(attributes);
    }
};

std::vector<wchar_t> child_environment(DWORD& error)
{
    std::vector<wchar_t> result;
    LPWCH raw_environment = GetEnvironmentStringsW();
    if (!raw_environment)
    {
        error = GetLastError();
        return result;
    }
    std::unique_ptr<wchar_t, EnvironmentDeleter> environment(raw_environment);
    for (const wchar_t* entry = environment.get(); *entry; entry += wcslen(entry) + 1)
    {
        const std::wstring value(entry);
        const bool loader_prefix = _wcsnicmp(entry, L"TMLoader", 8) == 0;
        const bool modloader = _wcsnicmp(entry, L"MODLOADER=", 10) == 0;
        const bool modloader_title = _wcsnicmp(entry, L"MODLOADER_TITLE=", 16) == 0;
        if (!loader_prefix && !modloader && !modloader_title)
        {
            result.insert(result.end(), value.begin(), value.end());
            result.push_back(L'\0');
        }
    }
    result.push_back(L'\0');
    if (result.size() == 1)
    {
        result.push_back(L'\0');
    }
    return result;
}

NativeError make_pipe(bool worker_reads, OwnedHandle& worker, OwnedHandle& child)
{
    static std::atomic<unsigned long> sequence{0};
    const std::wstring name = L"\\\\.\\pipe\\cinemania-worker-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(sequence.fetch_add(1));
    const DWORD access = worker_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND;
    worker.reset(CreateNamedPipeW(name.c_str(),
                                  access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_WAIT,
                                  1,
                                  65536,
                                  65536,
                                  0,
                                  nullptr));
    if (worker.get() == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        worker.reset();
        return failure(code, "create encoder pipe failed");
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    child.reset(CreateFileW(name.c_str(),
                            worker_reads ? GENERIC_WRITE : GENERIC_READ,
                            0,
                            &attributes,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr));
    if (child.get() == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        child.reset();
        return failure(code, "open encoder pipe failed");
    }
    return {};
}
} // namespace

OwnedHandle::OwnedHandle(HANDLE handle) : handle_(handle)
{
}

OwnedHandle::~OwnedHandle()
{
    reset();
}

OwnedHandle::OwnedHandle(OwnedHandle&& other) noexcept : handle_(other.release())
{
}

OwnedHandle& OwnedHandle::operator=(OwnedHandle&& other) noexcept
{
    if (this != &other)
    {
        reset(other.release());
    }
    return *this;
}

HANDLE OwnedHandle::get() const
{
    return handle_;
}

HANDLE OwnedHandle::release()
{
    HANDLE result = handle_;
    handle_ = nullptr;
    return result;
}

void OwnedHandle::reset(HANDLE handle)
{
    if (handle_ && handle_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle_);
    }
    handle_ = handle;
}

StopState::StopState() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (!event_.get())
    {
        const DWORD code = GetLastError();
        throw std::system_error(
            static_cast<int>(code), std::system_category(), "create stop event");
    }
}

void StopState::request(NativeError error, StopReason reason, StopOrigin origin)
{
    std::lock_guard lock(mutex_);
    if (!error_ && !committed_)
    {
        error_ = std::move(error);
        reason_ = reason;
        origin_ = origin;
        SetEvent(event_.get());
    }
}

void StopState::wake()
{
    SetEvent(event_.get());
}

bool StopState::stopped() const
{
    std::lock_guard lock(mutex_);
    return error_.has_value();
}

HANDLE StopState::event() const
{
    return event_.get();
}

NativeError StopState::error() const
{
    std::lock_guard lock(mutex_);
    return error_.value_or(NativeError{});
}

StopReason StopState::reason() const
{
    std::lock_guard lock(mutex_);
    return reason_;
}

StopOrigin StopState::origin() const
{
    std::lock_guard lock(mutex_);
    return origin_;
}

NativeError StopState::commit_if_running(const std::function<NativeError()>& operation)
{
    std::lock_guard lock(mutex_);
    if (error_)
    {
        return *error_;
    }
    NativeError result = operation();
    if (!result)
    {
        committed_ = true;
    }
    return result;
}

PipeChannel::PipeChannel(HANDLE handle) : handle_(handle)
{
}

void PipeChannel::close()
{
    handle_.reset();
}

HANDLE PipeChannel::get() const
{
    return handle_.get();
}

NativeError PipeChannel::read_exact(std::span<std::byte> bytes,
                                    StopState& stop,
                                    Deadline deadline,
                                    HANDLE process)
{
    return transfer(handle_.get(), bytes.data(), bytes.size(), false, stop, deadline, process);
}

NativeError PipeChannel::write_all(std::span<const std::byte> bytes,
                                   StopState& stop,
                                   Deadline deadline,
                                   HANDLE process)
{
    return transfer(handle_.get(),
                    const_cast<std::byte*>(bytes.data()),
                    bytes.size(),
                    true,
                    stop,
                    deadline,
                    process);
}

NativeError file_write_all(HANDLE file,
                           std::span<const std::byte> bytes,
                           std::uint64_t offset,
                           StopState& stop,
                           Deadline deadline)
{
    if (bytes.size() > UINT64_MAX - offset)
    {
        return failure(ERROR_ARITHMETIC_OVERFLOW, "file offset overflow");
    }
    return transfer(file,
                    const_cast<std::byte*>(bytes.data()),
                    bytes.size(),
                    true,
                    stop,
                    deadline,
                    nullptr,
                    &offset);
}

struct EncoderProcess::Impl
{
    OwnedHandle job;
    OwnedHandle job_port;
    bool job_assigned = false;
    OwnedHandle process;
    OwnedHandle stdin_pipe;
    OwnedHandle output_pipe;
    OwnedHandle drain_stop;
    std::thread drain_thread;
    std::string tail;
    std::string line;
    std::optional<std::uint64_t> frame_count;
    std::optional<std::uint64_t> last_progress_frame;
    std::optional<std::int64_t> last_progress_time_us;
    std::atomic<std::int64_t> progress_timestamp_ns{0};
    NativeError drain_error;

    void mark_progress()
    {
        const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
        progress_timestamp_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    void record_output(const char* bytes, std::size_t size)
    {
        constexpr std::size_t tail_limit = 65536;
        tail.append(bytes, size);
        if (tail.size() > tail_limit)
        {
            tail.erase(0, tail.size() - tail_limit);
        }
        for (std::size_t index = 0; index < size; ++index)
        {
            const char character = bytes[index];
            if (character == '\n' || character == '\r')
            {
                if (line.starts_with("frame="))
                {
                    const char* first = line.data() + 6;
                    const char* last = line.data() + line.size();
                    while (first != last && *first == ' ')
                    {
                        ++first;
                    }
                    std::uint64_t count = 0;
                    const auto parsed = std::from_chars(first, last, count);
                    if (parsed.ec == std::errc{} && parsed.ptr == last)
                    {
                        frame_count = count;
                        if (count > last_progress_frame.value_or(0))
                        {
                            last_progress_frame = count;
                            mark_progress();
                        }
                    }
                }
                else if (line.starts_with("out_time_us="))
                {
                    const char* first = line.data() + 12;
                    const char* last = line.data() + line.size();
                    std::int64_t media_time = 0;
                    const auto parsed = std::from_chars(first, last, media_time);
                    if (parsed.ec == std::errc{} && parsed.ptr == last && media_time >= 0 &&
                        media_time > last_progress_time_us.value_or(0))
                    {
                        last_progress_time_us = media_time;
                        mark_progress();
                    }
                }
                line.clear();
            }
            else if (line.size() < 4096)
            {
                line.push_back(character);
            }
        }
    }

    void drain()
    {
        OwnedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.get())
        {
            drain_error = failure(GetLastError(), "create encoder drain event failed");
            return;
        }
        char buffer[4096];
        for (;;)
        {
            if (WaitForSingleObject(drain_stop.get(), 0) == WAIT_OBJECT_0)
            {
                return;
            }
            OVERLAPPED operation{};
            operation.hEvent = event.get();
            ResetEvent(event.get());
            DWORD count = 0;
            if (!ReadFile(output_pipe.get(), buffer, sizeof(buffer), &count, &operation))
            {
                DWORD code = GetLastError();
                if (code != ERROR_IO_PENDING)
                {
                    if (code != ERROR_BROKEN_PIPE)
                    {
                        drain_error = failure(code, "encoder output read failed");
                    }
                    return;
                }
                HANDLE waits[] = {event.get(), drain_stop.get()};
                const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (result != WAIT_OBJECT_0)
                {
                    code = result == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED;
                    CancelIoEx(output_pipe.get(), &operation);
                    GetOverlappedResult(output_pipe.get(), &operation, &count, TRUE);
                    if (result == WAIT_FAILED)
                    {
                        drain_error = failure(code, "encoder output wait failed");
                    }
                    return;
                }
                if (!GetOverlappedResult(output_pipe.get(), &operation, &count, FALSE))
                {
                    code = GetLastError();
                    if (code != ERROR_BROKEN_PIPE)
                    {
                        drain_error = failure(code, "encoder output completion failed");
                    }
                    return;
                }
            }
            if (count == 0)
            {
                return;
            }
            record_output(buffer, count);
        }
    }

    NativeError wait_for_job_empty()
    {
        if (!job_assigned)
        {
            return {};
        }
        const Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;)
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(job.get(),
                                           JobObjectBasicAccountingInformation,
                                           &accounting,
                                           sizeof(accounting),
                                           nullptr))
            {
                return failure(GetLastError(), "query encoder job failed");
            }
            if (accounting.ActiveProcesses == 0)
            {
                job_assigned = false;
                return {};
            }
            DWORD message = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* process_id = nullptr;
            if (!GetQueuedCompletionStatus(
                    job_port.get(), &message, &key, &process_id, wait_milliseconds(deadline)))
            {
                const DWORD code = GetLastError();
                if (code != WAIT_TIMEOUT)
                {
                    return failure(code, "wait for encoder job failed");
                }
                if (!QueryInformationJobObject(job.get(),
                                               JobObjectBasicAccountingInformation,
                                               &accounting,
                                               sizeof(accounting),
                                               nullptr))
                {
                    return failure(GetLastError(), "query encoder job failed");
                }
                if (accounting.ActiveProcesses == 0)
                {
                    job_assigned = false;
                    return {};
                }
                return failure(ERROR_TIMEOUT, "wait for encoder job failed");
            }
            if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO)
            {
                job_assigned = false;
                return {};
            }
        }
    }

    NativeError cleanup()
    {
        NativeError result;
        stdin_pipe.reset();
        if (job.get() && !TerminateJobObject(job.get(), ERROR_OPERATION_ABORTED))
        {
            const DWORD code = GetLastError();
            result = failure(code, "terminate encoder job failed");
            if (process.get())
            {
                TerminateProcess(process.get(), ERROR_OPERATION_ABORTED);
            }
            job.reset();
        }
        if (job.get())
        {
            NativeError wait_error = wait_for_job_empty();
            if (!result && wait_error)
            {
                result = std::move(wait_error);
            }
        }
        if (process.get() && WaitForSingleObject(process.get(), 2000) != WAIT_OBJECT_0 && !result)
        {
            result = failure(ERROR_TIMEOUT, "wait for encoder process failed");
        }
        if (drain_thread.joinable())
        {
            if (WaitForSingleObject(drain_thread.native_handle(), 2000) != WAIT_OBJECT_0)
            {
                SetEvent(drain_stop.get());
                if (!result)
                {
                    result = failure(ERROR_TIMEOUT, "encoder output drain timed out");
                }
            }
            drain_thread.join();
        }
        output_pipe.reset();
        process.reset();
        job.reset();
        job_port.reset();
        job_assigned = false;
        drain_stop.reset();
        return result;
    }

    ~Impl()
    {
        try
        {
            static_cast<void>(cleanup());
        }
        catch (...)
        {
            if (job.get())
            {
                TerminateJobObject(job.get(), ERROR_OPERATION_ABORTED);
            }
            if (drain_stop.get())
            {
                SetEvent(drain_stop.get());
            }
            if (drain_thread.joinable())
            {
                drain_thread.join();
            }
        }
    }
};

EncoderProcess::EncoderProcess() : impl_(std::make_unique<Impl>())
{
}

EncoderProcess::~EncoderProcess() = default;

NativeError EncoderProcess::start(const std::wstring& executable,
                                  const std::vector<std::wstring>& arguments,
                                  StopState& stop)
{
    if (stop.stopped())
    {
        return stop.error();
    }
    const bool drive_absolute = executable.size() >= 3 && executable[1] == L':' &&
                                (executable[2] == L'\\' || executable[2] == L'/');
    const bool unc_absolute =
        executable.size() >= 3 && executable[0] == L'\\' && executable[1] == L'\\';
    if (impl_->process.get() || (!drive_absolute && !unc_absolute))
    {
        return failure(ERROR_INVALID_PARAMETER,
                       "encoder executable must be an absolute drive path");
    }
    // A failed earlier launch may have left partially initialized resources.
    NativeError cleanup_error = impl_->cleanup();
    if (cleanup_error)
    {
        return cleanup_error;
    }
    impl_->tail.clear();
    impl_->line.clear();
    impl_->frame_count.reset();
    impl_->last_progress_frame.reset();
    impl_->last_progress_time_us.reset();
    impl_->progress_timestamp_ns.store(0);
    impl_->drain_error = {};
    OwnedHandle child_input;
    OwnedHandle child_output;
    NativeError error = make_pipe(false, impl_->stdin_pipe, child_input);
    if (error)
    {
        return error;
    }
    error = make_pipe(true, impl_->output_pipe, child_output);
    if (error)
    {
        return error;
    }
    impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!impl_->job.get())
    {
        return failure(GetLastError(), "create encoder job failed");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            impl_->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        return failure(GetLastError(), "configure encoder job failed");
    }
    impl_->job_port.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!impl_->job_port.get())
    {
        return failure(GetLastError(), "create encoder job completion port failed");
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT completion{};
    completion.CompletionKey = impl_.get();
    completion.CompletionPort = impl_->job_port.get();
    if (!SetInformationJobObject(impl_->job.get(),
                                 JobObjectAssociateCompletionPortInformation,
                                 &completion,
                                 sizeof(completion)))
    {
        return failure(GetLastError(), "associate encoder job completion port failed");
    }
    impl_->drain_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl_->drain_stop.get())
    {
        return failure(GetLastError(), "create drain stop event failed");
    }
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
    {
        return failure(GetLastError(), "initialize process attributes failed");
    }
    std::unique_ptr<std::remove_pointer_t<PPROC_THREAD_ATTRIBUTE_LIST>, AttributeDeleter>
        attribute_guard(attributes);
    HANDLE inherited[] = {child_input.get(), child_output.get()};
    if (!UpdateProcThreadAttribute(attributes,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited,
                                   sizeof(inherited),
                                   nullptr,
                                   nullptr))
    {
        const DWORD code = GetLastError();
        return failure(code, "set process handle list failed");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input.get();
    startup.StartupInfo.hStdOutput = child_output.get();
    startup.StartupInfo.hStdError = child_output.get();
    startup.lpAttributeList = attributes;
    std::wstring command = quote_argument(executable);
    for (const auto& argument : arguments)
    {
        command += L" ";
        command += quote_argument(argument);
    }
    DWORD environment_error = ERROR_SUCCESS;
    auto environment = child_environment(environment_error);
    if (environment.empty())
    {
        return failure(environment_error, "copy encoder environment failed");
    }
    PROCESS_INFORMATION process{};
    const BOOL created =
        CreateProcessW(executable.c_str(),
                       command.data(),
                       nullptr,
                       nullptr,
                       TRUE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                           EXTENDED_STARTUPINFO_PRESENT,
                       environment.data(),
                       nullptr,
                       &startup.StartupInfo,
                       &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    attribute_guard.reset();
    child_input.reset();
    child_output.reset();
    if (!created)
    {
        return failure(create_error, "create encoder process failed");
    }
    impl_->process.reset(process.hProcess);
    OwnedHandle thread(process.hThread);
    if (!AssignProcessToJobObject(impl_->job.get(), impl_->process.get()))
    {
        const DWORD code = GetLastError();
        TerminateProcess(impl_->process.get(), code);
        return failure(code, "assign encoder job failed");
    }
    impl_->job_assigned = true;
    try
    {
        impl_->drain_thread = std::thread(
            [this, &stop]
            {
                try
                {
                    impl_->drain();
                }
                catch (...)
                {
                    // This assignment does not allocate, even if allocation caused the exception.
                    impl_->drain_error.code = ERROR_NOT_ENOUGH_MEMORY;
                }
                if (impl_->drain_error)
                {
                    try
                    {
                        stop.request({impl_->drain_error.code, {}},
                                     StopReason::failure,
                                     StopOrigin::encoder);
                    }
                    catch (...)
                    {
                        // A signaled event still interrupts pending writes if locking failed.
                        stop.wake();
                    }
                }
            });
    }
    catch (const std::system_error&)
    {
        return failure(ERROR_NOT_ENOUGH_MEMORY, "start encoder drain failed");
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    {
        return failure(GetLastError(), "resume encoder failed");
    }
    return {};
}

NativeError
EncoderProcess::write(std::span<const std::byte> bytes, StopState& stop, Deadline deadline)
{
    return transfer(impl_->stdin_pipe.get(),
                    const_cast<std::byte*>(bytes.data()),
                    bytes.size(),
                    true,
                    stop,
                    deadline,
                    impl_->process.get());
}

void EncoderProcess::close_stdin()
{
    impl_->stdin_pipe.reset();
}

NativeError EncoderProcess::close()
{
    return impl_->cleanup();
}

HANDLE EncoderProcess::process_handle() const
{
    return impl_->process.get();
}

EncoderResult EncoderProcess::finish(StopState& stop,
                                     Deadline deadline,
                                     const std::function<NativeError()>& progress_callback)
{
    EncoderResult result;
    close_stdin();
    if (!impl_->process.get())
    {
        result.error = failure(ERROR_INVALID_HANDLE, "encoder was not started");
        return result;
    }
    HANDLE waits[] = {impl_->process.get(), stop.event()};
    const auto started = std::chrono::steady_clock::now();
    const auto idle_limit = deadline > started ? deadline - started : Deadline::duration::zero();
    auto last_reported = started;
    DWORD waited = WAIT_TIMEOUT;
    for (;;)
    {
        const auto poll_deadline =
            progress_callback
                ? (std::min)(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(1))
                : deadline;
        waited = WaitForMultipleObjects(2, waits, FALSE, wait_milliseconds(poll_deadline));
        if (waited != WAIT_TIMEOUT)
        {
            break;
        }
        const auto timestamp = std::chrono::nanoseconds(impl_->progress_timestamp_ns.load());
        const Deadline progress(std::chrono::duration_cast<Deadline::duration>(timestamp));
        if (progress_callback && progress > last_reported)
        {
            last_reported = progress;
            result.error = progress_callback();
            if (result.error)
            {
                break;
            }
        }
        if (progress > started && idle_limit != Deadline::duration::zero() &&
            deadline != Deadline::max())
        {
            const auto remaining = Deadline::max() - progress;
            const Deadline next = idle_limit < remaining ? progress + idle_limit : Deadline::max();
            deadline = (std::max)(deadline, next);
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
    }
    if (waited != WAIT_OBJECT_0)
    {
        if (!result.error && waited == WAIT_OBJECT_0 + 1)
        {
            result.error = stop.error();
            if (!result.error)
            {
                result.error = failure(ERROR_OPERATION_ABORTED, "encoder wait stopped");
            }
        }
        else if (!result.error)
        {
            const DWORD code = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            result.error = failure(code, "wait for encoder failed");
        }
        if (!TerminateJobObject(impl_->job.get(), ERROR_OPERATION_ABORTED))
        {
            TerminateProcess(impl_->process.get(), ERROR_OPERATION_ABORTED);
            impl_->job.reset();
        }
        WaitForSingleObject(impl_->process.get(), 2000);
    }
    if (!GetExitCodeProcess(impl_->process.get(), &result.exit_code) && !result.error)
    {
        result.error = failure(GetLastError(), "get encoder exit code failed");
    }
    NativeError cleanup_error = impl_->cleanup();
    if (!result.error && cleanup_error)
    {
        result.error = std::move(cleanup_error);
    }
    if (!result.error && impl_->drain_error)
    {
        result.error = impl_->drain_error;
    }
    result.frame_count = impl_->frame_count;
    result.stderr_tail = std::move(impl_->tail);
    return result;
}
} // namespace cinemania::worker
