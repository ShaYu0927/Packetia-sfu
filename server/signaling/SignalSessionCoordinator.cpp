#include "SignalSessionCoordinator.h"
#include <algorithm>

namespace server::signaling
{
SignalSessionCoordinator::SignalSessionCoordinator(std::shared_ptr<TaskScheduler> owner, std::shared_ptr<RoomSignalHandler> room,
    AdmissionValidator validator, WebRtcServiceStats& stats, uint32_t timeout_ms)
    : owner_(std::move(owner)), room_(std::move(room)), validator_(std::move(validator)), stats_(stats), timeout_ms_(timeout_ms) {}

void SignalSessionCoordinator::Start()
{
    stopped_ = false;
    const auto weak = weak_from_this();
    room_->SetEmitter([weak](const std::string& connection, Json body, bool close)
    { const auto self = weak.lock(); return self && self->Emit(connection, std::move(body), close); });
}
std::shared_ptr<SignalSession> SignalSessionCoordinator::Open(SignalKey key,
    SignalChannel<SignalResponse>::Post connection_post, SignalChannel<SignalResponse>::Consume response)
{
    if (stopped_ || sessions_.count(key.connection)) return {};
    const auto weak = weak_from_this();
    const std::weak_ptr<TaskScheduler> owner = owner_;
    auto session = std::make_shared<SignalSession>(key,
        [owner](std::function<void()> task) { const auto loop = owner.lock(); return loop && loop->Post(std::move(task)); },
        std::move(connection_post),
        [weak, key](SignalRequest request) { if (auto self = weak.lock()) self->Receive(key, std::move(request)); },
        std::move(response), budget_);
    sessions_.emplace(key.connection, Entry{session, Timestamp::NowMs() + timeout_ms_});
    ++stats_.signaling_opened;
    return session;
}
SignalSessionCoordinator::Entry* SignalSessionCoordinator::Find(const SignalKey& key)
{
    const auto found = sessions_.find(key.connection);
    return found != sessions_.end() && found->second.session->Key() == key ? &found->second : nullptr;
}
void SignalSessionCoordinator::Closed(const SignalKey& key)
{
    const auto entry = Find(key);
    if (!entry) return;
    entry->session->Cancel();
    // 初始响应尚未被网络队列接纳的新参与者，本次接入失败必须回滚。
    if (entry->initial_pending && !entry->resumed) room_->RejectNewAdmission(key.connection);
    else room_->OnSignalingClosed(key.connection);
    sessions_.erase(key.connection);
    ++stats_.signaling_closed;
}
bool SignalSessionCoordinator::ConfirmInitial(const SignalKey& key, uint64_t admission_generation)
{
    const auto entry = Find(key);
    if (!entry || entry->session->IsCancelled()) return false;
    // 流水线中的旧首响仍按序发送，但不能确认新一轮接入。
    if (entry->admission_generation != admission_generation || entry->closing) return true;
    if (entry->deadline && Timestamp::NowMs() >= entry->deadline)
    {
        Expire(*entry);
        return false;
    }
    entry->initial_pending = false;
    entry->deadline = 0;
    return true;
}

void SignalSessionCoordinator::Expire(Entry& entry)
{
    if (entry.closing) return;
    ++stats_.admission_timeouts;
    entry.closing = true;
    if (entry.initial_pending)
    {
        if (!entry.resumed) room_->RejectNewAdmission(entry.session->Key().connection);
        else room_->OnSignalingClosed(entry.session->Key().connection);
        entry.session->Cancel();
    }
    else entry.session->Finish(Error("Room admission timed out"));
}
void SignalSessionCoordinator::Receive(const SignalKey& key, SignalRequest request)
{
    auto entry = Find(key);
    if (!entry || stopped_ || entry->closing || entry->session->IsCancelled()) return;
    if (entry->deadline && Timestamp::NowMs() >= entry->deadline) { Expire(*entry); return; }
    if (!request.rejection.is_null()) { entry->session->Respond(std::move(request.rejection)); return; }
    using Type = SignalRequest::Type;
    const bool admission = request.type == Type::Join || request.type == Type::Resume;
    if (!admission)
    {
        auto response = room_->Handle(key.connection, request);
        if (request.type == Type::Leave && !room_->HasMember(key.connection))
        {
            entry->deadline = Timestamp::NowMs() + timeout_ms_;
            ++entry->admission_generation;
            entry->initial_pending = false;
            entry->session->ResetAdmission();
        }
        entry->session->Respond(std::move(response));
        return;
    }
    const bool resume = request.type == Type::Resume;
    auto& attempts = resume ? stats_.resume_attempts : stats_.join_attempts;
    auto& succeeded = resume ? stats_.resume_succeeded : stats_.join_succeeded;
    auto& failed = resume ? stats_.resume_failed : stats_.join_failed;
    auto& total = resume ? stats_.resume_duration_us_total : stats_.join_duration_us_total;
    auto& maximum = resume ? stats_.resume_duration_us_max : stats_.join_duration_us_max;
    ++attempts;
    const auto before = succeeded, started = Timestamp::NowUs();
    bool validated = false;
    Json response;
    try
    {
        const auto result = validator_.Validate(request);
        validated = result.init.has_value();
        response = validated ? room_->Handle(key.connection, request, &*result.init) : result.error;
    }
    catch (...) { response = Error("Invalid signaling request"); }
    const auto duration = Timestamp::NowUs() - started;
    total += duration; maximum = std::max(maximum, duration);
    const bool success = succeeded != before;
    if (!success)
    {
        ++failed;
        if (validated) ++stats_.admission_binding_failed; else ++stats_.admission_validation_failed;
    }
    else { entry->initial_pending = true; entry->resumed = resume; ++entry->admission_generation; }
    entry->session->Respond(std::move(response), success, false, entry->admission_generation);
}
bool SignalSessionCoordinator::Emit(const std::string& connection, Json body, bool close)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end() || found->second.closing) return false;
    auto& entry = found->second;
    if (close) entry.closing = true;
    const auto result = close ? entry.session->Finish(std::move(body)) : entry.session->Respond(std::move(body), false, true);
    return result == SignalResult::Accepted;
}
void SignalSessionCoordinator::Tick()
{
    if (stopped_) return;
    const auto now = Timestamp::NowMs();
    for (auto& item : sessions_)
        if (item.second.deadline && now >= item.second.deadline) Expire(item.second);
    room_->Tick();
}
WebRtcServiceStats SignalSessionCoordinator::GetStats() const
{
    auto snapshot = stats_;
    snapshot.active_signaling = sessions_.size();
    for (const auto& item : sessions_) if (item.second.deadline && !item.second.closing) ++snapshot.pending_admissions;
    snapshot.reconnecting_participants = room_->ReconnectingCount();
    return snapshot;
}
void SignalSessionCoordinator::Stop()
{
    stopped_ = true;
    for (auto& item : sessions_) item.second.session->Cancel();
    stats_.signaling_closed += sessions_.size();
    sessions_.clear(); room_->SetEmitter({});
}
}
