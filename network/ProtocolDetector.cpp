#include "ProtocolDetector.h"


namespace protocol 
{
bool ProtocolDetectorSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    LOG_INFO("[Detector] OnRead enter, fd=", conn ? conn->GetSocket() : -1,
             " readable=", buffer.ReadableBytes());
    auto parser = detector_->Detect(buffer);
    if (!parser) return true; // Need more data

    itcp_sess::ISessionBase::Ptr sess;
    if (factory_)
    {
        sess = factory_->Create(parser->Name(), conn);
    }
    else
    {
        sess = std::make_shared<ProtocolDetectorSession>(*this);
    }
    auto promote_session = promote_;
    promote_ = nullptr;
    if (promote_session)
    {
        promote_session(sess);
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
    if (buffer.ReadableBytes() < 4)
        return nullptr;

    const char* p = buffer.Peek();
    size_t n = buffer.ReadableBytes();

    if ((uint8_t)p[0] == '$')
    {
        return std::make_shared<rtsp::RtspProtocolParser>(); 
    }

    size_t k = std::min(n, (size_t)256);
    std::string s(p, k);

    if (s.find("RTSP/1.0") != std::string::npos ||
        s.find("rtsp://") != std::string::npos)
    {
        // return std::make_shared<RtspParser>();
    }

    if (s.find("SIP/2.0") != std::string::npos ||
        s.find("sip:") != std::string::npos)
    {
        // return std::make_shared<SipParser>();
    }

    return nullptr;
}
}