#ifndef _RTSPMESSAGE_H_
#define _RTSPMESSAGE_H_

#include <string>
#include "BufferRead.h"

class RtspRequest
{
public:
    enum class Method
    {
        OPTIONS = 0,
        DESCRIBE,
        SETUP,
        PLAY,
        PAUSE,
        TEARDOWN,
        GET_PARAMETER,
        SET_PARAMETER,
        ANNOUNCE,
        RECORD,
        REDIRECT
    };
    enum class Version
    {
        RTSP_1_0,
        RTSP_2_0
    };

    const char* GetMethodString(Method method)
    {
        switch (method)
        {
        case Method::OPTIONS: return "OPTIONS";
        case Method::DESCRIBE: return "DESCRIBE";
        case Method::SETUP: return "SETUP";
        case Method::PLAY: return "PLAY";
        case Method::PAUSE: return "PAUSE";
        case Method::TEARDOWN: return "TEARDOWN";
        case Method::GET_PARAMETER: return "GET_PARAMETER";
        case Method::SET_PARAMETER: return "SET_PARAMETER";
        case Method::ANNOUNCE: return "ANNOUNCE";
        case Method::RECORD: return "RECORD";
        case Method::REDIRECT: return "REDIRECT";
        default: return "";
        }
    }
    const char* GetVersionString(Version version)
    {
        switch (version)
        {
        case Version::RTSP_1_0: return "RTSP/1.0";
        case Version::RTSP_2_0: return "RTSP/2.0";
        default: return "";
        }
    }
    
    bool ParseRequest(BufferReader *buffer);
};

#endif