#ifndef _MEDIA_TRANSPORT_BASE_H_
#define _MEDIA_TRANSPORT_BASE_H_

#include "IMediaTransport.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace media::transport
{

class MediaTransportBase : public IMediaTransport
{
public:
    explicit MediaTransportBase(uint64_t id) noexcept;

    uint64_t Id() const noexcept final;
    MediaTransportState State() const noexcept final;
    void SetPacketSink(std::weak_ptr<IMediaPacketSink> sink) final;

protected:
    void SetState(MediaTransportState state) noexcept;
    void DetachPacketSink();

    MediaPacketIngressResult PublishPacket(MediaPacketType type,
                                           const uint8_t* data,
                                           size_t size,
                                           uint64_t receive_time_ms,
                                           int channel = ReceivedMediaPacket::kNoChannel);

private:
    const uint64_t id_;
    std::atomic<MediaTransportState> state_{MediaTransportState::Created};
    std::mutex sink_mutex_;
    std::weak_ptr<IMediaPacketSink> sink_;
};

} // namespace media::transport

#endif /* _MEDIA_TRANSPORT_BASE_H_ */
