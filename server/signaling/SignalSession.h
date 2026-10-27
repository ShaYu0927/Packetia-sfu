#pragma once
#include "SignalChannel.h"
#include "SignalMessages.h"
#include <atomic>

namespace server::signaling
{
// 只管理两个通道和取消状态；不持有 WebSocket、SDP 或媒体对象。
class SignalSession
{
public:
    SignalSession(SignalKey key, SignalChannel<SignalRequest>::Post room_post,
                  SignalChannel<SignalResponse>::Post connection_post,
                  SignalChannel<SignalRequest>::Consume request,
                  SignalChannel<SignalResponse>::Consume response,
                  std::shared_ptr<SignalBudget> budget)
        : key_(std::move(key)), requests_(std::move(room_post), std::move(request), budget),
          responses_(std::move(connection_post), std::move(response), std::move(budget), 4 * 1024 * 1024, 64, true) {}
    const SignalKey& Key() const { return key_; }
    SignalResult Request(SignalRequest request, std::size_t bytes)
    {
        if (finished_) return SignalResult::Closed;
        const auto result = requests_.Send(std::move(request), bytes);
        if (result != SignalResult::Accepted) Cancel();
        return result;
    }
    SignalResult Respond(Json body, bool initial = false, bool notification = false, uint64_t admission_generation = 0)
    {
        if (finished_) return SignalResult::Closed;
        return SendResponse({std::move(body), false, initial, admission_generation}, initial, notification);
    }
    SignalResult Finish(Json body)
    {
        if (finished_.exchange(true)) return SignalResult::Closed;
        requests_.Close();
        return SendResponse({std::move(body), true}, false, false);
    }
    void ResetAdmission() { responses_.ResetNotificationGate(); }
    void Cancel() { cancelled_ = true; requests_.Cancel(); responses_.Cancel(); }
    bool IsCancelled() const
    { return cancelled_ || requests_.Status() == SignalResult::Cancelled || responses_.Status() == SignalResult::Cancelled; }
private:
    SignalResult SendResponse(SignalResponse response, bool initial, bool notification)
    {
        try
        {
            const auto bytes = response.body.dump().size();
            if (bytes > kMaxMessageBytes) { Cancel(); return SignalResult::Full; }
            const auto result = responses_.Send(std::move(response), bytes, notification, initial);
            if (result != SignalResult::Accepted) Cancel();
            return result;
        }
        catch (...) { Cancel(); return SignalResult::Cancelled; }
    }
    const SignalKey key_;
    SignalChannel<SignalRequest> requests_;
    SignalChannel<SignalResponse> responses_;
    std::atomic<bool> cancelled_{false}, finished_{false};
};
}
