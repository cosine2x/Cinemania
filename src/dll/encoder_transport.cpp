#include "encoder_transport.hpp"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <vector>

// The linker defines this symbol for the containing module.
extern "C" IMAGE_DOS_HEADER __ImageBase; // NOLINT(readability-identifier-naming)

namespace cinemania::dll
{
namespace
{
class AttributeOwner
{
public:
    explicit AttributeOwner(LPPROC_THREAD_ATTRIBUTE_LIST attributes) : attributes_(attributes)
    {
    }

    ~AttributeOwner()
    {
        DeleteProcThreadAttributeList(attributes_);
    }

    AttributeOwner(const AttributeOwner&) = delete;
    AttributeOwner& operator=(const AttributeOwner&) = delete;

private:
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_;
};

class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value)
    {
    }

    ~Handle()
    {
        reset();
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const
    {
        return value_;
    }

    HANDLE release()
    {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr)
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }

        value_ = value;
    }

private:
    HANDLE value_;
};

std::wstring quote(const std::wstring& argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;

    for (wchar_t character : argument)
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
            result.push_back(character);
            slashes = 0;
        }
    }

    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

DWORD make_pipe(bool parent_writes, Handle& parent, Handle& child)
{
    static std::atomic<unsigned long> sequence{0};
    const std::wstring name = L"\\\\.\\pipe\\CinemaniaClient-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(sequence.fetch_add(1));
    const DWORD access = parent_writes ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;

    parent.reset(CreateNamedPipeW(name.c_str(),
                                  access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_WAIT,
                                  1,
                                  65536,
                                  65536,
                                  0,
                                  nullptr));
    if (parent.get() == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    child.reset(CreateFileW(name.c_str(),
                            parent_writes ? GENERIC_READ : GENERIC_WRITE,
                            0,
                            &security,
                            OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED,
                            nullptr));
    if (child.get() == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

DWORD executable_directory(std::wstring& directory)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(&__ImageBase), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
    }

    directory.assign(buffer.data(), length);
    const auto slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return ERROR_INVALID_NAME;
    }

    directory.resize(slash + 1);
    return ERROR_SUCCESS;
}

DWORD child_environment(std::vector<wchar_t>& result)
{
    wchar_t* environment = GetEnvironmentStringsW();
    if (!environment)
    {
        return GetLastError();
    }

    try
    {
        for (const wchar_t* entry = environment; *entry; entry += std::wcslen(entry) + 1)
        {
            if (_wcsnicmp(entry, L"TMLoader", 8) == 0 || _wcsnicmp(entry, L"MODLOADER=", 10) == 0 ||
                _wcsnicmp(entry, L"MODLOADER_TITLE=", 16) == 0)
            {
                continue;
            }

            const auto length = std::wcslen(entry) + 1;
            result.insert(result.end(), entry, entry + length);
        }

        result.push_back(L'\0');
        if (result.size() == 1)
        {
            result.push_back(L'\0');
        }
    }
    catch (...)
    {
        FreeEnvironmentStringsW(environment);
        throw;
    }

    FreeEnvironmentStringsW(environment);
    return ERROR_SUCCESS;
}
} // namespace

EncoderTransport::EncoderTransport()
    : stopped_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      data_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      control_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      status_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

EncoderTransport::~EncoderTransport()
{
    stop();

    if (process_)
    {
        CloseHandle(process_);
    }
    if (job_)
    {
        CloseHandle(job_);
    }

    if (data_)
    {
        CloseHandle(data_);
    }
    if (control_)
    {
        CloseHandle(control_);
    }
    if (status_)
    {
        CloseHandle(status_);
    }

    if (completion_port_)
    {
        CloseHandle(completion_port_);
    }
    if (stopped_)
    {
        CloseHandle(stopped_);
    }

    if (data_event_)
    {
        CloseHandle(data_event_);
    }
    if (control_event_)
    {
        CloseHandle(control_event_);
    }
    if (status_event_)
    {
        CloseHandle(status_event_);
    }
}

DWORD EncoderTransport::launch(const std::wstring& scratch, std::uint64_t session)
{
    if (!stopped_ || !data_event_ || !control_event_ || !status_event_ || process_)
    {
        return ERROR_INVALID_STATE;
    }
    if (WaitForSingleObject(stopped_, 0) == WAIT_OBJECT_0)
    {
        return ERROR_OPERATION_ABORTED;
    }

    std::wstring directory;
    DWORD error = executable_directory(directory);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }

    const std::wstring executable = directory + L"bin\\CinemaniaWorker.exe";
    const std::wstring ffmpeg = directory + L"bin\\ffmpeg.exe";

    Handle data;
    Handle data_child;
    Handle control;
    Handle control_child;
    Handle status;
    Handle status_child;
    error = make_pipe(true, data, data_child);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }
    error = make_pipe(true, control, control_child);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }
    error = make_pipe(false, status, status_child);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get())
    {
        return GetLastError();
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        return GetLastError();
    }

    Handle completion(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!completion.get())
    {
        return GetLastError();
    }

    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = job.get();
    association.CompletionPort = completion.get();
    if (!SetInformationJobObject(job.get(),
                                 JobObjectAssociateCompletionPortInformation,
                                 &association,
                                 sizeof(association)))
    {
        return GetLastError();
    }

    HANDLE inherited[] = {data_child.get(), control_child.get(), status_child.get()};
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<std::byte> storage(attribute_bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
    {
        return GetLastError();
    }
    AttributeOwner attribute_owner(attributes);
    if (!UpdateProcThreadAttribute(attributes,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited,
                                   sizeof(inherited),
                                   nullptr,
                                   nullptr))
    {
        error = GetLastError();
        return error;
    }

    std::vector<wchar_t> environment;
    error = child_environment(environment);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }

    const auto number = [](HANDLE value)
    {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(value));
    };

    std::wstring command = quote(executable) + L" --ffmpeg " + quote(ffmpeg) + L" --data " +
                           number(data_child.get()) + L" --control " + number(control_child.get()) +
                           L" --status " + number(status_child.get()) + L" --session " +
                           std::to_wstring(session) + L" --scratch " + quote(scratch);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    const BOOL created =
        CreateProcessW(executable.c_str(),
                       command.data(),
                       nullptr,
                       nullptr,
                       TRUE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                           EXTENDED_STARTUPINFO_PRESENT,
                       environment.data(),
                       directory.c_str(),
                       &startup.StartupInfo,
                       &info);
    error = created ? ERROR_SUCCESS : GetLastError();
    if (!created)
    {
        return error;
    }

    Handle process(info.hProcess);
    Handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get()))
    {
        error = GetLastError();
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        return error;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    {
        error = GetLastError();
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        return error;
    }

    data_ = data.release();
    control_ = control.release();
    status_ = status.release();
    process_ = process.release();
    completion_port_ = completion.release();
    job_ = job.release();

    if (WaitForSingleObject(stopped_, 0) == WAIT_OBJECT_0)
    {
        TerminateJobObject(job_.load(), 1);
        return ERROR_OPERATION_ABORTED;
    }
    return ERROR_SUCCESS;
}

DWORD EncoderTransport::transfer(
    HANDLE pipe, void* bytes, std::size_t size, bool write, DWORD timeout)
{
    if (!pipe || !process_.load())
    {
        return ERROR_INVALID_HANDLE;
    }

    HANDLE event = pipe == data_.load()
                       ? data_event_
                       : (pipe == control_.load() ? control_event_ : status_event_);
    auto* cursor = static_cast<std::byte*>(bytes);
    const ULONGLONG deadline = GetTickCount64() + timeout;

    while (size != 0)
    {
        if (WaitForSingleObject(stopped_, 0) == WAIT_OBJECT_0)
        {
            return ERROR_OPERATION_ABORTED;
        }
        if (GetTickCount64() >= deadline)
        {
            return ERROR_TIMEOUT;
        }

        OVERLAPPED operation{};
        operation.hEvent = event;
        ResetEvent(event);
        DWORD count = 0;
        const DWORD amount =
            static_cast<DWORD>((std::min)(size, static_cast<std::size_t>(MAXDWORD)));
        const BOOL immediate = write ? WriteFile(pipe, cursor, amount, &count, &operation)
                                     : ReadFile(pipe, cursor, amount, &count, &operation);
        if (!immediate)
        {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                return error;
            }

            HANDLE waits[] = {event, stopped_, process_.load()};
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            const DWORD result = WaitForMultipleObjects(3, waits, FALSE, remaining);
            if (result != WAIT_OBJECT_0)
            {
                error = result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
                if (result == WAIT_FAILED)
                {
                    error = GetLastError();
                }
                if (result == WAIT_OBJECT_0 + 2)
                {
                    error = ERROR_PROCESS_ABORTED;
                }
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &count, TRUE);
                return error;
            }

            if (!GetOverlappedResult(pipe, &operation, &count, FALSE))
            {
                return GetLastError();
            }
        }
        if (count == 0)
        {
            return ERROR_BROKEN_PIPE;
        }

        cursor += count;
        size -= count;
    }

    return ERROR_SUCCESS;
}

DWORD EncoderTransport::write_data(std::span<const std::byte> bytes, DWORD timeout)
{
    return transfer(data_, const_cast<std::byte*>(bytes.data()), bytes.size(), true, timeout);
}

DWORD EncoderTransport::write_control(std::span<const std::byte> bytes, DWORD timeout)
{
    return transfer(control_, const_cast<std::byte*>(bytes.data()), bytes.size(), true, timeout);
}

DWORD EncoderTransport::read_status(std::span<std::byte> bytes, DWORD timeout)
{
    return transfer(status_, bytes.data(), bytes.size(), false, timeout);
}

void EncoderTransport::cancel_if_unlaunched()
{
    if (!process_.load())
    {
        SetEvent(stopped_);
    }
}

void EncoderTransport::stop()
{
    if (stopped_)
    {
        SetEvent(stopped_);
    }
    if (job_)
    {
        TerminateJobObject(job_, 1);
    }
}

DWORD EncoderTransport::wait(DWORD timeout, DWORD& exit_code) const
{
    const DWORD result = WaitForSingleObject(process_, timeout);
    if (result == WAIT_TIMEOUT)
    {
        return ERROR_TIMEOUT;
    }
    if (result != WAIT_OBJECT_0)
    {
        return GetLastError();
    }
    return GetExitCodeProcess(process_, &exit_code) ? ERROR_SUCCESS : GetLastError();
}

HANDLE EncoderTransport::stopped_event() const
{
    return stopped_;
}

bool EncoderTransport::job_empty() const
{
    HANDLE job = job_.load();
    if (!job)
    {
        return true;
    }

    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;)
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
        if (!QueryInformationJobObject(job,
                                       JobObjectBasicAccountingInformation,
                                       &information,
                                       sizeof(information),
                                       nullptr))
        {
            return false;
        }
        if (information.ActiveProcesses == 0)
        {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return false;
        }

        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED value = nullptr;
        if (!GetQueuedCompletionStatus(
                completion_port_, &message, &key, &value, static_cast<DWORD>(deadline - now)))
        {
            return false;
        }
    }
}
} // namespace cinemania::dll
