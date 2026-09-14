#ifndef _RTP_SENDER_TRACK_FACTORY_H_
#define _RTP_SENDER_TRACK_FACTORY_H_

#include "RtpSenderTrack.h"
#include "IMediaTransport.h"
#include <utility>

class RtpSenderTrackFactory
{
public:
    static std::shared_ptr<rtsp::RtpSenderTrack> Create(
        const TrackInfo& source_info,
        std::shared_ptr<IMediaTransport> transport,
        std::shared_ptr<media::TransportSequenceAllocator> transport_sequence_allocator = nullptr,
        uint8_t transport_cc_extension_id = 0)
    {
        rtsp::RtpSenderTrackConfig config;
        config.local_ssrc = GenerateSsrc();
        config.rewrite_payload_type = true;
        config.payload_type = source_info.payload_type;
        config.sample_rate = source_info.clock_rate > 0 ? source_info.clock_rate : 90000;
        config.rtp_cache_size = 512;
        // 支持发送侧控制的 Transport 自动提供共享 allocator。下面的参数
        // 用于自定义 Transport；此时调用方必须为同一路径传入同一个实例。
        // extension ID 必须来自下游协商，不能使用上游 source_info 的 ID。
        config.transport_sequence_allocator = std::move(transport_sequence_allocator);
        config.transport_cc_extension_id = transport_cc_extension_id;

        return std::make_shared<rtsp::RtpSenderTrack>(config, std::move(transport));
    }

private:
    static uint32_t GenerateSsrc()
    {
        static std::atomic<uint32_t> ssrc{100000};
        return ++ssrc;
    }
};

#endif /* _RTP_SENDER_TRACK_FACTORY_H_ */
