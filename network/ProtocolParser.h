#pragma once

#include "BufferRead.h"

#include <functional>

enum class ParseResult {
    Ok,
    NeedMoreData,
    Error
};

class ProtocolParser {
public:
    using Ptr = std::shared_ptr<ProtocolParser>;
    virtual ~ProtocolParser() = default;

    virtual ParseResult Parse(BufferReader& buffer) = 0;
    virtual const char* Name() const = 0;
};


class ProtocolDetector {
public:
    using Creator = std::function<ProtocolParser::Ptr()>;

    struct Rule {
        std::string name;
        size_t min_bytes;
        std::function<bool(const uint8_t*, size_t)> matcher;
        Creator creator;
    };

    void Register(Rule rule) {
        rules_.push_back(std::move(rule));
    }

    ProtocolParser::Ptr Detect(BufferReader& buffer);

private:
    std::vector<Rule> rules_;
};

