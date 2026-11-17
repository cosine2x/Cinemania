#include "protocol.hpp"

namespace cinemania::protocol
{
namespace
{
bool read_integer(std::span<const std::byte> bytes,
                  std::size_t& offset,
                  std::size_t count,
                  std::uint64_t& value)
{
    if (offset > bytes.size() || count > bytes.size() - offset)
    {
        return false;
    }

    value = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                 << (index * 8);
    }
    offset += count;
    return true;
}

void append_integer(std::vector<std::byte>& bytes, std::uint64_t value, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        bytes.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xff));
    }
}
} // namespace

bool read_u32(std::span<const std::byte> bytes, std::size_t& offset, std::uint32_t& value)
{
    std::uint64_t decoded = 0;
    if (!read_integer(bytes, offset, 4, decoded))
    {
        return false;
    }
    value = static_cast<std::uint32_t>(decoded);
    return true;
}

bool read_u64(std::span<const std::byte> bytes, std::size_t& offset, std::uint64_t& value)
{
    return read_integer(bytes, offset, 8, value);
}

bool decode_header(std::span<const std::byte> bytes, Header& header)
{
    if (bytes.size() != kHeaderBytes)
    {
        return false;
    }
    std::size_t offset = 0;
    std::uint64_t magic = 0;
    std::uint64_t version = 0;
    std::uint64_t type = 0;
    std::uint64_t reserved = 0;
    if (!read_integer(bytes, offset, 4, magic) || !read_integer(bytes, offset, 2, version) ||
        !read_integer(bytes, offset, 2, type) || !read_u64(bytes, offset, header.session) ||
        !read_u64(bytes, offset, header.sequence) ||
        !read_u32(bytes, offset, header.payload_bytes) || !read_integer(bytes, offset, 4, reserved))
    {
        return false;
    }
    header.type = static_cast<MessageType>(type);
    return magic == kMagic && version == kVersion && reserved == 0 && header.session != 0 &&
           header.payload_bytes <= kMaxPayloadBytes;
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value)
{
    append_integer(bytes, value, 2);
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    append_integer(bytes, value, 4);
}

void append_u64(std::vector<std::byte>& bytes, std::uint64_t value)
{
    append_integer(bytes, value, 8);
}

std::vector<std::byte> encode_header(const Header& header)
{
    std::vector<std::byte> bytes;
    bytes.reserve(kHeaderBytes);
    append_u32(bytes, kMagic);
    append_u16(bytes, kVersion);
    append_u16(bytes, static_cast<std::uint16_t>(header.type));
    append_u64(bytes, header.session);
    append_u64(bytes, header.sequence);
    append_u32(bytes, header.payload_bytes);
    append_u32(bytes, 0);
    return bytes;
}
} // namespace cinemania::protocol
