#ifndef _PARTICIPANT_H_
#define _PARTICIPANT_H_

#include "ClientSession.h"

typedef struct Participant 
{
    std::string id;                 // userId / sessionId
    RClientSession::Ptr egress;     // 下行发送会话（给这个人发）
}Participant;


#endif // _PARTICIPANT_H_