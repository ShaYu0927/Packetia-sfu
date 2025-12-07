#pragma once

#include <string>
#include <unordered_map>

typedef struct SipRequest {
    std::string method;
    std::string uri;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};


typedef struct SipResponse {
    int status_code;
    std::string reason_phrase;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};


struct SipTransaction {
    enum State {
        Trying,
        Proceeding,
        Completed,
        Confirmed,
        Terminated
    } state;

    std::string branch;
    std::string call_id;
};