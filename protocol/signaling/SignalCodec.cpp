#include "SignalCodec.h"

namespace protocol 
{
SignalCmd StringToCmd(const std::string& cmd)
{
    if (cmd == "createRoom") return SignalCmd::CreateRoom;
    if (cmd == "joinRoom") return SignalCmd::JoinRoom;
    if (cmd == "leaveRoom") return SignalCmd::LeaveRoom;
    if (cmd == "publish") return SignalCmd::Publish;
    if (cmd == "subscribe") return SignalCmd::Subscribe;
    if (cmd == "offer") return SignalCmd::Offer;
    if (cmd == "answer") return SignalCmd::Answer;
    if (cmd == "candidate") return SignalCmd::Candidate;
    if (cmd == "ping") return SignalCmd::Ping;
    if (cmd == "pong") return SignalCmd::Pong;
    return SignalCmd::Unknown;
}

bool SignalCodec::Decode(const std::string& text, SignalMessage& msg)
{
    try
    {
        auto j = nlohmann::json::parse(text);

        msg.cmd = StringToCmd(j.value("cmd", ""));
        msg.request_id = j.value("requestId", "");
        msg.room_id = j.value("roomId", "");
        msg.user_id = j.value("userId", "");
        msg.target_user_id = j.value("targetUserId", "");

        if (j.contains("body"))
        {
            msg.body = j["body"];
        }

        return msg.cmd != SignalCmd::Unknown;
    }
    catch (...)
    {
        return false;
    }
}

std::string SignalCodec::EncodeResponse(const std::string& requestId, int code, const std::string& message,const nlohmann::json& data)
{
    nlohmann::json j;
    j["type"] = "response";
    j["requestId"] = requestId;
    j["code"] = code;
    j["message"] = message;
    j["data"] = data;
    return j.dump();
}

std::string SignalCodec::EncodeNotify(const std::string& event, const nlohmann::json& data)
{
    nlohmann::json j;
    j["type"] = "notify";
    j["event"] = event;
    j["data"] = data;
    return j.dump();
}
}