#ifndef _UTILS_H_
#define _UTILS_H_

#include <string>


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
};

}

#endif /* _UTILS_H_ */