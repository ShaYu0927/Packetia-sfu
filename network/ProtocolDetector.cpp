#include "ProtocolDetector.h"
#include "RtspUtil.h"


namespace protocol 
{
bool ProtocolDetectorSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    LOG_INFO("[Detector] OnRead enter, fd=",
            conn ? conn->GetSocket() : -1,
            " readable=", buffer.ReadableBytes());

    auto parser = detector_->Detect(buffer);

    if (!parser)
    {
        LOG_INFO("[Detector] Detect result: NeedMoreData, fd=",
                conn ? conn->GetSocket() : -1);
        return true;
    }
    itcp_sess::ISessionBase::Ptr sess;

    if (sess_factory_)
    {
        sess = sess_factory_->Create(parser->Name(), conn);

        LOG_INFO("[Detector] Session created by factory, type=",
                parser->Name(),
                " sess_ptr=",
                sess.get());
    }
    else
    {
        sess = std::make_shared<ProtocolDetectorSession>(*this);
        LOG_INFO("[Detector] No factory, fallback to ProtocolDetectorSession, sess_ptr=",
                sess.get());
    }

    auto promote_session = promote_;
    promote_ = nullptr;

    if (promote_session)
    {
        LOG_INFO("[Detector] Promoting session, sess_ptr=", sess.get());
        LOG_INFO("[Detector] OnRead exit, fd=",
            conn ? conn->GetSocket() : -1);
        promote_session(sess);
        sess->Start();
        if (buffer.ReadableBytes() > 0)
        {
            sess->OnRead(conn, buffer);
        }
    }
    else
    {
        LOG_INFO("[Detector] No promote callback set");
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
    {
        LOG_INFO("[Detect] Not enough data, readable=",
                buffer.ReadableBytes());
        return nullptr;
    }

    const char* p = buffer.Peek();
    size_t n = buffer.ReadableBytes();

#if RTP_DEBUG
    LOG_INFO("[Detect] Enter, readable=", n);

    // 打印前 32 字节（ASCII + HEX）
    size_t dump_len = std::min(n, (size_t)32);

    std::ostringstream hex;
    std::ostringstream ascii;

    for (size_t i = 0; i < dump_len; ++i)
    {
        uint8_t c = (uint8_t)p[i];
        hex << std::hex << std::setw(2) << std::setfill('0')
            << (int)c << " ";

        if (std::isprint(c))
            ascii << (char)c;
        else
            ascii << '.';
    }

    LOG_INFO("[Detect] head32_hex=", hex.str());
    LOG_INFO("[Detect] head32_ascii=", ascii.str());
#endif

    /* ---------- RTP over TCP ---------- */
    if ((uint8_t)p[0] == '$')
    {
        LOG_INFO("[Detect] Hit RTP interleaved ($)");
        return std::make_shared<rtsp::RtspProtocolParser>();
    }

    /* ---------- 文本协议 ---------- */
    size_t k = std::min(n, (size_t)256);
    std::string s(p, k);

    if (s.find("RTSP/1.0") != std::string::npos ||
        s.find("rtsp://") != std::string::npos)
    {
        LOG_INFO("[Detect] Hit RTSP text protocol");
        return std::make_shared<rtsp::RtspProtocolParser>();
    }

    if (s.find("SIP/2.0") != std::string::npos ||
        s.find("sip:") != std::string::npos)
    {
        LOG_INFO("[Detect] Hit SIP protocol");
        // return std::make_shared<SipParser>();
    }

    LOG_INFO("[Detect] No protocol matched");
    return nullptr;
}
}