#include "rtmp_chunck.h"

namespace protocol::rtmp {

bool RtmpChunkCodec::Feed(const uint8_t* data, std::size_t size,
                          std::vector<RtmpMessage>& messages) {
    if (!data && size) return false;
    if (size) pending_.insert(pending_.end(), data, data + size);
    std::size_t consumed = 0;
    while (ParseOne(consumed, messages) && consumed) {
        pending_.erase(pending_.begin(), pending_.begin() + consumed);
        consumed = 0;
    }
    return true;
}

bool RtmpChunkCodec::Encode(const RtmpMessage& message,
                            std::vector<uint8_t>& output) {
    (void)message; (void)output;
    // TODO: write fmt=0 header and fmt=3 continuation chunks.
    return false;
}

void RtmpChunkCodec::SetInboundChunkSize(uint32_t size) {
    if (size && !(size & 0x80000000U)) inbound_chunk_size_ = size;
}
void RtmpChunkCodec::SetOutboundChunkSize(uint32_t size) {
    if (size && !(size & 0x80000000U)) outbound_chunk_size_ = size;
}
void RtmpChunkCodec::Reset() {
    pending_.clear(); inbound_streams_.clear();
    inbound_chunk_size_ = outbound_chunk_size_ =
        static_cast<uint32_t>(kDefaultChunkSize);
}
bool RtmpChunkCodec::ParseOne(std::size_t& consumed,
                              std::vector<RtmpMessage>& messages) {
    (void)messages; consumed = 0;
    // TODO: basic header -> message header -> extended timestamp -> payload.
    return false;
}

}  // namespace protocol::rtmp
