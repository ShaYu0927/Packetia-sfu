#ifndef _PROTOCOLPARSER_H_
#define _PROTOCOLPARSER_H_

#include "BufferRead.h"
#include <functional>
#include "TcpSession.h"

namespace protocolDetector
{
enum class DetectResult 
{
    Undecided,  
    Matched,   
};

class ProtocolParser 
{
public:
    enum class ParseResult 
    {
        Ok,
        NeedMoreData,
        Error
    };
    using Ptr = std::shared_ptr<ProtocolParser>;
    virtual ~ProtocolParser() = default;

    virtual ParseResult Parse(BufferReader& buffer) = 0;
    virtual const char* Name() const = 0;
};

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
    

protected:
    bool OnRead(TcpConnection::Ptr conn, BufferReader& buffer) override;
    void OnClosed(int reason) override;
    void Start() override;

private:
    std::shared_ptr<ProtocolDetector> detector_;
    std::function<void(itcp_sess::ISessionBase::Ptr)> promote_;
};
}

#endif // _PROTOCOLPARSER_H_