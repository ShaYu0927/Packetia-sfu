#ifndef _TRANSPORT_FEEDBACK_ADAPTER_H_
#define _TRANSPORT_FEEDBACK_ADAPTER_H_

#include "NetworkStats.h"
#include "PacketHistory.h"
#include "RtcpContext.h"

namespace media
{

class TransportFeedbackAdapter
{
public:
    TransportFeedback Build(const rtcpx::TransportFeedbackReport& report,
                            const PacketHistory& history,
                            uint64_t feedback_time_ms) const;
};

}

#endif /* _TRANSPORT_FEEDBACK_ADAPTER_H_ */