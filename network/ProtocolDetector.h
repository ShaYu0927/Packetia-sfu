#ifndef _PROTOCOL_DETECTOR_H_
#define _PROTOCOL_DETECTOR_H_

#include <functional>
#include <vector>
#include <memory>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <algorithm>

#include "ProtocolParser.h"
#include "RtspUtil.h"
#include "BufferRead.h"
#include "TcpSession.h"


namespace protocol
{
class ProtocolDetector 
{
public:
    using Creator = std::function<std::shared_ptr<ProtocolParser>()>;

    struct Rule 
    {
        std::string name;
        size_t min_bytes = 0;
        std::function<bool(const uint8_t*, size_t)> matcher;
        Creator creator;
        int priority = 0;
    };

    void Register(Rule rule) 
    {
        rules_.push_back(std::move(rule));
        std::sort(rules_.begin(), rules_.end(),
                  [](const Rule& a, const Rule& b){ return a.priority > b.priority; });
    }

    std::shared_ptr<ProtocolParser> Detect(BufferReader& buffer) const;

private:
    std::vector<Rule> rules_;
};


class ProtocolDetectorSession : public std::enable_shared_from_this<ProtocolDetectorSession>
                                    , public itcp_sess::ISessionBase 
{
public:
    ProtocolDetectorSession(std::shared_ptr<ProtocolDetector> detector,
                  std::function<void(itcp_sess::ISessionBase::Ptr)> promote)
        : detector_(std::move(detector)), promote_(std::move(promote)) {}

    void SetSessionFactory(std::shared_ptr<itcp_sess::ISessionFactory> f)
	{
		sess_factory_ = std::move(f);
	}
    

protected:
    bool OnRead(TcpConnection::Ptr conn, BufferReader& buffer) override;
    void OnClosed(int reason) override;
    void Start() override;

private:
    std::shared_ptr<ProtocolDetector> detector_;
    std::function<void(itcp_sess::ISessionBase::Ptr)> promote_;

    std::shared_ptr<itcp_sess::ISessionFactory> sess_factory_;
};
}


#endif /* _PROTOCOL_DETECTOR_H_ */