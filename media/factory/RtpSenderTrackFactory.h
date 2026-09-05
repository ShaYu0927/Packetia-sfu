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
        // 同一订阅者下行 Transport 创建音频、视频 Track 时，调用方必须
        // 传入同一个 allocator；Factory 只注入依赖，不能在这里按 Track 新建。
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
