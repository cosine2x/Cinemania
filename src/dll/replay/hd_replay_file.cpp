#include "hd_replay_file.hpp"

#include "game/game_abi.hpp"
#include "win32_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

#include <objbase.h>

namespace cinemania::hd_replay
{
namespace
{
constexpr std::uint32_t kReplayClass = 0x03093000;
constexpr std::uint32_t kRemappedReplayClass = 0x2407e000;
constexpr std::uint32_t kSamplesChunk = 0x0303f005;
constexpr std::uint32_t kVehicleStateVersion = 9;
constexpr std::uint32_t kMaxBodyBytes = 64 * 1024 * 1024;
constexpr std::wstring_view kExtension = L".Replay.Gbx";

// The pinned game contains the x86 LZO 1X and zlib 1.2.3 codecs used by its GBX archives.
constexpr std::size_t kLzoDecompressRva = 0x50fc00;
constexpr std::size_t kLzoCompressRva = 0x50faf0;
constexpr std::size_t kDeflateInitRva = 0x5175e0;
constexpr std::size_t kInflateInitRva = 0x517740;
constexpr std::size_t kDeflateRva = 0x515df0;
constexpr std::size_t kInflateRva = 0x517870;
constexpr std::size_t kDeflateEndRva = 0x5165d0;
constexpr std::size_t kInflateEndRva = 0x518e60;

using game_abi::function_at;
using Lzo = int(__cdecl*)(const void*, std::uint32_t, void*, std::uint32_t*, void*);

struct ZStream
{
    const void* input = nullptr;
    std::uint32_t input_available = 0;
    std::uint32_t input_used = 0;
    void* output = nullptr;
    std::uint32_t output_available = 0;
    std::uint32_t output_used = 0;
    void* message = nullptr;
    void* state = nullptr;
    void* allocate = nullptr;
    void* release = nullptr;
    void* user = nullptr;
    int type = 0;
    std::uint32_t adler = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(ZStream) == 0x38);

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t position)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + position, sizeof(value));
    return value;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

enum class ZlibOperation
{
    deflate,
    inflate
};

bool zlib(unsigned char* game,
          ZlibOperation operation,
          std::span<const std::uint8_t> input,
          std::vector<std::uint8_t>& output)
{
    using Init = int(__cdecl*)(ZStream*, const char*, int);
    using DeflateInit = int(__cdecl*)(ZStream*, int, const char*, int);
    using Step = int(__cdecl*)(ZStream*, int);
    using End = int(__cdecl*)(ZStream*);
    ZStream stream{};
    const bool compress = operation == ZlibOperation::deflate;
    const int initialized =
        compress ? function_at<DeflateInit>(game, kDeflateInitRva)(&stream, 6, "1.2.3", 0x38)
                 : function_at<Init>(game, kInflateInitRva)(&stream, "1.2.3", 0x38);
    if (initialized != 0)
    {
        return false;
    }
    stream.input = input.data();
    stream.input_available = static_cast<std::uint32_t>(input.size());
    stream.output = output.data();
    stream.output_available = static_cast<std::uint32_t>(output.size());
    const auto step = function_at<Step>(game, compress ? kDeflateRva : kInflateRva);
    constexpr int finish = 4;
    const int result = step(&stream, finish);
    const int closed = function_at<End>(game, compress ? kDeflateEndRva : kInflateEndRva)(&stream);
    if (result != 1 || closed != 0 || stream.input_used != input.size() ||
        (!compress && stream.output_used != output.size()))
    {
        return false;
    }
    output.resize(stream.output_used);
    return true;
}

bool read_file(const std::wstring& path, std::vector<std::uint8_t>& data)
{
    const FileHandle file(CreateFileW(path.c_str(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr));
    LARGE_INTEGER size{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.get(), &size) ||
        size.QuadPart < 25 || size.QuadPart > kMaxBodyBytes)
    {
        return false;
    }
    data.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    return ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
           read == data.size();
}

bool decode_body(unsigned char* game, SourceFile& source)
{
    const auto& file = source.file;
    if (std::memcmp(file.data(), "GBX\x06\0BU", 7) != 0 || (file[7] != 'C' && file[7] != 'U') ||
        (file[8] != 'R' && file[8] != 'E'))
    {
        return false;
    }
    const std::uint32_t class_id = read_u32(file, 9);
    const std::size_t header_size = read_u32(file, 13);
    if ((class_id != kReplayClass && class_id != kRemappedReplayClass) ||
        header_size > file.size() - 25 || read_u32(file, 21 + header_size) != 0)
    {
        return false;
    }

    const std::size_t start = 25 + header_size;
    source.body_start = start;
    source.compressed = file[7] == 'C';
    if (!source.compressed)
    {
        source.body.assign(file.begin() + start, file.end());
        return !source.body.empty();
    }
    if (file.size() - start <= 8)
    {
        return false;
    }
    const std::uint32_t decoded_size = read_u32(file, start);
    const std::uint32_t compressed_size = read_u32(file, start + 4);
    if (decoded_size == 0 || decoded_size > kMaxBodyBytes ||
        compressed_size != file.size() - start - 8)
    {
        return false;
    }
    source.body.resize(decoded_size);
    std::uint32_t written = decoded_size;
    const auto decompress = function_at<Lzo>(game, kLzoDecompressRva);
    return decompress(
               file.data() + start + 8, compressed_size, source.body.data(), &written, nullptr) ==
               0 &&
           written == decoded_size;
}

bool matches_layout(std::span<const std::uint8_t> raw, const SampleLayout& layout)
{
    if (read_u32(raw, 0) != layout.vehicle_class || read_u32(raw, 4) != 1 ||
        read_u32(raw, 12) != layout.period_ms || read_u32(raw, 16) != layout.version)
    {
        return false;
    }
    const std::uint32_t buffer_size = read_u32(raw, 20);
    if (buffer_size == 0 || buffer_size > raw.size() - 36)
    {
        return false;
    }
    const std::size_t count_at = 24 + buffer_size;
    if (read_u32(raw, count_at) != layout.count || read_u32(raw, count_at + 4) != 0)
    {
        return false;
    }
    const std::uint32_t stride = read_u32(raw, count_at + 8);
    const std::size_t remaining = raw.size() - count_at - 12;
    if (stride != std::numeric_limits<std::uint32_t>::max())
    {
        return remaining == 0 &&
               static_cast<std::uint64_t>(stride) * (layout.count - 1) < buffer_size;
    }
    if (remaining != static_cast<std::size_t>(layout.count - 1) * 4)
    {
        return false;
    }
    std::uint64_t end = 0;
    for (std::size_t i = count_at + 12; i < raw.size(); i += 4)
    {
        end += read_u32(raw, i);
    }
    return end < buffer_size;
}

bool find_sample_chunk(unsigned char* game, SourceFile& source, const SampleLayout& layout)
{
    const auto& body = source.body;
    std::size_t candidates = 0;
    for (std::size_t i = 0; i + 14 <= body.size(); ++i)
    {
        if (body[i] != 5 || read_u32(body, i) != kSamplesChunk)
        {
            continue;
        }
        const std::uint32_t decoded = read_u32(body, i + 4);
        const std::uint32_t compressed = read_u32(body, i + 8);
        if (decoded < 37 || decoded > 2 * kMaxStateBytes || compressed < 2 ||
            compressed > body.size() - i - 12 || (body[i + 12] & 0x0f) != 8)
        {
            continue;
        }
        if (++candidates > 32)
        {
            return false;
        }
        std::vector<std::uint8_t> raw(decoded);
        if (!zlib(game, ZlibOperation::inflate, std::span(body).subspan(i + 12, compressed), raw) ||
            !matches_layout(raw, layout))
        {
            continue;
        }
        if (!source.sample_header.empty())
        {
            return false;
        }
        source.sample_header.assign(raw.begin(), raw.begin() + 12);
        source.sample_start = i;
        source.sample_end = i + 12 + compressed;
        i = source.sample_end - 1;
    }
    return !source.sample_header.empty();
}

std::vector<std::uint8_t> encode_samples(const SourceFile& source,
                                         std::span<const std::uint8_t> states,
                                         std::span<const std::uint32_t> offsets)
{
    if (source.target_samples < 2 || offsets.size() < source.target_samples ||
        offsets.front() != 0 || !std::is_sorted(offsets.begin(), offsets.end()) ||
        offsets.back() >= states.size())
    {
        return {};
    }
    const std::size_t used =
        offsets.size() == source.target_samples ? states.size() : offsets[source.target_samples];
    offsets = offsets.first(source.target_samples);
    // The native recorder can repeat an earlier offset when a tick was skipped.
    if (used <= offsets.back() ||
        std::adjacent_find(offsets.begin(), offsets.end()) != offsets.end())
    {
        return {};
    }
    const std::uint32_t stride = offsets[1];
    bool fixed_size = true;
    for (std::size_t i = 2; i < offsets.size(); ++i)
    {
        if (offsets[i] - offsets[i - 1] != stride)
        {
            fixed_size = false;
            break;
        }
    }
    auto raw = source.sample_header;
    append_u32(raw, kSamplePeriodMs);
    append_u32(raw, kVehicleStateVersion);
    append_u32(raw, static_cast<std::uint32_t>(used));
    raw.insert(raw.end(), states.begin(), states.begin() + used);
    append_u32(raw, static_cast<std::uint32_t>(offsets.size()));
    append_u32(raw, 0);
    append_u32(raw, fixed_size ? stride : std::numeric_limits<std::uint32_t>::max());
    if (!fixed_size)
    {
        for (std::size_t i = 1; i < offsets.size(); ++i)
        {
            append_u32(raw, offsets[i] - offsets[i - 1]);
        }
    }
    return raw;
}

bool publish(const std::wstring& destination, std::span<const std::uint8_t> bytes)
{
    GUID guid{};
    std::array<wchar_t, 40> suffix{};
    if (FAILED(CoCreateGuid(&guid)) ||
        StringFromGUID2(guid, suffix.data(), static_cast<int>(suffix.size())) == 0)
    {
        return false;
    }
    const std::wstring temporary = destination + suffix.data() + L".tmp";
    bool written = false;
    {
        const FileHandle file(CreateFileW(temporary.c_str(),
                                          GENERIC_WRITE,
                                          0,
                                          nullptr,
                                          CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD size = 0;
        written =
            WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &size, nullptr) &&
            size == bytes.size() && FlushFileBuffers(file.get());
    }
    if (written && MoveFileExW(temporary.c_str(),
                               destination.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return true;
    }
    DeleteFileW(temporary.c_str());
    return false;
}
} // namespace

const wchar_t* open_source(unsigned char* game,
                           const std::wstring& path,
                           const SampleLayout& layout,
                           SourceFile& source)
{
    if (path.size() <= kExtension.size() ||
        _wcsicmp(path.c_str() + path.size() - kExtension.size(), kExtension.data()) != 0)
    {
        return L"Select a replay stored as a local .Replay.Gbx file.";
    }
    source.destination = path.substr(0, path.size() - kExtension.size()) + L"_HD.Replay.Gbx";
    if (layout.version != kVehicleStateVersion || layout.period_ms < kSamplePeriodMs ||
        layout.count < 2)
    {
        return L"This replay uses an unsupported car sample format.";
    }
    if (!read_file(path, source.file) || !decode_body(game, source) ||
        !find_sample_chunk(game, source, layout))
    {
        return L"Could not locate the selected car's sample data in the source replay.";
    }
    return nullptr;
}

const wchar_t* save_replay(unsigned char* game,
                           const SourceFile& source,
                           std::span<const std::uint8_t> states,
                           std::span<const std::uint32_t> offsets)
{
    const auto raw = encode_samples(source, states, offsets);
    if (raw.empty())
    {
        return L"The game did not capture a complete 10 ms sample stream.";
    }
    std::vector<std::uint8_t> sample(raw.size() + raw.size() / 16 + 64);
    if (!zlib(game, ZlibOperation::deflate, raw, sample))
    {
        return L"Could not compress the captured car samples.";
    }
    std::vector<std::uint8_t> body(source.body.begin(), source.body.begin() + source.sample_start);
    append_u32(body, kSamplesChunk);
    append_u32(body, static_cast<std::uint32_t>(raw.size()));
    append_u32(body, static_cast<std::uint32_t>(sample.size()));
    body.insert(body.end(), sample.begin(), sample.end());
    body.insert(body.end(), source.body.begin() + source.sample_end, source.body.end());

    std::vector<std::uint8_t> output(source.file.begin(), source.file.begin() + source.body_start);
    if (source.compressed)
    {
        std::vector<std::uint8_t> compressed(body.size() + body.size() / 16 + 67);
        std::vector<std::uint32_t> work(0x10000 / sizeof(std::uint32_t));
        std::uint32_t size = static_cast<std::uint32_t>(compressed.size());
        const auto compress = function_at<Lzo>(game, kLzoCompressRva);
        if (compress(body.data(),
                     static_cast<std::uint32_t>(body.size()),
                     compressed.data(),
                     &size,
                     work.data()) != 0 ||
            size > compressed.size())
        {
            return L"Could not compress the HD replay.";
        }
        append_u32(output, static_cast<std::uint32_t>(body.size()));
        append_u32(output, size);
        output.insert(output.end(), compressed.begin(), compressed.begin() + size);
    }
    else
    {
        output.insert(output.end(), body.begin(), body.end());
    }
    return publish(source.destination, output) ? nullptr
                                               : L"Could not save the HD replay beside the source.";
}
} // namespace cinemania::hd_replay
