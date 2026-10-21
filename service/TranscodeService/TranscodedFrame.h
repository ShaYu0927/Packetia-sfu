#pragma once

#include "MediaFrame.h"
#include <string>

namespace service::transcode {

struct TranscodedFrame
{
    std::string rendition_id;
    media::EncodedFrame::ConstPtr frame;
};

} // namespace service::transcode
