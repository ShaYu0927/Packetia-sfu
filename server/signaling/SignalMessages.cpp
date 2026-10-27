#include "SignalMessages.h"
#include <stdexcept>

namespace server::signaling
{
bool DecodeRequest(const std::string& text, SignalRequest& request, Json& error)
{
    if (text.size() > kMaxMessageBytes) { error = Error("Signaling message is too large"); return false; }
    try
    {
        auto body = Json::parse(text, [](int depth, Json::parse_event_t, Json&)
        {
            if (depth > 8) throw std::invalid_argument("信令嵌套过深");
            return true;
        });
        if (!body.is_object() || !body.contains("type") || !body["type"].is_string())
        { error = Error("Expected a signaling message type"); return false; }
        request.type = SignalRequest::ParseType(body["type"].get_ref<const std::string&>());
        request.body = std::move(body);
        return true;
    }
    catch (...) { error = Error("Invalid signaling request"); return false; }
}
}
