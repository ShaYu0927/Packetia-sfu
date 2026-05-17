#ifndef _SIGNAL_MESSAGE_H_
#define _SIGNAL_MESSAGE_H_

#include <string>
#include "nlohmann/json.hpp"

namespace protocol 
{
enum class SignalCmd
{
    Unknown = 0,
    CreateRoom,
    JoinRoom,
    LeaveRoom,
    Publish,
    Subscribe,
    Offer,
    Answer,
    Candidate,
    Ping,
    Pong
};

struct SignalMessage
{
    SignalCmd cmd = SignalCmd::Unknown;

    std::string request_id;
    std::string room_id;
    std::string user_id;
    std::string target_user_id;

    nlohmann::json body;
};

SignalCmd StringToCmd(const std::string& cmd);
std::string CmdToString(SignalCmd cmd);
}


#endif /* _SIGNAL_MESSAGE_H_ */