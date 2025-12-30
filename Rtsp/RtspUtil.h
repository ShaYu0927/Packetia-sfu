#ifndef _RTSP_UTIL_H_
#define _RTSP_UTIL_H_

#include <string>

class RtspUtil
{
public:
    static int ParseStreamId(const std::string& control);
};


#endif