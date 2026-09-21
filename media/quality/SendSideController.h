#ifndef PACKETIA_MEDIA_QUALITY_SENDSIDECONTROLLER_H_
#define PACKETIA_MEDIA_QUALITY_SENDSIDECONTROLLER_H_

#include "GoogCcNetworkController.h"
#include "PacketHistory.h"
#include "TransportFeedbackAdapter.h"
#include "TransportSequenceAllocator.h"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace media
{
// One instance per downstream transport. Serializes packet history and GCC
// across audio/video workers and the transport's RTCP receive thread.
class SendSideController
{
public:
    using UpdateCallback = std::function<void(const NetworkControlUpdate&)>;

    explicit SendSideController(const NetworkControllerConfig& config = {});
    std::shared_ptr<TransportSequenceAllocator> SequenceAllocator() const;
    void OnPacketSent(const PacketSendInfo& packet);
    void RegisterSender(uint32_t ssrc, uint32_t clock_rate);
    void UnregisterSender(uint32_t ssrc);
    bool OnRtcpPacket(const uint8_t* data, size_t size, uint64_t receive_time_ms);
    void OnReceiverFeedback(const WeakNetFeedback& feedback);
    void SetNetworkAvailable(bool available);
    void SetUpdateCallback(UpdateCallback callback);
    NetworkControlUpdate GetNetworkState() const;

private:
    bool IsTwccRecent(uint64_t now_ms) const
    {
        if (!has_twcc_)
            return false;

        if (now_ms < last_valid_twcc_receive_time_ms_)
            return true;

        return now_ms - last_valid_twcc_receive_time_ms_ < 1000;
    }

private:
    mutable std::mutex mutex_;
    NetworkControllerConfig config_;
    GoogCcNetworkController controller_;
    std::shared_ptr<TransportSequenceAllocator> sequence_allocator_;
    PacketHistory history_;
    TransportFeedbackAdapter adapter_;
    bool available_;
    bool has_twcc_ = false;
    uint64_t last_valid_twcc_receive_time_ms_ = 0;
    UpdateCallback update_callback_;
    std::unordered_map<uint32_t, uint32_t> sender_clock_rates_;
};
}

#endif // PACKETIA_MEDIA_QUALITY_SENDSIDECONTROLLER_H_
