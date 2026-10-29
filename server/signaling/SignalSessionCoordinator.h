#pragma once
#include "AdmissionValidator.h"
#include "RoomOwner.h"
#include "RoomSignalHandler.h"
#include "SignalSession.h"
#include "TaskScheduler.h"
#include <atomic>
#include <limits>
#include <unordered_map>
#include <vector>

namespace server::signaling
{
// 连接 owner 维护接入状态；业务异步路由到固定房间 owner，不同步等待。
class SignalSessionCoordinator : public std::enable_shared_from_this<SignalSessionCoordinator>
{
public:
    SignalSessionCoordinator(std::shared_ptr<TaskScheduler> owner, std::shared_ptr<RoomSignalHandler> room,
                             AdmissionValidator validator, WebRtcServiceStats& stats, uint32_t timeout_ms);
    SignalSessionCoordinator(std::shared_ptr<TaskScheduler> owner, std::vector<RoomOwner> rooms,
                             AdmissionValidator validator, WebRtcServiceStats& stats, uint32_t timeout_ms);
    void Start();
    std::shared_ptr<SignalSession> Open(SignalKey key, SignalChannel<SignalResponse>::Post connection_post,
                                      SignalChannel<SignalResponse>::Consume response);
    void Closed(const SignalKey& key);
    bool ConfirmInitial(const SignalKey& key, uint64_t admission_generation);
    void Tick();
    void Stop();
    WebRtcServiceStats GetStats() const;
    std::size_t ParticipantCount() const;
    std::vector<RoomRelease> ReleasedRooms() const;
    // 控制线程完成各 owner 的停服后，提交最终快照。
    void Refresh(std::size_t shard, std::size_t participants, std::size_t reconnecting, WebRtcServiceStats stats, std::optional<std::vector<RoomRelease>> released = std::nullopt);
    static std::size_t SelectOwner(const std::string& room, std::size_t count);
private:
    static constexpr auto kUnbound = std::numeric_limits<std::size_t>::max();
    using Complete = SignalChannel<SignalRequest>::Complete;
    struct Entry
    {
        std::shared_ptr<SignalSession> session;
        uint64_t deadline = 0;
        bool closing = false;
        bool disconnected = false;
        bool initial_pending = false;
        bool admitting = false;
        bool admitted = false;
        bool resumed = false;
        uint64_t admission_generation = 0;
        std::size_t shard = kUnbound;
    };
    struct Snapshot
    {
        std::size_t participants = 0, reconnecting = 0;
        WebRtcServiceStats stats;
        std::vector<RoomRelease> released;
        std::shared_ptr<std::atomic<bool>> tick_pending = std::make_shared<std::atomic<bool>>(false);
    };
    Entry* Find(const SignalKey& key);
    void Receive(const SignalKey& key, SignalRequest request, Complete complete);
    void Finish(const SignalKey&, SignalRequest::Type, bool admission, bool validated,
                uint64_t started, Json response, Complete complete);
    bool Emit(std::size_t shard, const std::string& connection, Json body, bool close);
    void Expire(Entry& entry);
    bool Detach(const Entry& entry);
    void PostRoom(std::size_t shard, std::function<void()> task);
    bool QueueEmission(std::size_t shard, const std::string&, Json, bool close);
    void RegisterEmission(const SignalKey&, std::size_t, const std::shared_ptr<SignalSession>&);
    void UnregisterEmission(const SignalKey&);
    void ClearEmissions();
    struct Emission
    {
        SignalKey key;
        std::size_t shard;
        std::weak_ptr<SignalSession> session;
    };
    std::mutex emissions_mutex_;
    std::unordered_map<std::string, Emission> emissions_;
    const std::shared_ptr<TaskScheduler> owner_;
    const std::vector<RoomOwner> rooms_;
    const AdmissionValidator validator_;
    WebRtcServiceStats& stats_;
    const uint32_t timeout_ms_;
    std::shared_ptr<SignalBudget> budget_ = std::make_shared<SignalBudget>();
    std::unordered_map<std::string, Entry> sessions_;
    std::vector<Snapshot> snapshots_;
    bool stopped_ = true;
};
}
