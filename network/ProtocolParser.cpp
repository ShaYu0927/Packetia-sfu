#include "ProtocolParser.h"

namespace protocolDetector
{
bool ProtocolDetectorSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    auto parser = detector_->Detect(buffer);
    if (!parser) return true; // Need more data

    itcp_sess::ISessionBase::Ptr sess;
    if (std::string(parser->Name()) == "SIP") 
    {
        // sess = std::make_shared<SipSession>(std::move(parser));
    } 
    else if (std::string(parser->Name()) == "RTSP") 
    {
        // sess = std::make_shared<RtspSession>(std::move(parser));
    } 
    else 
    {
        // sess = std::make_shared<RawSession>(std::move(parser));
    }

    auto promote_session = promote_;
    promote_ = nullptr;
    if (promote_session)
    {
        promote_session(std::make_shared<ProtocolDetectorSession>(*this));
    }
    return true;
}

void ProtocolDetectorSession::OnClosed(int reason)
{
    (void)reason;
}

void ProtocolDetectorSession::Start()
{

}

std::shared_ptr<ProtocolParser> ProtocolDetector::Detect(BufferReader& buffer) const
{
    return nullptr;
}

}
