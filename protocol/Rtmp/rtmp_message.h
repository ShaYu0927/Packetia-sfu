#ifndef PACKETIA_PROTOCOL_RTMP_MESSAGE_H_
#define PACKETIA_PROTOCOL_RTMP_MESSAGE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace protocol::rtmp 
{

constexpr std::size_t kHandshakeSize = 1536;
constexpr std::size_t kDefaultChunkSize = 128;
constexpr uint8_t kPlainHandshakeVersion = 3;

enum class MessageType : uint8_t 
{
    kSetChunkSize = 1, kAbort = 2, kAcknowledgement = 3,
    kUserControl = 4, kWindowAcknowledgementSize = 5,
    kSetPeerBandwidth = 6, kAudio = 8, kVideo = 9, kDataAmf3 = 15,
    kSharedObjectAmf3 = 16, kCommandAmf3 = 17, kDataAmf0 = 18,
    kSharedObjectAmf0 = 19, kCommandAmf0 = 20, kAggregate = 22,
};

enum class UserControlEvent : uint16_t 
{
    kStreamBegin = 0, kStreamEof = 1, kStreamDry = 2,
    kSetBufferLength = 3, kStreamIsRecorded = 4,
    kPingRequest = 6, kPingResponse = 7,
};

struct RtmpMessage 
{
    uint32_t chunk_stream_id = 0;
    uint32_t timestamp = 0;
    uint32_t message_stream_id = 0;
    MessageType type = MessageType::kCommandAmf0;
    std::vector<uint8_t> payload;
};

struct ChunkHeader 
{
    uint8_t format = 0;
    uint32_t chunk_stream_id = 0;
    uint32_t timestamp = 0;
    uint32_t message_length = 0;
    MessageType message_type = MessageType::kCommandAmf0;
    uint32_t message_stream_id = 0;
    bool has_extended_timestamp = false;
};

}  // namespace protocol::rtmp
#endif
