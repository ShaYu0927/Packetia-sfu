#include "ReceiveFeedbackHandler.h"
#include "RtcpHealper.h"
#include "logger.h"

#include <algorithm>
#include <limits>

namespace media::endpoint
{
namespace
{
uint32_t ClampToUint32(double value)
{
    if (value <= 0.0)
    {
        return 0;
    }
    const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(std::min(value, maximum));
}
}

bool ReceiveFeedbackHandler::RegisterSource(uint32_t source_ssrc)
{
    return quality_.RegisterSource(source_ssrc);
}

std::optional<NetworkStatsSnapshot> ReceiveFeedbackHandler::GetSnapshot(uint32_t source_ssrc, uint64_t now_ms) const
{
    return quality_.GetSnapshot(source_ssrc, now_ms);
}

bool ReceiveFeedbackHandler::Evaluate(const rtsp::RtpReceiverTrack::Ptr& track, uint64_t now_ms,
    uint32_t local_rtcp_ssrc, const SendCallback& send)
{
    if (!track) return false;
    const auto source_ssrc = track->getTrackInfo().ssrc;
    if (!quality_.GetSnapshot(source_ssrc, now_ms)) return false;
    if (auto video = std::dynamic_pointer_cast<rtsp::RtpVideoTracker>(track))
    {
        video->UpdateNackRtt(track->GetRttMs());
        video->TickNack(now_ms);
    }
    const auto report = track->BuildReceiverReport(now_ms);

    if (send)
    {
        rtcpx::RrBlock block;
        block.ssrc = report.media_ssrc;
        block.fraction_lost = report.fraction_lost;
        block.cumulative_lost = report.cumulative_lost;
        block.extended_highest_seq = report.extended_highest_seq;
        block.jitter = report.jitter;
        block.lsr = report.lsr;
        block.dlsr = report.dlsr;
        auto rtcp_sender = rtcpx::CreateRtcpSender();
        const auto packet = rtcp_sender->BuildReceiverReport(local_rtcp_ssrc, block);
        if (packet.empty() || !send(packet.data(), packet.size()))
        {
            LOG_ERROR("[RTCP][RR] send failed", " media_ssrc=", source_ssrc);
        }
    }

    WeakNetFeedback feedback;
    feedback.now_ms = now_ms;
    feedback.send_bitrate_bps = ClampToUint32(track->GetSenderBitrateBps());
    feedback.receive_bitrate_bps = ClampToUint32(track->GetReceiverBitrateBps(now_ms));
    feedback.loss_rate = static_cast<double>(report.fraction_lost) / 256.0;
    const uint32_t clock_rate = track->getTrackInfo().clock_rate;
    if (clock_rate > 0)
    {
        feedback.jitter_ms = static_cast<uint32_t>(static_cast<uint64_t>(report.jitter) * 1000ULL / clock_rate);
    }
    feedback.rtt_ms = track->GetRttMs();
    feedback.nack_count = track->GetNackCount();
    feedback.pli_count = track->GetPliCount();
    feedback.fir_count = track->GetFirCount();

    LOG_DEBUG("[WEAK_NET][FEEDBACK] submit",
             " ssrc=", source_ssrc,
             " now_ms=", feedback.now_ms,
             " sender_bitrate_bps=", feedback.send_bitrate_bps,
             " receive_bitrate_bps=", feedback.receive_bitrate_bps,
             " fraction_lost=", static_cast<uint32_t>(report.fraction_lost),
             " loss_rate=", feedback.loss_rate,
             " jitter_ticks=", report.jitter,
             " jitter_ms=", feedback.jitter_ms,
             " rtt_ms=", feedback.rtt_ms,
             " nack_count=", feedback.nack_count,
             " pli_count=", feedback.pli_count,
             " fir_count=", feedback.fir_count);

    const auto update = quality_.OnFeedback(source_ssrc, feedback);
    return update.request_key_frame && track->getTrackType() == TrackVideo;
}

void ReceiveFeedbackHandler::Remove(uint32_t ssrc)
{
    quality_.Remove(ssrc);
}

void ReceiveFeedbackHandler::Clear()
{
    quality_.Clear();
}
}
