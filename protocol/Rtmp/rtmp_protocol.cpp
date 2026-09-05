#include "rtmp_protocol.h"

#include <algorithm>

namespace protocol::rtmp {
namespace {

void WriteBe32(uint8_t* output, uint32_t value)
{
    output[0] = static_cast<uint8_t>(value >> 24);
    output[1] = static_cast<uint8_t>(value >> 16);
    output[2] = static_cast<uint8_t>(value >> 8);
    output[3] = static_cast<uint8_t>(value);
}

std::vector<uint8_t> MakeC0C1(uint32_t timestamp)
{
    std::vector<uint8_t> result(1 + kHandshakeSize, 0);
    result[0] = kPlainHandshakeVersion;
    WriteBe32(result.data() + 1, timestamp);
    return result;
}

}  // namespace

RtmpProtocol::RtmpProtocol(EndpointRole role) : role_(role)
{
    if (role_ == EndpointRole::kServer) 
    {
        next_step_func_ = [this](std::vector<RtmpMessage>& messages) {
            return HandleC0C1(messages);
        };
    } 
    else 
    {
        next_step_func_ = [this](std::vector<RtmpMessage>& messages) {
            return HandleS0S1S2(messages);
        };
    }
}

void RtmpProtocol::Feed(const uint8_t* data, std::size_t size, std::vector<RtmpMessage>& messages) 
{
    if (state_ == ProtocolState::kFailed || (!data && size)) 
    {
        Fail("invalid RTMP input"); return;
    }
    if (size) handshake_buffer_.insert(handshake_buffer_.end(), data, data + size);
    while (state_ != ProtocolState::kFailed && next_step_func_(messages)) {}
}

void RtmpProtocol::Encode(const RtmpMessage& message, std::vector<uint8_t>& output) 
{
    if (state_ == ProtocolState::kChunkStream && !chunk_codec_.Encode(message, output)) Fail("RTMP encode failed");
}

std::vector<uint8_t> RtmpProtocol::StartHandshake(uint32_t timestamp) 
{
    if (state_ == ProtocolState::kFailed || role_ != EndpointRole::kClient) 
    {
        Fail("only an RTMP client can start the handshake");
        return {};
    }
    return MakeC0C1(timestamp);
}

std::vector<uint8_t> RtmpProtocol::TakeOutboundData()
{
    std::vector<uint8_t> result;
    result.swap(outbound_data_);
    return result;
}

bool RtmpProtocol::HandleC0C1(std::vector<RtmpMessage>&)
{
    if (handshake_buffer_.size() < 1 + kHandshakeSize) return false;
    if (handshake_buffer_[0] != kPlainHandshakeVersion) 
    {
        Fail("unsupported RTMP handshake version");
        return false;
    }

    // Simple handshake: S2 echoes C1; S1 contains the server time and zeros.
    outbound_data_.resize(1 + 2 * kHandshakeSize, 0);
    outbound_data_[0] = kPlainHandshakeVersion;
    std::copy_n(handshake_buffer_.begin() + 1, kHandshakeSize, outbound_data_.begin() + 1 + kHandshakeSize);
    handshake_buffer_.erase(handshake_buffer_.begin(), handshake_buffer_.begin() + 1 + kHandshakeSize);
    next_step_func_ = [this](std::vector<RtmpMessage>& messages) {
        return HandleC2(messages);
    };
    return true;
}

bool RtmpProtocol::HandleS0S1S2(std::vector<RtmpMessage>&)
{
    constexpr std::size_t response_size = 1 + 2 * kHandshakeSize;
    if (handshake_buffer_.size() < response_size) return false;
    if (handshake_buffer_[0] != kPlainHandshakeVersion) 
    {
        Fail("unsupported RTMP handshake version");
        return false;
    }

    // C2 echoes S1.
    outbound_data_.assign(handshake_buffer_.begin() + 1, handshake_buffer_.begin() + 1 + kHandshakeSize);
    handshake_buffer_.erase(handshake_buffer_.begin(), handshake_buffer_.begin() + response_size);
    SetRtmpStep();
    return true;
}

bool RtmpProtocol::HandleC2(std::vector<RtmpMessage>&)
{
    if (handshake_buffer_.size() < kHandshakeSize) return false;
    handshake_buffer_.erase(handshake_buffer_.begin(),
                            handshake_buffer_.begin() + kHandshakeSize);
    SetRtmpStep();
    return true;
}

bool RtmpProtocol::HandleRtmp(std::vector<RtmpMessage>& messages)
{
    if (handshake_buffer_.empty()) return false;
    if (!chunk_codec_.Feed(handshake_buffer_.data(), handshake_buffer_.size(), messages)) {
        Fail("invalid RTMP chunk");
    }
    handshake_buffer_.clear();
    return false;
}

void RtmpProtocol::SetRtmpStep()
{
    state_ = ProtocolState::kChunkStream;
    next_step_func_ = [this](std::vector<RtmpMessage>& messages) 
    {
        return HandleRtmp(messages);
    };
}

void RtmpProtocol::Fail(const char* reason) 
{
    state_ = ProtocolState::kFailed; last_error_ = reason;
}
}  // namespace protocol::rtmp
