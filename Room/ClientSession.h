#ifndef _CLIENT_SESSION_H_
#define _CLIENT_SESSION_H_



class ISession 
{
public:
    virtual ~ISession() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
};




#endif