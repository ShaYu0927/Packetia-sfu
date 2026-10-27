#pragma once
#include "AdmissionValidator.h"
#include "RoomSignalHandler.h"
#include "SignalSession.h"
#include "TaskScheduler.h"
#include <unordered_map>

namespace server::signaling
{
// 单节点进程内协调器；与房间共用 owner，网络回调只投递有界消息。
class SignalSessionCoordinator : public std::enable_shared_from_this<SignalSessionCoordinator>
{
public:
    SignalSessionCoordinator(std::shared_ptr<TaskScheduler> owner, std::shared_ptr<RoomSignalHandler> room,
                             AdmissionValidator validator, WebRtcServiceStats& stats, uint32_t timeout_ms);
    void Start();
    std::shared_ptr<SignalSession> Open(SignalKey key, SignalChannel<SignalResponse>::Post connection_post,
                                      SignalChannel<SignalResponse>::Consume response);
    void Closed(const SignalKey& key);
    bool ConfirmInitial(const SignalKey& key, uint64_t admission_generation);
    void Tick();
    void Stop();
    WebRtcServiceStats GetStats() const;
private:
    struct Entry
    {
        std::shared_ptr<SignalSession> session;
        uint64_t deadline = 0;
        bool closing = false;
        bool initial_pending = false;
        bool resumed = false;
        uint64_t admission_generation = 0;
    };
    Entry* Find(const SignalKey& key);
    void Receive(const SignalKey& key, SignalRequest request);
    bool Emit(const std::string& connection, Json body, bool close);
    void Expire(Entry& entry);
    const std::shared_ptr<TaskScheduler> owner_;
    const std::shared_ptr<RoomSignalHandler> room_;
    const AdmissionValidator validator_;
    WebRtcServiceStats& stats_;
    const uint32_t timeout_ms_;
    std::shared_ptr<SignalBudget> budget_ = std::make_shared<SignalBudget>();
    std::unordered_map<std::string, Entry> sessions_;
    bool stopped_ = true;
};
}
