#include "executable_check.hpp"
#include "win32_file.hpp"

#include <vector>

// clang-format off
// Windows types must precede BCrypt declarations.
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace cinemania
{
namespace
{
constexpr std::array<unsigned char, 32> kSupportedHash = {
    0x8c, 0x53, 0x25, 0xd0, 0x4a, 0x10, 0xd5, 0xe6, 0x92, 0x1a, 0xfb, 0x99, 0x31, 0x77, 0x02, 0x79,
    0xcc, 0xa8, 0xcd, 0xfc, 0x7f, 0x66, 0x42, 0x5b, 0x63, 0x65, 0xd0, 0xa8, 0x70, 0xf5, 0x69, 0x4a};

class HashProvider
{
public:
    HashProvider() = default;
    HashProvider(const HashProvider&) = delete;
    HashProvider& operator=(const HashProvider&) = delete;

    ~HashProvider()
    {
        if (hash != nullptr)
        {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

CheckResult stream_hash(HANDLE file, BCRYPT_HASH_HANDLE hash)
{
    CheckResult result;
    std::array<unsigned char, 65536> buffer{};
    for (;;)
    {
        DWORD bytes_read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr))
        {
            result.code = GetLastError();
            result.stage = "ReadFile(executable)";
            result.domain = ErrorDomain::win32;
            return result;
        }
        if (bytes_read == 0)
        {
            break;
        }
        NTSTATUS status = BCryptHashData(hash, buffer.data(), bytes_read, 0);
        if (status < 0)
        {
            result.stage = "BCryptHashData";
            result.domain = ErrorDomain::ntstatus;
            result.code = static_cast<unsigned long>(status);
            return result;
        }
    }

    NTSTATUS status =
        BCryptFinishHash(hash, result.hash.data(), static_cast<ULONG>(result.hash.size()), 0);
    if (status < 0)
    {
        result.stage = "BCryptFinishHash";
        result.domain = ErrorDomain::ntstatus;
        result.code = static_cast<unsigned long>(status);
        return result;
    }
    result.stage = "complete";
    result.has_hash = true;
    return result;
}

CheckResult hash_file(HANDLE file)
{
    CheckResult result;
    std::vector<unsigned char> hash_object;
    HashProvider provider;
    NTSTATUS status =
        BCryptOpenAlgorithmProvider(&provider.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0)
    {
        result.stage = "BCryptOpenAlgorithmProvider";
        result.domain = ErrorDomain::ntstatus;
        result.code = static_cast<unsigned long>(status);
        return result;
    }

    DWORD object_size = 0;
    DWORD property_bytes = 0;
    status = BCryptGetProperty(provider.algorithm,
                               BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&object_size),
                               sizeof(object_size),
                               &property_bytes,
                               0);
    if (status < 0 || property_bytes != sizeof(object_size) || object_size == 0)
    {
        result.stage = "BCryptGetProperty";
        result.domain = status < 0 ? ErrorDomain::ntstatus : ErrorDomain::win32;
        result.code = status < 0 ? static_cast<unsigned long>(status) : ERROR_INVALID_DATA;
        return result;
    }

    hash_object.resize(object_size);
    status = BCryptCreateHash(
        provider.algorithm, &provider.hash, hash_object.data(), object_size, nullptr, 0, 0);
    if (status < 0)
    {
        result.stage = "BCryptCreateHash";
        result.domain = ErrorDomain::ntstatus;
        result.code = static_cast<unsigned long>(status);
        return result;
    }

    return stream_hash(file, provider.hash);
}

} // namespace

CheckResult check_executable()
{
    CheckResult result;
    std::array<wchar_t, 32768> executable_path{};
    const DWORD path_length = GetModuleFileNameW(
        nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (path_length == 0 || path_length >= executable_path.size())
    {
        result.code = path_length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        result.stage = "GetModuleFileNameW";
        result.domain = ErrorDomain::win32;
        return result;
    }

    FileHandle file(CreateFileW(executable_path.data(),
                                GENERIC_READ,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
    {
        result.code = GetLastError();
        result.stage = "CreateFileW(executable)";
        result.domain = ErrorDomain::win32;
        return result;
    }

    result = hash_file(file.get());
    if (result.has_hash)
    {
        result.outcome =
            result.hash == kSupportedHash ? CheckOutcome::compatible : CheckOutcome::unsupported;
    }

    return result;
}
} // namespace cinemania
