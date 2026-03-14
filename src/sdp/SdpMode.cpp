#include "SdpMode.h"

namespace sdp 
{


std::string SdpMedia::GetAttribute(const std::string& key) const
{
    for (const auto& attr : attributes)
    {
        if (attr.key == key)
            return attr.value;
    }
    return "";
}

bool SdpMedia::HasAttribute(const std::string& key) const
{
    for (const auto& attr : attributes)
    {
        if (attr.key == key)
            return true;
    }
    return false;
}

}