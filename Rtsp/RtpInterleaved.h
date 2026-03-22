#ifndef _RTPINTERLEAVED_H_
#define _RTPINTERLEAVED_H_

#include "ShardedWorkerPool.h"



class RtpInterleaved : public IJobHandler
{

protected:
    void handle(WorkJob& job) override; 

private:

};




#endif //_RTPINTERLEAVED_H_