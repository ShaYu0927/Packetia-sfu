#ifndef _AI_TYPES_H_
#define _AI_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace service
{
namespace ai
{

enum class AITaskType : uint8_t
{
    Chat = 0,
    Summarize,
    Moderate,
    Vision
};

struct AIImageInput
{
    std::string mime_type;
    std::string uri;
};

struct AIRequest
{
    std::string request_id;
    std::string room_id;
    std::string participant_id;
    std::string session_id;
    AITaskType task = AITaskType::Chat;
    std::string text;
    std::vector<AIImageInput> images;
    uint32_t timeout_ms = 15000;
};

struct AIUsage
{
    uint64_t input_tokens = 0;
    uint64_t output_tokens = 0;
};

struct AIChunk
{
    std::string request_id;
    uint64_t sequence = 0;
    std::string text;
};

struct AIResponse
{
    std::string request_id;
    bool success = false;
    std::string text;
    std::string error;
    AIUsage usage;
};

using AIRequestHandle = uint64_t;

} // namespace ai
} // namespace service

#endif /* _AI_TYPES_H_ */
