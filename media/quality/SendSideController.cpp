#include "SendSideController.h"
#include "RtcpReciver.h"
#include <utility>
#include <chrono>
#include <limits>
#include <algorithm>

namespace media
{
namespace
{
// Parse into values first: malformed compound framing must not partially
// advance history or GCC. Other RTCP events still go to the media endpoint.
class FeedbackCollector final : public rtcpx::IRtcpObserver
{
public:
    std::vector<rtcpx::TransportFeedbackReport> reports;
    std::vector<rtcpx::RrBlock> receiver_reports;
    void OnTransportFeedback(const rtcpx::TransportFeedbackReport& report) override
    { reports.push_back(report); }
    void OnReceiverReport(uint32_t, uint32_t ssrc, uint8_t lost, int32_t cumulative,
                          uint32_t highest, uint32_t jitter, uint32_t lsr, uint32_t dlsr) override
    { receiver_reports.push_back({ssrc, lost, cumulative, highest, jitter, lsr, dlsr}); }
    void OnSenderReport(uint32_t, uint64_t, uint32_t, uint32_t, uint32_t) override {}
    void OnNack(uint32_t, uint32_t, const uint16_t*, size_t) override {}
    void OnPli(uint32_t, uint32_t) override {}
    void OnFir(uint32_t, uint32_t, uint8_t) override {}
    void OnBye(uint32_t) override {}
    void OnRttUpdated(uint32_t, uint32_t) override {}
};

uint32_t RttMs(uint32_t lsr, uint32_t dlsr)
{
    if (lsr == 0) return 0;
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const auto seconds = duration_cast<std::chrono::seconds>(now);
    const auto nanos = duration_cast<nanoseconds>(now - seconds).count();
    const uint64_t ntp_seconds = seconds.count() + 2208988800ULL;
    const uint32_t compact = static_cast<uint32_t>((ntp_seconds << 16) |
        ((static_cast<uint64_t>(nanos) << 16) / 1000000000ULL));
    const uint32_t elapsed = compact - lsr - dlsr;
    // A negative interval (modulo compact NTP) is not a valid RTT sample.
    if (elapsed > 0x7fffffffU) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(elapsed) * 1000 + 32768) / 65536);
}
}

SendSideController::SendSideController(const NetworkControllerConfig& config)
    : config_(config), controller_(config),
      sequence_allocator_(std::make_shared<TransportSequenceAllocator>()),
      available_(config.network_available)
{
}

std::shared_ptr<TransportSequenceAllocator> SendSideController::SequenceAllocator() const
{
    return sequence_allocator_;
}

void SendSideController::OnPacketSent(const PacketSendInfo& packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (available_) history_.AddPacket(packet);
}

void SendSideController::RegisterSender(uint32_t ssrc, uint32_t clock_rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sender_clock_rates_[ssrc] = clock_rate;
}

void SendSideController::UnregisterSender(uint32_t ssrc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sender_clock_rates_.erase(ssrc);
}

/**
 * @brief 处理接收到的 RTCP 数据包，并根据网络反馈更新发送端拥塞控制状态。
 *
 * 支持的主要反馈类型：
 * 1. TWCC：提供逐包的发送、接收和丢包信息，用于精细带宽估计。
 * 2. RR：提供丢包率、抖动和 RTT；没有 TWCC 时作为弱网控制依据。
 *
 * @param data            RTCP 原始数据。
 * @param size            RTCP 数据长度。
 * @param receive_time_ms 本端收到 RTCP 包的时间，单位为毫秒。
 *
 * @return true  RTCP 解析成功。
 * @return false RTCP 数据格式错误或解析失败。
 */
bool SendSideController::OnRtcpPacket(const uint8_t* data, size_t size, uint64_t receive_time_ms)
{
    FeedbackCollector collector;
    rtcpx::RtcpReceiverImpl receiver(&collector);
    if (!receiver.OnRtcpPacket(data, size)) return false;

    NetworkControlUpdate update;
    UpdateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available_) return true;
         /*
         * 处理 TWCC Transport Feedback。
         *
         * TWCC 可以提供每个 RTP 包的接收状态和接收时间，
         * 控制器能够据此分析网络排队延迟、丢包和接收速率。
         */
        for (const auto& report : collector.reports)
        {
            auto feedback = adapter_.Build(report, history_, receive_time_ms);
            if (feedback.packet_feedbacks.empty()) continue;
            /* 已经收到反馈的包不再需要保留在发送历史中 */
            for (const auto& packet : feedback.packet_feedbacks)
                history_.RemovePacket(packet.sent_packet.transport_sequence);
            
            /* 把逐包反馈交给拥塞控制器 */
            update = controller_.OnTransportPacketsFeedback({std::move(feedback)});
            has_twcc_ = true;
            
            /* 记录最近一次有效 TWCC 的本地接收时间 */
            last_valid_twcc_receive_time_ms_ = receive_time_ms;
        }

        /*
         * 处理 RTCP Receiver Report。
         *
         * RR 中主要包含：
         * - fraction_lost：接收端统计的丢包率
         * - jitter：RTP 时间戳单位的抖动值
         * - LSR/DLSR：用于计算 RTT
         */
        for (const auto& report : collector.receiver_reports)
        {
            // 根据 RR 中的 SSRC 查找对应媒体流的 RTP 时钟频率。
            //
            // 常见时钟频率：
            // - Opus 音频：48000 Hz
            // - H.264 视频：90000 Hz
            const auto sender = sender_clock_rates_.find(report.ssrc);
            if (sender == sender_clock_rates_.end()) continue;
            WeakNetFeedback feedback;
            feedback.now_ms = receive_time_ms;
            feedback.loss_rate = static_cast<double>(report.fraction_lost) / 256.0;
            feedback.jitter_ms = sender->second == 0 ? 0 : static_cast<uint32_t>(
                std::min<uint64_t>(static_cast<uint64_t>(report.jitter) * 1000 / sender->second,
                                   std::numeric_limits<uint32_t>::max()));
            feedback.rtt_ms = RttMs(report.lsr, report.dlsr);
            if (!IsTwccRecent(receive_time_ms))
                update = controller_.OnReceiverFeedback(feedback);
            else if (feedback.rtt_ms != 0)
                update = controller_.OnRoundTripTimeUpdate({feedback.now_ms, feedback.rtt_ms});
        }
        callback = update_callback_;
    }
    

    if (callback && update.HasUpdates()) callback(update);
    return true;
}

void SendSideController::OnReceiverFeedback(const WeakNetFeedback& feedback)
{
    NetworkControlUpdate update;
    UpdateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available_) return;
        // RR is the fallback when TWCC is absent or stale. While TWCC is recent,
        // RR may supplement RTT but must not replace the transport loss/rate
        // with one track's summary (or drive another bitrate increase).
        if (IsTwccRecent(feedback.now_ms))
        {
            if (feedback.rtt_ms == 0) return;
            update = controller_.OnRoundTripTimeUpdate({feedback.now_ms, feedback.rtt_ms});
        }
        else
            update = controller_.OnReceiverFeedback(feedback);
        callback = update_callback_;
    }
    if (callback && update.HasUpdates()) callback(update);
}

void SendSideController::SetNetworkAvailable(bool available)
{
    UpdateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (available_ == available) return;
        available_ = available;
        history_.Clear();
        has_twcc_ = false;
        config_.network_available = available;
        controller_ = GoogCcNetworkController(config_);
        callback = update_callback_;
    }
    // An empty update on a state transition clears the consumer's old rate.
    // Keep sequence allocation monotonic so delayed feedback cannot alias
    // freshly sent packets when the network becomes writable again.
    if (callback) callback(NetworkControlUpdate{});
}

void SendSideController::SetUpdateCallback(UpdateCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    update_callback_ = std::move(callback);
}

NetworkControlUpdate SendSideController::GetNetworkState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return controller_.GetNetworkState(0);
}
}
