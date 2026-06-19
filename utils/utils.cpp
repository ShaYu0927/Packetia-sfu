#include "utils.h"
#include <algorithm>

namespace utils 
{

std::string Utils::ToUpper(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        }
    );

    return value;
}

uint8_t Utils::ReadUint8(const uint8_t* data)
{
    return data[0];
}

uint16_t Utils::ReadUint16BE(const uint8_t* data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        (static_cast<uint16_t>(data[1]))
    );
}

uint32_t Utils::ReadUint24BE(const uint8_t* data)
{
    return  (static_cast<uint32_t>(data[0]) << 16) |
            (static_cast<uint32_t>(data[1]) << 8)  |
            (static_cast<uint32_t>(data[2]));
}

uint32_t Utils::ReadUint32BE(const uint8_t* data)
{
    return  (static_cast<uint32_t>(data[0]) << 24) |
            (static_cast<uint32_t>(data[1]) << 16) |
            (static_cast<uint32_t>(data[2]) << 8)  |
            (static_cast<uint32_t>(data[3]));
}

uint64_t Utils::ReadUint64BE(const uint8_t* data)
{
    return  (static_cast<uint64_t>(data[0]) << 56) |
            (static_cast<uint64_t>(data[1]) << 48) |
            (static_cast<uint64_t>(data[2]) << 40) |
            (static_cast<uint64_t>(data[3]) << 32) |
            (static_cast<uint64_t>(data[4]) << 24) |
            (static_cast<uint64_t>(data[5]) << 16) |
            (static_cast<uint64_t>(data[6]) << 8)  |
            (static_cast<uint64_t>(data[7]));
}

int32_t Utils::ReadInt24BE(const uint8_t* data)
{
    const uint32_t value = ReadUint24BE(data);

    // 24-bit signed integer:
    // 0x000000 ~ 0x7FFFFF => 0 ~ 8388607
    // 0x800000 ~ 0xFFFFFF => -8388608 ~ -1
    if (value & 0x800000) 
    {
        return static_cast<int32_t>(value) - 0x1000000;
    }

    return static_cast<int32_t>(value);
}

void Utils::WriteUint16BE(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(value & 0xFF);
}

void Utils::WriteUint24BE(uint8_t* data, uint32_t value)
{
    value &= 0x00FFFFFF;

    data[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>(value & 0xFF);
}

void Utils::WriteUint32BE(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<uint8_t>(value & 0xFF);
}

void Utils::WriteUint64BE(uint8_t* data, uint64_t value)
{
    data[0] = static_cast<uint8_t>((value >> 56) & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
    data[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[7] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t Utils::ReadUint16LE(const uint8_t* data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[1]) << 8) |
        (static_cast<uint16_t>(data[0]))
    );
}

uint32_t Utils::ReadUint24LE(const uint8_t* data)
{
    return  (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[1]) << 8)  |
            (static_cast<uint32_t>(data[0]));
}

uint32_t Utils::ReadUint32LE(const uint8_t* data)
{
    return  (static_cast<uint32_t>(data[3]) << 24) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[1]) << 8)  |
            (static_cast<uint32_t>(data[0]));
}

uint64_t Utils::ReadUint64LE(const uint8_t* data)
{
    return  (static_cast<uint64_t>(data[7]) << 56) |
            (static_cast<uint64_t>(data[6]) << 48) |
            (static_cast<uint64_t>(data[5]) << 40) |
            (static_cast<uint64_t>(data[4]) << 32) |
            (static_cast<uint64_t>(data[3]) << 24) |
            (static_cast<uint64_t>(data[2]) << 16) |
            (static_cast<uint64_t>(data[1]) << 8)  |
            (static_cast<uint64_t>(data[0]));
}

void Utils::WriteUint16LE(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void Utils::WriteUint24LE(uint8_t* data, uint32_t value)
{
    value &= 0x00FFFFFF;

    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
}

void Utils::WriteUint32LE(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void Utils::WriteUint64LE(uint8_t* data, uint64_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[4] = static_cast<uint8_t>((value >> 32) & 0xFF);
    data[5] = static_cast<uint8_t>((value >> 40) & 0xFF);
    data[6] = static_cast<uint8_t>((value >> 48) & 0xFF);
    data[7] = static_cast<uint8_t>((value >> 56) & 0xFF);
}

std::string Utils::ReadFourCC(const uint8_t* data)
{
    return std::string(reinterpret_cast<const char*>(data), 4);
}


}