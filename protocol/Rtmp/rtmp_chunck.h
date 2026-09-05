#ifndef PACKETIA_PROTOCOL_RTMP_CHUNK_H_
#define PACKETIA_PROTOCOL_RTMP_CHUNK_H_

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "rtmp_message.h"

namespace protocol::rtmp {

class RtmpChunkCodec {
public:
    bool Feed(const uint8_t* data, std::size_t size,
              std::vector<RtmpMessage>& messages);
    bool Encode(const RtmpMessage& message, std::vector<uint8_t>& output);
    void SetInboundChunkSize(uint32_t size);
    void SetOutboundChunkSize(uint32_t size);
    uint32_t inbound_chunk_size() const { return inbound_chunk_size_; }
    uint32_t outbound_chunk_size() const { return outbound_chunk_size_; }
    void Reset();

private:
    struct InboundStream 
    {
        ChunkHeader header;
        uint32_t bytes_read = 0;
        std::vector<uint8_t> payload;
        bool has_previous_header = false;
    };
    bool ParseOne(std::size_t& consumed, std::vector<RtmpMessage>& messages);

    uint32_t inbound_chunk_size_ = static_cast<uint32_t>(kDefaultChunkSize);
    uint32_t outbound_chunk_size_ = static_cast<uint32_t>(kDefaultChunkSize);
    std::vector<uint8_t> pending_;
    std::unordered_map<uint32_t, InboundStream> inbound_streams_;
};

}  // namespace protocol::rtmp
#endif
