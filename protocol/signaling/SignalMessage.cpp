#include "SignalMessage.h"

namespace protocol
{

SignalCmd StringToCmd(const std::string& cmd)
{
    if (cmd == "createRoom")
    {
        return SignalCmd::CreateRoom;
    }
    else if (cmd == "joinRoom")
    {
        return SignalCmd::JoinRoom;
    }
    else if (cmd == "leaveRoom")
    {
        return SignalCmd::LeaveRoom;
    }
    else if (cmd == "publish")
    {
        return SignalCmd::Publish;
    }
    else if (cmd == "subscribe")
    {
        return SignalCmd::Subscribe;
    }
    else if (cmd == "offer")
    {
        return SignalCmd::Offer;
    }
    else if (cmd == "answer")
    {
        return SignalCmd::Answer;
    }
    else if (cmd == "candidate")
    {
        return SignalCmd::Candidate;
    }
    else if (cmd == "ping")
    {
        return SignalCmd::Ping;
    }
    else if (cmd == "pong")
    {
        return SignalCmd::Pong;
    }

    return SignalCmd::Unknown;
}

std::string CmdToString(SignalCmd cmd)
{
    switch (cmd)
    {
    case SignalCmd::CreateRoom:
        return "createRoom";

    case SignalCmd::JoinRoom:
        return "joinRoom";

    case SignalCmd::LeaveRoom:
        return "leaveRoom";

    case SignalCmd::Publish:
        return "publish";

    case SignalCmd::Subscribe:
        return "subscribe";

    case SignalCmd::Offer:
        return "offer";

    case SignalCmd::Answer:
        return "answer";

    case SignalCmd::Candidate:
        return "candidate";

    case SignalCmd::Ping:
        return "ping";

    case SignalCmd::Pong:
        return "pong";

    case SignalCmd::Unknown:
    default:
        return "unknown";
    }
}

}