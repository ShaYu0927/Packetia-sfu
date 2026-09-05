#include "rtmp_session.h"

namespace protocol::rtmp
{

RtmpSession::RtmpSession(TcpConnection::Ptr connection, EndpointRole role)
    : RtmpSession(std::make_shared<RtmpTcpTransport>(std::move(connection)), role)
{
}

RtmpSession::RtmpSession(std::shared_ptr<IRtmpTransport> transport,
                         EndpointRole role)
    : transport_(std::move(transport)), role_(role), protocol_(role)
{
}

void RtmpSession::Start()
{
    if (!transport_ || !transport_->Start(weak_from_this())) return;

    if (role_ == EndpointRole::kClient) {
        SendBytes(protocol_.StartHandshake());
    }
}

bool RtmpSession::OnRtmpBytes(const uint8_t* data, size_t size)
{
    std::vector<RtmpMessage> messages;
    protocol_.Feed(data, size, messages);

    SendBytes(protocol_.TakeOutboundData());

    for (const auto& message : messages)
    {
        if (handler_) handler_->OnMessage(*this, message);
    }

    return protocol_.state() != ProtocolState::kFailed;
}

void RtmpSession::OnRtmpTransportClosed(int reason)
{
    if (handler_) handler_->OnClosed(*this, reason);
}


void RtmpSession::Send(const RtmpMessage& message)
{
    std::vector<uint8_t> bytes;
    protocol_.Encode(message, bytes);
    SendBytes(bytes);
}


void RtmpSession::SetHandler(const std::shared_ptr<IRtmpMessageHandler>& handler)
{
    handler_ = handler;
}

void RtmpSession::SendBytes(const std::vector<uint8_t>& bytes)
{
    if (!transport_ || bytes.empty()) return;
    if (transport_->Send(bytes.data(), bytes.size()) != RtmpTransportSendResult::Ok)
        transport_->Close();
}
}  // namespace protocol::rtmp
