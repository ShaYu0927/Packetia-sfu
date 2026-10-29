#include "SignalSessionCoordinator.h"
#include <algorithm>
#include <stdexcept>

namespace server::signaling
{
SignalSessionCoordinator::SignalSessionCoordinator(std::shared_ptr<TaskScheduler> owner, std::shared_ptr<RoomSignalHandler> room,
    AdmissionValidator validator, WebRtcServiceStats& stats, uint32_t timeout_ms)
    : SignalSessionCoordinator(owner, std::vector<RoomOwner>{{owner, std::move(room),
          std::shared_ptr<WebRtcServiceStats>(&stats, [](auto*) {})}}, std::move(validator), stats, timeout_ms) {}

SignalSessionCoordinator::SignalSessionCoordinator(std::shared_ptr<TaskScheduler> owner, std::vector<RoomOwner> rooms,
    AdmissionValidator validator, WebRtcServiceStats& stats, uint32_t timeout_ms)
    : owner_(std::move(owner)), rooms_(std::move(rooms)), validator_(std::move(validator)), stats_(stats),
      timeout_ms_(timeout_ms), snapshots_(rooms_.size()) {}

std::size_t SignalSessionCoordinator::SelectOwner(const std::string& room, std::size_t count)
{
    // 固定算法；同一轮服务运行中 owner 集合不变，房间不会随请求迁移。
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : room) { hash ^= c; hash *= 1099511628211ULL; }
    return count ? hash % count : 0;
}
void SignalSessionCoordinator::PostRoom(std::size_t shard, std::function<void()> task)
{
    const auto& executor = rooms_.at(shard).executor;
    if (executor->IsCurrentThread()) task();
    else if (!executor->Post(std::move(task))) throw std::runtime_error("Room owner unavailable");
}
void SignalSessionCoordinator::Start()
{
    stopped_ = false;
    const auto weak = weak_from_this();
    const std::weak_ptr<TaskScheduler> front = owner_;
    for (std::size_t i = 0; i < rooms_.size(); ++i)
    {
        auto handler = rooms_[i].handler;
        PostRoom(i, [handler, weak, front, i] {
            handler->SetEmitter([weak, front, i](const std::string& connection, Json body, bool close) {
                const auto owner = front.lock();
                if (!owner) return false;
                if (owner->IsCurrentThread())
                {
                    const auto self = weak.lock();
                    return self && self->Emit(i, connection, std::move(body), close);
                }
                // 通知直接进入会话的有界响应队列，避免前置执行器上无界排队。
                const auto self = weak.lock();
                return self && self->QueueEmission(i, connection, std::move(body), close);
            });
        });
    }
}
std::shared_ptr<SignalSession> SignalSessionCoordinator::Open(SignalKey key,
    SignalChannel<SignalResponse>::Post connection_post, SignalChannel<SignalResponse>::Consume response)
{
    if (stopped_ || rooms_.empty() || sessions_.count(key.connection)) return {};
    const auto weak = weak_from_this();
    const std::weak_ptr<TaskScheduler> owner = owner_;
    auto session = std::make_shared<SignalSession>(key,
        [owner](std::function<void()> task) { const auto loop = owner.lock(); return loop && loop->Post(std::move(task)); },
        std::move(connection_post),
        [weak, key](SignalRequest request, Complete complete) {
            if (auto self = weak.lock()) self->Receive(key, std::move(request), std::move(complete));
            else complete();
        }, std::move(response), budget_);
    sessions_.emplace(key.connection, Entry{session, Timestamp::NowMs() + timeout_ms_});
    ++stats_.signaling_opened;
    return session;
}
SignalSessionCoordinator::Entry* SignalSessionCoordinator::Find(const SignalKey& key)
{
    const auto found = sessions_.find(key.connection);
    return found != sessions_.end() && found->second.session->Key() == key ? &found->second : nullptr;
}
bool SignalSessionCoordinator::Detach(const Entry& entry)
{
    if (entry.shard == kUnbound) return true;
    const auto handler = rooms_[entry.shard].handler;
    const auto connection = entry.session->Key().connection;
    const bool rollback = !entry.resumed && (entry.initial_pending || (entry.admitting && !entry.admitted));
    auto cleanup = [handler, connection, rollback] {
        if (rollback) handler->RejectNewAdmission(connection);
        else handler->OnSignalingClosed(connection);
    };
    const auto& executor = rooms_[entry.shard].executor;
    if (executor->IsStopped()) executor->Invoke(std::move(cleanup));
    else if (executor->IsCurrentThread()) cleanup();
    else return executor->Post(std::move(cleanup));
    return true;
}
void SignalSessionCoordinator::Closed(const SignalKey& key)
{
    const auto entry = Find(key);
    if (!entry || entry->disconnected) return;
    entry->disconnected = entry->closing = true;
    entry->session->Cancel();
    UnregisterEmission(key);
    ++stats_.signaling_closed;
    // 执行器暂时满时保留取消后的清理记录，由 Tick 重试；不能遗留旧信令绑定。
    if (Detach(*entry)) sessions_.erase(key.connection);
}
bool SignalSessionCoordinator::ConfirmInitial(const SignalKey& key, uint64_t admission_generation)
{
    const auto entry = Find(key);
    if (!entry || entry->session->IsCancelled()) return false;
    if (entry->admission_generation != admission_generation || entry->closing) return true;
    if (entry->deadline && Timestamp::NowMs() >= entry->deadline) { Expire(*entry); return false; }
    entry->initial_pending = false;
    entry->deadline = 0;
    return true;
}
void SignalSessionCoordinator::Expire(Entry& entry)
{
    if (entry.closing) return;
    ++stats_.admission_timeouts;
    entry.closing = true;
    if (entry.initial_pending || entry.admitting)
    {
        Detach(entry);
        entry.session->Cancel();
    }
    else entry.session->Finish(Error("Room admission timed out"));
}
void SignalSessionCoordinator::Receive(const SignalKey& key, SignalRequest request, Complete complete)
{
    auto entry = Find(key);
    if (!entry || stopped_ || entry->closing || entry->session->IsCancelled()) { complete(); return; }
    if (entry->deadline && Timestamp::NowMs() >= entry->deadline) { Expire(*entry); complete(); return; }
    if (!request.rejection.is_null()) { entry->session->Respond(std::move(request.rejection)); complete(); return; }
    using Type = SignalRequest::Type;
    const auto type = request.type;
    const bool admission = type == Type::Join || type == Type::Resume;
    const auto started = Timestamp::NowUs();
    std::optional<ParticipantInit> init;
    if (admission)
    {
        ++(type == Type::Resume ? stats_.resume_attempts : stats_.join_attempts);
        try
        {
            auto result = validator_.Validate(request);
            if (!result.init)
            {
                Finish(key, type, true, false, started, std::move(result.error), std::move(complete));
                return;
            }
            init = std::move(result.init);
        }
        catch (...) { Finish(key, type, true, false, started, Error("Invalid signaling request"), std::move(complete)); return; }
        if (entry->shard == kUnbound) entry->shard = SelectOwner(init->room_id, rooms_.size());
        entry->admitting = true;
        // 已入房后的重复入房失败不能把原成员当成本次资源回滚。
        if (!entry->admitted) entry->resumed = type == Type::Resume;
        RegisterEmission(key, entry->shard, entry->session);
    }
    if (entry->shard == kUnbound)
    {
        entry->session->Respond(type == Type::Leave ? Json{{"type", "closed"}} : AuthError(service::auth::AuthError::Unauthenticated));
        complete(); return;
    }
    const auto shard = entry->shard;
    const auto room = rooms_[shard];
    const auto session = entry->session;
    const auto weak = weak_from_this();
    const auto front = owner_;
    auto task = [weak, front, room, shard, session, key, request = std::move(request), init = std::move(init),
                 type, admission, started, complete]() mutable {
        Json response;
        if (!session->IsCancelled()) response = room.handler->Handle(key.connection, request, init ? &*init : nullptr);
        const auto participants = room.handler->ParticipantCount();
        const auto reconnecting = room.handler->ReconnectingCount();
        const auto counters = *room.stats;
        auto finish = [weak, shard, key, type, admission, started, response = std::move(response),
                       participants, reconnecting, counters, complete]() mutable {
            if (auto self = weak.lock())
            {
                self->Refresh(shard, participants, reconnecting, counters);
                self->Finish(key, type, admission, true, started, std::move(response), std::move(complete));
            }
            else complete();
        };
        if (front->IsCurrentThread()) finish();
        else if (!front->Post(std::move(finish))) { session->Cancel(); complete(); }
    };
    try { PostRoom(shard, std::move(task)); }
    catch (...) { entry->session->Cancel(); complete(); }
}
void SignalSessionCoordinator::Finish(const SignalKey& key, SignalRequest::Type type, bool admission,
    bool validated, uint64_t started, Json response, Complete complete)
{
    auto entry = Find(key);
    const bool resume = type == SignalRequest::Type::Resume;
    const bool success = response.is_object() && response.value("type", "") == (resume ? "resumed" : "joined");
    if (admission)
    {
        const auto duration = Timestamp::NowUs() - started;
        auto& total = resume ? stats_.resume_duration_us_total : stats_.join_duration_us_total;
        auto& maximum = resume ? stats_.resume_duration_us_max : stats_.join_duration_us_max;
        total += duration; maximum = std::max(maximum, duration);
        if (success) ++(resume ? stats_.resume_succeeded : stats_.join_succeeded);
        else
        {
            ++(resume ? stats_.resume_failed : stats_.join_failed);
            if (validated) ++stats_.admission_binding_failed; else ++stats_.admission_validation_failed;
        }
    }
    if (!entry || stopped_ || entry->closing || entry->session->IsCancelled()) { complete(); return; }
    if (admission)
    {
        entry->admitting = false;
        if (success)
        {
            entry->admitted = true;
            entry->initial_pending = true;
            entry->resumed = resume;
            ++entry->admission_generation;
        }
        else if (!entry->admitted)
        { entry->shard = kUnbound; UnregisterEmission(key); }
    }
    if (type == SignalRequest::Type::Leave && response.value("type", "") == "closed")
    {
        entry->deadline = Timestamp::NowMs() + timeout_ms_;
        ++entry->admission_generation;
        entry->initial_pending = entry->admitted = false;
        entry->shard = kUnbound;
        UnregisterEmission(key);
        entry->session->ResetAdmission();
    }
    entry->session->Respond(std::move(response), admission && success, false, entry->admission_generation);
    complete();
}
bool SignalSessionCoordinator::Emit(std::size_t shard, const std::string& connection, Json body, bool close)
{
    const auto found = sessions_.find(connection);
    if (stopped_ || found == sessions_.end() || found->second.closing || found->second.shard != shard) return false;
    auto& entry = found->second;
    if (close) entry.closing = true;
    const auto result = close ? entry.session->Finish(std::move(body)) : entry.session->Respond(std::move(body), false, true);
    return result == SignalResult::Accepted;
}
void SignalSessionCoordinator::RegisterEmission(const SignalKey& key, std::size_t shard, const std::shared_ptr<SignalSession>& session)
{
    std::lock_guard<std::mutex> lock(emissions_mutex_);
    emissions_.insert_or_assign(key.connection, Emission{key, shard, session});
}
void SignalSessionCoordinator::UnregisterEmission(const SignalKey& key)
{
    std::lock_guard<std::mutex> lock(emissions_mutex_);
    const auto found = emissions_.find(key.connection);
    if (found != emissions_.end() && found->second.key == key) emissions_.erase(found);
}
void SignalSessionCoordinator::ClearEmissions()
{
    std::lock_guard<std::mutex> lock(emissions_mutex_);
    emissions_.clear();
}
bool SignalSessionCoordinator::QueueEmission(std::size_t shard, const std::string& connection, Json body, bool close)
{
    std::shared_ptr<SignalSession> session;
    {
        std::lock_guard<std::mutex> lock(emissions_mutex_);
        const auto found = emissions_.find(connection);
        if (found == emissions_.end() || found->second.shard != shard) return false;
        session = found->second.session.lock();
    }
    if (!session || session->IsCancelled()) return false;
    return (close ? session->Finish(std::move(body)) : session->Respond(std::move(body), false, true)) == SignalResult::Accepted;
}
void SignalSessionCoordinator::Refresh(std::size_t shard, std::size_t participants, std::size_t reconnecting, WebRtcServiceStats stats, std::optional<std::vector<RoomRelease>> released)
{
    snapshots_[shard].participants = participants;
    snapshots_[shard].reconnecting = reconnecting;
    snapshots_[shard].stats = stats;
    if (released) snapshots_[shard].released = std::move(*released);
}
void SignalSessionCoordinator::Tick()
{
    if (stopped_) return;
    const auto now = Timestamp::NowMs();
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto& entry = it->second;
        if (entry.disconnected && Detach(entry)) { it = sessions_.erase(it); continue; }
        if (!entry.disconnected && entry.deadline && now >= entry.deadline) Expire(entry);
        ++it;
    }
    const auto weak = weak_from_this();
    const auto front = owner_;
    for (std::size_t i = 0; i < rooms_.size(); ++i)
    {
        const auto pending = snapshots_[i].tick_pending;
        if (pending->exchange(true)) continue;
        const auto room = rooms_[i];
        try { PostRoom(i, [weak, front, room, i, pending] {
            // 即使网络执行器拒收快照或业务抛出异常，也允许下一轮 Tick 重试。
            struct Reset
            {
                std::shared_ptr<std::atomic<bool>> pending;
                ~Reset() { pending->store(false); }
            } reset{pending};
            room.handler->Tick();
            const auto participants = room.handler->ParticipantCount(), reconnecting = room.handler->ReconnectingCount();
            const auto stats = *room.stats;
            const auto released = room.handler->ReleasedRooms();
            auto update = [weak, i, participants, reconnecting, stats, released] {
                if (const auto self = weak.lock())
                    self->Refresh(i, participants, reconnecting, stats, released);
            };
            if (front->IsCurrentThread()) update(); else front->Post(std::move(update));
        }); }
        catch (...) { pending->store(false); }
    }
}
std::size_t SignalSessionCoordinator::ParticipantCount() const
{
    std::size_t count = 0;
    for (const auto& snapshot : snapshots_) count += snapshot.participants;
    return count;
}
std::vector<RoomRelease> SignalSessionCoordinator::ReleasedRooms() const
{
    std::vector<RoomRelease> result;
    for (const auto& snapshot : snapshots_)
        for (const auto& release : snapshot.released)
        {
            result.push_back(release);
            if (result.size() == 16) return result;
        }
    return result;
}
WebRtcServiceStats SignalSessionCoordinator::GetStats() const
{
    auto snapshot = stats_;
    snapshot.active_signaling = 0;
    snapshot.pending_admissions = snapshot.reconnecting_participants = 0;
    for (const auto& item : sessions_)
    {
        if (!item.second.disconnected) ++snapshot.active_signaling;
        if (item.second.deadline && !item.second.closing) ++snapshot.pending_admissions;
    }
    for (std::size_t i = 0; i < rooms_.size(); ++i)
    {
        snapshot.reconnecting_participants += snapshots_[i].reconnecting;
        if (rooms_[i].stats.get() == &stats_) continue;
        const auto& s = snapshots_[i].stats;
        snapshot.recovery_expired += s.recovery_expired;
        snapshot.negotiation_timeouts += s.negotiation_timeouts;
        snapshot.media_closed += s.media_closed;
    }
    return snapshot;
}
void SignalSessionCoordinator::Stop()
{
    stopped_ = true;
    for (auto& item : sessions_)
    {
        item.second.session->Cancel();
        if (!item.second.disconnected) ++stats_.signaling_closed;
    }
    sessions_.clear();
    ClearEmissions();
    for (std::size_t i = 0; i < rooms_.size(); ++i)
    {
        const auto handler = rooms_[i].handler;
        const auto& executor = rooms_[i].executor;
        if (executor->IsStopped()) executor->Invoke([handler] { handler->SetEmitter({}); });
        // 停服时执行器可能拒收；回调只持有协调器弱引用，handler::Stop 仍会清空它。
        else executor->Post([handler] { handler->SetEmitter({}); });
    }
}
}
