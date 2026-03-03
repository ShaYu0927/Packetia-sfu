#include "IcePair.h"
#include <sstream>
#include <string>
namespace ice
{

std::string IceCandidate::ToString() const
{
    std::ostringstream oss;
    return oss.str();
}

std::string IceCandidate::ToSdpCandidateLine() const
{
    return "";
}

}