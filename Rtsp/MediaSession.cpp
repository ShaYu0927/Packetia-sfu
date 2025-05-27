#include "MediaSession.h"

std::string MediaSession::GetSdpMessage(std::string ip, std::string session_name ="")
{
    if(sdp_ == "")
    {
        return sdp_;
    }

    if (media_sources_.empty()) {
		return "";
	}
}