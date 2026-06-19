#ifndef _UTILS_H_
#define _UTILS_H_

#include <string>
#include <cstdint>

namespace utils 
{

class Utils
{
public:
    static std::string ToUpper(std::string value);

    static uint8_t ReadUint8(const uint8_t* data);

    static uint16_t ReadUint16BE(const uint8_t* data);
    static uint32_t ReadUint24BE(const uint8_t* data);
    static uint32_t ReadUint32BE(const uint8_t* data);
    static uint64_t ReadUint64BE(const uint8_t* data);
    static int32_t ReadInt24BE(const uint8_t* data);

    static void WriteUint16BE(uint8_t* data, uint16_t value);
    static void WriteUint24BE(uint8_t* data, uint32_t value);
    static void WriteUint32BE(uint8_t* data, uint32_t value);
    static void WriteUint64BE(uint8_t* data, uint64_t value);

    static uint16_t ReadUint16LE(const uint8_t* data);
    static uint32_t ReadUint24LE(const uint8_t* data);
    static uint32_t ReadUint32LE(const uint8_t* data);
    static uint64_t ReadUint64LE(const uint8_t* data);

    static void WriteUint16LE(uint8_t* data, uint16_t value);
    static void WriteUint24LE(uint8_t* data, uint32_t value);
    static void WriteUint32LE(uint8_t* data, uint32_t value);
    static void WriteUint64LE(uint8_t* data, uint64_t value);

    static std::string ReadFourCC(const uint8_t* data);
};

}

#endif /* _UTILS_H_ */