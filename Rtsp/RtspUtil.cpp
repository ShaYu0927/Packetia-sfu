#include "RtspUtil.h"
#include "logger.h"

int RtspUtil::ParseStreamId(const std::string &control)
{
    // streamid=NUMBER
    static const std::string kKey = "streamid=";

    auto pos = control.find(kKey);
    if (pos == std::string::npos) 
    {
        LOG_ERROR("ParseStreamId: no 'streamid=' in control=", control);
        return -1;
    }

    std::string num = control.substr(pos + kKey.size());
    if (num.empty()) 
    {
        LOG_ERROR("ParseStreamId: empty streamid in control=", control);
        return -1;
    }

    try {
        int idx = std::stoi(num);
        if (idx < 0) {
            LOG_ERROR("ParseStreamId: negative streamid=", idx);
            return -1;
        }
        return idx;
    }
    catch (const std::exception& e) {
        LOG_ERROR("ParseStreamId: invalid streamid=", num, " err=", e.what());
        return -1;
    }
}