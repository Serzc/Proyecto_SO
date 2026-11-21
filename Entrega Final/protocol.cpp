// Codificación de encabezados y cargas en orden de red.
#include "dsm_internal.h"

namespace dsm {

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

bool readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    value = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
            static_cast<std::uint32_t>(bytes[offset + 3]);
    return true;
}

std::array<std::uint8_t, MESSAGE_HEADER_SIZE> encodeHeader(const MsgHeader& header) {
    std::array<std::uint8_t, MESSAGE_HEADER_SIZE> bytes{};
    bytes[0] = header.type;
    bytes[1] = header.sender;
    bytes[2] = header.status;
    bytes[3] = header.flags;
    const auto writeU32 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
        bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
    };
    writeU32(4, header.seq);
    writeU32(8, header.page);
    writeU32(12, header.payload_len);
    return bytes;
}

bool decodeHeader(const std::array<std::uint8_t, MESSAGE_HEADER_SIZE>& bytes,
                  MsgHeader& header) {
    header.type = bytes[0];
    header.sender = bytes[1];
    header.status = bytes[2];
    header.flags = bytes[3];
    const auto readField = [&bytes](std::size_t offset, std::uint32_t& value) {
        value = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
                static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    readField(4, header.seq);
    readField(8, header.page);
    readField(12, header.payload_len);
    return true;
}


}  // namespace dsm
