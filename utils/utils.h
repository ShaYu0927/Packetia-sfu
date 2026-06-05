#ifndef _UTILS_H_
#define _UTILS_H_

#include <string>
#include <cstdint>

namespace utils 
{

class Utils
{
public:
    /**
     * @brief Convert a string to uppercase.
     *
     * Converts all alphabetic characters in the given string to uppercase.
     * The input string is passed by value, so the original string will not be modified.
     *
     * @param value The source string to convert.
     * @return A new string with all alphabetic characters converted to uppercase.
     */
    static std::string ToUpper(std::string value);

    /**
     * @brief Read a 16-bit unsigned integer in big-endian byte order.
     *
     * @param data Pointer to the source byte buffer.
     * @return The parsed 16-bit unsigned integer.
     */
    static uint16_t ReadUint16BE(const uint8_t* data);

    /**
     * @brief Read a 32-bit unsigned integer in big-endian byte order.
     *
     * @param data Pointer to the source byte buffer.
     * @return The parsed 32-bit unsigned integer.
     */
    static uint32_t ReadUint32BE(const uint8_t* data);

    /**
     * @brief Write a 16-bit unsigned integer in big-endian byte order.
     *
     * @param data Pointer to the destination byte buffer.
     * @param value The value to write.
     */
    static void WriteUint16BE(uint8_t* data, uint16_t value);

    /**
     * @brief Write a 32-bit unsigned integer in big-endian byte order.
     *
     * @param data Pointer to the destination byte buffer.
     * @param value The value to write.
     */
    static void WriteUint32BE(uint8_t* data, uint32_t value);

    /**
     * @brief Read a 16-bit unsigned integer in little-endian byte order.
     *
     * @param data Pointer to the source byte buffer.
     * @return The parsed 16-bit unsigned integer.
     */
    static uint16_t ReadUint16LE(const uint8_t* data);

    /**
     * @brief Read a 32-bit unsigned integer in little-endian byte order.
     *
     * @param data Pointer to the source byte buffer.
     * @return The parsed 32-bit unsigned integer.
     */
    static uint32_t ReadUint32LE(const uint8_t* data);

    /**
     * @brief Write a 16-bit unsigned integer in little-endian byte order.
     *
     * @param data Pointer to the destination byte buffer.
     * @param value The value to write.
     */
    static void WriteUint16LE(uint8_t* data, uint16_t value);

    /**
     * @brief Write a 32-bit unsigned integer in little-endian byte order.
     *
     * @param data Pointer to the destination byte buffer.
     * @param value The value to write.
     */
    static void WriteUint32LE(uint8_t* data, uint32_t value);
};

}

#endif /* _UTILS_H_ */