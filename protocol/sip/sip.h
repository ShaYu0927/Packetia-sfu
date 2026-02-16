#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <variant>

enum class SipMsgType { Request, Response };


typedef struct SipHeader 
{
    std::string name;
    std::string value;
}SipHeader;


typedef struct SipHeaders 
{
    std::vector<SipHeader> list;
    std::unordered_map<std::string, std::vector<size_t>> index;

    void build_index();

    std::string get_one(std::string_view name) const;
    std::vector<std::string> get_all(std::string_view name) const;
    void add(std::string name, std::string value);
}SipHeaders;

typedef struct SipRequest 
{
    std::string method;
    std::string uri;
    std::string version = "SIP/2.0";
    SipHeaders headers;
    std::string body;
}SipRequest;


typedef struct SipResponse 
{
    std::string version = "SIP/2.0";
    int status_code;
    std::string reason_phrase;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
}SipResponse;


struct SipTransaction 
{
    enum State 
    {
        Trying,
        Proceeding,
        Completed,
        Confirmed,
        Terminated
    } state;

    std::string branch;
    std::string call_id;
};

typedef struct SipMessage 
{
    SipMsgType type;
    std::variant<SipRequest, SipResponse> msg;

    bool is_request()  const { return type == SipMsgType::Request; }
    bool is_response() const { return type == SipMsgType::Response; }

    SipRequest&  as_req()  { return std::get<SipRequest>(msg); }
    SipResponse& as_resp() { return std::get<SipResponse>(msg); }
}SipMessage;