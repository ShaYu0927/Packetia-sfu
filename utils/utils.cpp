#include "utils.h"

namespace utils 
{
    std::string Utils::ToUpper(std::string value)
    {
        for (char& c : value)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return value;
    }

uint16_t Utils::ReadUint16BE(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) |
           (static_cast<uint16_t>(data[1]));
}

uint32_t Utils::ReadUint32BE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8)  |
           (static_cast<uint32_t>(data[3]));
}

void Utils::WriteUint16BE(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(value & 0xFF);
}

void Utils::WriteUint32BE(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t Utils::ReadUint16LE(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[1]) << 8) |
           (static_cast<uint16_t>(data[0]));
}

uint32_t Utils::ReadUint32LE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[3]) << 24) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[1]) << 8)  |
           (static_cast<uint32_t>(data[0]));
}

void Utils::WriteUint16LE(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void Utils::WriteUint32LE(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}
}