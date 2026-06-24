#pragma once

#include "TcpSession.h"
#include "sip.h"

#include <memory>
#include <string>

namespace sip
{

class SipSession : public itcp_sess::ISessionBase,
                   public std::enable_shared_from_this<SipSession>
{
public:
    using Ptr = std::shared_ptr<SipSession>;

    explicit SipSession(TcpConnection::Ptr conn);
    ~SipSession();

    bool OnRead(TcpConnection::Ptr conn, BufferReader& buffer) override;
    void OnClosed(int reason) override;
    void Start() override;

private:
    enum class ConsumeResult
    {
        Consumed,
        NeedMore,
        Error
    };

    ConsumeResult TryConsumeOne(BufferReader& buffer);
    bool ParseMessage(const char* data, size_t len, SipMessage& out);
    void Dispatch(const SipMessage& msg);
    void SendResponse(const SipRequest& req, int status_code, const std::string& reason, const std::string& extra_headers = "", const std::string& body = "");
    void SendRaw(const std::string& data);

private:
    TcpConnection::Ptr conn_;
};

} // namespace sip