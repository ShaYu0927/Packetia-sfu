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
}