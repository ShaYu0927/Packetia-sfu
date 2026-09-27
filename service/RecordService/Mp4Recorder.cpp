#include "Mp4Recorder.h"
#include "LocalFileIO.h"
#include "RecordingCatalog.h"
#include "logger.h"
#include "TimeUtil.h"
#include <algorithm>
#include <filesystem>

namespace service {
namespace {
int64_t DurationUs(const media::EncodedFrame& frame) {
    if (frame.info.media_type == media::MediaType::Audio && frame.sample_rate && frame.sample_count)
        return static_cast<int64_t>(frame.sample_count) * 1000000 / frame.sample_rate;
    return 40000; // Last video packet fallback, matching Mp4Writer.
}
bool SameFormat(const media::EncodedFrame& a, const media::EncodedFrame& b) {
    return a.info.codec == b.info.codec && a.rtp.ssrc == b.rtp.ssrc &&
        a.info.timestamp.time_base_num == b.info.timestamp.time_base_num &&
        a.info.timestamp.time_base_den == b.info.timestamp.time_base_den &&
        a.sample_rate == b.sample_rate && a.channels == b.channels &&
        !(a.codec_config && b.codec_config && a.codec_config != b.codec_config) &&
        (a.info.media_type != media::MediaType::Video ||
         (a.video.width == b.video.width && a.video.height == b.video.height));
}
}

void Mp4Recorder::DiscardPending() {
    context_.pending_bytes -= segment_.pending_bytes;
    segment_.pending_bytes = 0;
    pending_.clear();
}

void Mp4Recorder::Store() {
    if (context_.catalog) context_.catalog->Store(info_);
}

bool Mp4Recorder::OpenSegment(int64_t origin_us) {
    if (segment_.failed_) return false;
    info_ = {};
    info_.recording_id = std::to_string(instance_.run_id) + "-" + std::to_string(instance_.generation);
    info_.sequence = ++segment_sequence_;
    info_.segment_id = info_.recording_id + "-" + std::to_string(info_.sequence);
    info_.session_id = instance_.session.session_id;
    info_.stream_id = instance_.session.stream_id;
    info_.relative_path = info_.segment_id + ".mp4";
    info_.media_start_us = origin_us - recording_origin_us_;
    info_.media_end_us = info_.media_start_us;
    info_.started_at_ms = wall_anchor_ms_ + (origin_us - media_anchor_us_) / 1000;
    segment_.origin_us = origin_us;
    segment_.frames = 0;
    written_tracks_.clear();
    segment_.terminal_event_sent = false;
    segment_.path = (std::filesystem::path(context_.options.directory) / info_.relative_path).string();
    segment_.tmp_path = (std::filesystem::path(context_.options.directory) / ("." + info_.relative_path)).string();
    std::vector<media::EncodedFrameEvent> formats;
    for (const auto& entry : segment_.tracks) {
        const auto& event = entry.second.first;
        const auto& f = *event.frame;
        formats.push_back(event);
        info_.tracks.push_back({event.source.endpoint_id, event.source.track_id,
            f.info.codec == media::CodecType::H264 ? "h264" : "aac",
            f.video.width, f.video.height, static_cast<int>(f.sample_rate), static_cast<int>(f.channels)});
    }
    try {
        // Write intent before creating a file. Interrupted Writing records are
        // never exposed as playable. Opaque generated names contain no user input.
        Store();
        if (std::filesystem::exists(segment_.path) || std::filesystem::exists(segment_.tmp_path))
            throw std::runtime_error("recording filename collision");
        auto file = std::make_unique<LocalFileIO>();
        if (file->Open(segment_.tmp_path) < 0) throw std::runtime_error(file->Error());
        segment_.writer_ = std::make_unique<Mp4Writer>();
        if (!segment_.writer_->Open(std::move(file), formats)) throw std::runtime_error(segment_.writer_->Error());
        segment_.state_ = RecordingSegmentState::Writing;
        EmitEvent(RecordingEventType::SegmentStarted, RecordingSessionState::Recording);
        return true;
    } catch (const std::exception& error) {
        Fail(std::string("open segment: ") + error.what());
        return false;
    }
}

bool Mp4Recorder::FinalizeSegment() {
    if (!segment_.writer_) return !segment_.failed_;
    info_.media_end_us = info_.media_start_us;
    for (const auto& track : written_tracks_)
        info_.media_end_us = std::max(info_.media_end_us,
            track.second.last_us - recording_origin_us_ + track.second.duration_us);
    segment_.state_ = RecordingSegmentState::Finalizing;
    if (!segment_.writer_->Close()) {
        const auto error = segment_.writer_->Error();
        segment_.writer_.reset();
        Fail("finalize segment: " + error);
        return false;
    }
    segment_.writer_.reset();
    try {
        std::filesystem::rename(segment_.tmp_path, segment_.path);
        info_.size_bytes = std::filesystem::file_size(segment_.path);
        info_.ended_at_ms = info_.started_at_ms + (info_.media_end_us - info_.media_start_us + 999) / 1000;
        info_.status = SegmentStatus::Completed;
        Store(); // Publish completion only after both file and index succeed.
        segment_.state_ = RecordingSegmentState::Completed;
        segment_.terminal_event_sent = true;
        ++context_.completed;
        EmitEvent(RecordingEventType::SegmentCompleted, RecordingSessionState::Recording);
        return true;
    } catch (const std::exception& error) {
        Fail(std::string("publish segment: ") + error.what());
        return false;
    }
}

void Mp4Recorder::Fail(const std::string& message) {
    if (segment_.failed_) return;
    segment_.failed_ = true;
    segment_.state_ = RecordingSegmentState::Failed;
    ++context_.errors;
    LOG_ERROR("[RECORD] stream failed, session=", instance_.session.session_id,
              " stream=", instance_.session.stream_id, " error=", message);
    if (segment_.writer_) { segment_.writer_->Close(); segment_.writer_.reset(); }
    if (info_.segment_id.empty()) {
        info_.recording_id = std::to_string(instance_.run_id) + "-" + std::to_string(instance_.generation);
        info_.sequence = ++segment_sequence_;
        info_.segment_id = info_.recording_id + "-" + std::to_string(info_.sequence);
        info_.session_id = instance_.session.session_id;
        info_.stream_id = instance_.session.stream_id;
        info_.started_at_ms = static_cast<int64_t>(Timestamp::WallNowMs());
    }
    // Retain failed temporary files for diagnosis; never publish as Completed.
    if (!info_.segment_id.empty()) {
        info_.status = SegmentStatus::Failed;
        info_.error = message;
        info_.ended_at_ms = std::max(info_.started_at_ms + 1, static_cast<int64_t>(Timestamp::WallNowMs()));
        try { Store(); } catch (const std::exception& error) {
            LOG_ERROR("[RECORD] failed to persist segment failure: ", error.what());
        }
    }
    if (!segment_.terminal_event_sent) {
        segment_.terminal_event_sent = true;
        EmitEvent(RecordingEventType::SegmentFailed, RecordingSessionState::Failed, message);
    }
    DiscardPending();
}

void Mp4Recorder::Open() {
    if (discovered_ || segment_.failed_ || pending_.empty()) return;
    bool video_ready = false;
    bool use_capture = true;
    for (const auto& entry : segment_.tracks) {
        const bool video = entry.second.first.frame->info.media_type == media::MediaType::Video;
        video_ready |= video;
        use_capture &= entry.second.first.frame->info.timestamp.capture_time_valid;
    }
    if (segment_.video_seen && !video_ready) return;
    // Choose one clock domain for ALL tracks after discovery. Mixing an RTCP
    // capture clock on video with a monotonic receive clock on audio corrupts
    // both interleaving and segment boundaries.
    if (use_capture) {
        decltype(pending_) aligned;
        for (auto& entry : pending_) {
            auto packet = entry.second;
            const auto& clock = segment_.tracks.at({packet.event.source.endpoint_id, packet.event.source.track_id});
            packet.time_us += clock.first.frame->info.timestamp.capture_time_ms * 1000 - clock.anchor_us;
            aligned.emplace(std::make_pair(packet.time_us, entry.first.second), std::move(packet));
        }
        pending_.swap(aligned);
        for (auto& entry : segment_.tracks)
            entry.second.anchor_us = entry.second.first.frame->info.timestamp.capture_time_ms * 1000;
        latest_us_ = pending_.rbegin()->first.first;
    }
    int64_t origin = INT64_MAX;
    media_anchor_us_ = INT64_MAX;
    for (const auto& entry : segment_.tracks) {
        media_anchor_us_ = std::min(media_anchor_us_, entry.second.anchor_us);
        if (entry.second.first.frame->info.media_type == media::MediaType::Video || !segment_.video_seen)
            origin = std::min(origin, entry.second.anchor_us);
    }
    recording_origin_us_ = origin;
    discovered_ = OpenSegment(origin);
}

void Mp4Recorder::Write(const PendingFrame& packet) {
    if (packet.time_us < segment_.origin_us) { ++context_.dropped; return; }
    if (!segment_.writer_->Write(packet.event, packet.time_us - segment_.origin_us)) {
        Fail(segment_.writer_->Error());
        return;
    }
    const RecordingTrackKey key{packet.event.source.endpoint_id, packet.event.source.track_id};
    auto found = written_tracks_.find(key);
    const auto& frame = *packet.event.frame;
    int64_t duration = DurationUs(frame);
    if (found != written_tracks_.end() && frame.info.media_type == media::MediaType::Video)
        duration = packet.time_us - found->second.last_us;
    written_tracks_[key] = {packet.time_us, duration};
    ++segment_.frames;
    ++context_.written;
}

void Mp4Recorder::Drain(uint64_t now, bool force) {
    if (!discovered_ || segment_.failed_) return;
    while (!pending_.empty()) {
        const auto it = pending_.begin();
        const auto& first = it->second;
        const auto window_us = static_cast<int64_t>(context_.options.reorder_ms) * 1000;
        if (!force && first.time_us > latest_us_ - window_us &&
            now - first.received_ms < context_.options.reorder_ms) break;
        auto packet = std::move(it->second);
        const auto order = it->first;
        const auto bytes = packet.event.frame->StorageSize();
        pending_.erase(it);
        segment_.pending_bytes -= bytes;
        context_.pending_bytes -= bytes;
        if (order < drained_key_) { ++context_.dropped; continue; }
        drained_key_ = order;
        const auto& frame = *packet.event.frame;
        const bool cut_point = segment_.video_seen
            ? frame.info.media_type == media::MediaType::Video && frame.IsKeyFrame()
            : frame.info.media_type == media::MediaType::Audio;
        if (segment_.frames && context_.options.segment_ms && cut_point &&
            packet.time_us - segment_.origin_us >= static_cast<int64_t>(context_.options.segment_ms) * 1000) {
            // Audio packets are indivisible. Their start timestamp selects the
            // segment; their duration may overlap the boundary by one packet.
            if (!FinalizeSegment() || !OpenSegment(packet.time_us)) return;
        }
        Write(packet);
        if (segment_.failed_) return;
    }
}

void Mp4Recorder::InputFrame(const media::EncodedFrameEvent& event, uint64_t now) {
    if (closed_ || segment_.failed_) { ++context_.dropped; return; }
    if (!segment_.first_ms) segment_.first_ms = now;
    segment_.last_ms = now;
    const auto& frame = *event.frame;
    const auto& timestamp = frame.info.timestamp;
    const bool video = frame.info.media_type == media::MediaType::Video;
    segment_.video_seen |= video;
    const RecordingTrackKey key{event.source.endpoint_id, event.source.track_id};
    auto found = segment_.tracks.find(key);
    if (found == segment_.tracks.end()) {
        if (discovered_) { Fail("track added after discovery; start a new recording instance"); return; }
        for (const auto& existing : segment_.tracks) {
            if (existing.second.first.frame->info.media_type == frame.info.media_type) {
                Fail("multiple tracks of the same type: each publisher needs a unique stream_id"); return;
            }
        }
        if (!Mp4Writer::Ready(frame)) { ++context_.dropped; return; }
        RecordingSegment::Clock clock;
        clock.first = event;
        clock.previous = static_cast<uint32_t>(timestamp.dts);
        clock.anchor_us = (timestamp.receive_time_ms > 0 ? timestamp.receive_time_ms : static_cast<int64_t>(now)) * 1000;
        found = segment_.tracks.emplace(key, std::move(clock)).first;
    } else if (!SameFormat(frame, *found->second.first.frame)) {
        Fail("track parameters changed; start a new recording instance"); return;
    }
    if (frame.IsConfigFrame()) return;
    auto& clock = found->second;
    const uint32_t current = static_cast<uint32_t>(timestamp.dts);
    const int32_t delta = static_cast<int32_t>(current - clock.previous);
    if (delta < 0 || timestamp.time_base_num <= 0 || timestamp.time_base_den <= 0) {
        Fail("invalid/backwards RTP clock (B frames are unsupported)"); return;
    }
    clock.ticks += delta;
    clock.previous = current;
    const int64_t time_us = clock.anchor_us + clock.ticks * 1000000LL * timestamp.time_base_num / timestamp.time_base_den;
    if (!clock_anchored_) {
        clock_anchored_ = true;
        media_anchor_us_ = time_us;
        wall_anchor_ms_ = static_cast<int64_t>(Timestamp::WallNowMs());
    }
    if (std::make_pair(time_us, video ? 0 : 1) < drained_key_) { ++context_.dropped; return; }
    const auto bytes = frame.StorageSize();
    if (!context_.ReservePending(bytes)) { Fail("recording reorder/discovery buffer limit reached"); return; }
    try { pending_.emplace(std::make_pair(time_us, video ? 0 : 1), PendingFrame{event, time_us, now}); }
    catch (...) { context_.pending_bytes -= bytes; throw; }
    segment_.pending_bytes += bytes;
    latest_us_ = std::max(latest_us_, time_us);
    if (now - segment_.first_ms >= context_.options.discovery_ms) Open();
    Drain(now, false);
}

bool Mp4Recorder::Tick(uint64_t now, bool stopping) {
    if (closed_) return true;
    const bool idle = now - segment_.last_ms >= context_.options.idle_timeout_ms;
    // Keep a failed generation until stop/idle instead of recreating one for
    // every queued delta frame and flooding the index with failed segments.
    if (segment_.failed_) return stopping || idle;
    if (stopping || idle || now - segment_.first_ms >= context_.options.discovery_ms) Open();
    Drain(now, stopping || idle);
    if (stopping || idle) { Close(); return true; }
    return false;
}

void Mp4Recorder::Close() {
    if (closed_) return;
    if (!segment_.failed_) {
        Open();
        Drain(segment_.last_ms, true);
        if (!discovered_ && !segment_.failed_) Fail("no playable segment: missing keyframe/SPS/PPS or AAC config");
        if (!segment_.failed_) FinalizeSegment();
    }
    DiscardPending();
    closed_ = true;
}

Mp4Recorder::~Mp4Recorder() {
    // Normal close is explicit on the owning worker. Unexpected destruction
    // leaves a non-playable Writing index and temporary file for reconciliation.
    DiscardPending();
}

void Mp4Recorder::EmitEvent(RecordingEventType type, RecordingSessionState state, const std::string& error) {
    if (!context_.event_sink) return;
    try {
        RecordingEvent event{type, instance_, state, RecordingStopReason::None,
            Timestamp::NowMs(), segment_.path, error};
        if (!info_.segment_id.empty()) event.segment = info_;
        context_.event_sink->OnRecordingEvent(event);
    } catch (...) { LOG_ERROR("[RECORD] segment notification failed, path=", segment_.path); }
}
} // namespace service
