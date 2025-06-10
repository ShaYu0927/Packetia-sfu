#include "MediaSession.h"

std::string MediaSession::GetRtspSuffix() const
{
    return suffix_;
}



void MediaSession::PushFrame(MediaChannelId channel_id, AVFrame &frame)
{
}

std::string MediaSession::GetSdpMessage(std::string ip, std::string session_name)
{
    if(sdp_ == "")
    {
        return sdp_;
    }

    if (media_sources_.empty()) {
		return "";
	}
    return "v=0\r\ns=" + session_name + "\r\nc=IN IP4 " + ip + "\r\n";
}