#include "Mp4Recorder.h"
#include "LocalFileIO.h"
#include "logger.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

#include "TimeUtil.h"

namespace service 
{
namespace 
{
std::string RecordingFileName(uint64_t sequence)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    std::ostringstream name;
    name << std::put_time(&local, "%Y-%m-%d-%H-%M-%S") << '-' << sequence << ".mp4";
    return name.str();
}
}

void Mp4Recorder::DiscardPending() 
{
    auto& recording = segment_;
    context_.pending_bytes -= recording.pending_bytes;
    recording.pending_bytes = 0;
    recording.pending.clear();
}

void Mp4Recorder::Close() 
{
    auto& recording = segment_;
    if (recording.writer_) 
    {
        recording.state_ = RecordingSegmentState::Finalizing;
        if (!recording.writer_->Close()) 
        {
            const auto error = recording.writer_->Error();

            recording.state_ = RecordingSegmentState::Failed;
            recording.failed_ = true;
            ++context_.errors;
            LOG_ERROR("[RECORD] finalize failed, path=", recording.path, " error=", recording.writer_->Error());
        
            if(!recording.terminal_event_sent)
            {
                recording.terminal_event_sent = true;
                EmitEvent(RecordingEventType::SegmentFailed, RecordingSessionState::Failed, error);
            }
            recording.writer_.reset();
            DiscardPending();
            return;
        } 
        recording.writer_.reset();

        std::error_code error;
        std::filesystem::rename(recording.tmp_path, recording.path, error);

        if (error)
        {
            recording.state_ = RecordingSegmentState::Failed;
            recording.failed_ = true;
            ++context_.errors;

             if (!recording.terminal_event_sent)
            {
                recording.terminal_event_sent = true;
                EmitEvent(RecordingEventType::SegmentFailed, RecordingSessionState::Failed, error.message());
            }

            DiscardPending();
            return;
        }

        recording.state_ = RecordingSegmentState::Completed;
        if(recording.frames > 0)
        {
            ++context_.completed;
            if (!recording.terminal_event_sent)
            {
                recording.terminal_event_sent = true;
                EmitEvent(RecordingEventType::SegmentCompleted, RecordingSessionState::Recording);
            }
        }
    }
    DiscardPending();
}

void Mp4Recorder::Fail(const std::string& message) 
{
    auto& recording = segment_;
    ++context_.errors;
    LOG_ERROR("[RECORD] stream stopped, path=", recording.path, " error=", message);
    if (!recording.terminal_event_sent)
    {
        recording.terminal_event_sent = true;
        EmitEvent(RecordingEventType::SegmentFailed, RecordingSessionState::Failed, message);
    }
    Close();
    recording.failed_ = true;
    recording.state_ = RecordingSegmentState::Failed;
}

void Mp4Recorder::Write(const media::EncodedFrameEvent& event) {
    auto& recording = segment_;
    auto& clock = recording.tracks.at(event.source.endpoint_id);
    const auto& frame = *event.frame;
    const auto& first = *clock.first.frame;
    if (frame.info.codec != first.info.codec || frame.rtp.ssrc != first.rtp.ssrc ||
        frame.info.timestamp.time_base_num != first.info.timestamp.time_base_num ||
        frame.info.timestamp.time_base_den != first.info.timestamp.time_base_den ||
        frame.sample_rate != first.sample_rate || frame.channels != first.channels ||
        (frame.codec_config && first.codec_config && *frame.codec_config != *first.codec_config) ||
        (frame.info.media_type == media::MediaType::Video &&
         (frame.video.width != first.video.width || frame.video.height != first.video.height))) {
        Fail("track parameters changed; restart publishing to begin a new recording");
        return;
    }
    const uint32_t current = static_cast<uint32_t>(frame.info.timestamp.dts);
    const int32_t delta = static_cast<int32_t>(current - clock.previous);
    if (delta < 0) {
        Fail("RTP timestamp moved backwards; use H264 without B frames (-bf 0)");
        return;
    }
    clock.ticks += delta;
    clock.previous = current;
    const int64_t timestamp = clock.anchor_us - recording.origin_us +
        clock.ticks * 1000000LL * frame.info.timestamp.time_base_num / frame.info.timestamp.time_base_den;
    if (timestamp < 0) { ++context_.dropped; return; }
    if (!recording.writer_->Write(event, timestamp)) {
        const auto error = recording.writer_->Error();
        Fail(error);
        return;
    }
    ++recording.frames;
    ++context_.written;
}

void Mp4Recorder::Open() 
{
    auto& recording = segment_;
    if (recording.failed_ || recording.writer_ || recording.pending.empty()) return;
    bool video_ready = false, use_capture = true;
    for (const auto& entry : recording.tracks) 
    {
        video_ready |= entry.second.first.frame->info.media_type == media::MediaType::Video;
        use_capture &= entry.second.first.frame->info.timestamp.capture_time_valid;
    }
    if (recording.video_seen && !video_ready) return;
    std::vector<media::EncodedFrameEvent> formats;
    int64_t earliest = INT64_MAX, video_origin = INT64_MAX;
    for (auto& entry : recording.tracks) 
    {
        auto& clock = entry.second;
        const auto& frame = *clock.first.frame;
        clock.anchor_us = use_capture ? frame.info.timestamp.capture_time_ms * 1000 :
            frame.info.timestamp.receive_time_ms > 0 ? frame.info.timestamp.receive_time_ms * 1000 :
            static_cast<int64_t>(recording.first_ms) * 1000;
        earliest = std::min(earliest, clock.anchor_us);
        if (frame.info.media_type == media::MediaType::Video) video_origin = std::min(video_origin, clock.anchor_us);
        formats.push_back(clock.first);
    }
    recording.origin_us = video_ready ? video_origin : earliest;
    const auto name = RecordingFileName(++context_.sequence);
    recording.path = (std::filesystem::path(context_.options.directory) / name).string();
    recording.tmp_path = (std::filesystem::path(context_.options.directory) / ("." + name)).string();
    auto file = std::make_unique<LocalFileIO>();
    if (file->Open(recording.tmp_path) < 0) 
    {
        const auto error = file->Error();
        Fail("open recording file: " + error);
        return;
    }
    recording.writer_ = std::make_unique<Mp4Writer>();
    if (!recording.writer_->Open(std::move(file), formats)) 
    {
        const auto error = recording.writer_->Error();
        Fail(error);
        return;
    }
    recording.state_ = RecordingSegmentState::Writing;
    LOG_INFO("[RECORD] opened, path=", recording.path, " tracks=", formats.size()," clock=", use_capture ? "RTCP-SR" : "receive-time anchor");
    if (!recording.start_event_sent)
    {
        recording.start_event_sent = true;
        EmitEvent(RecordingEventType::SegmentStarted, RecordingSessionState::Recording);
    }
    
    auto pending = std::move(recording.pending);
    DiscardPending();
    for (const auto& frame : pending) 
    {
        if (recording.failed_) break;
        Write(frame);
    }
}

void Mp4Recorder::InputFrame(const media::EncodedFrameEvent& event, uint64_t now) 
{
    auto& recording = segment_;
    if (!recording.first_ms) 
    {
        recording.first_ms = now;
        recording.session_id = event.source.session_id;
        recording.stream_id = event.source.stream_id;
    }
    recording.last_ms = now;
    const bool video = event.frame->info.media_type == media::MediaType::Video;
    recording.video_seen |= video;
    if (!recording.failed_ && recording.writer_ &&
        (!recording.tracks.count(event.source.endpoint_id) ||
         (context_.options.segment_ms && now - recording.first_ms >= context_.options.segment_ms &&
          (video ? event.frame->IsKeyFrame() : !recording.video_seen)))) 
    {
        const bool video_seen = recording.video_seen;
        const auto session_id = recording.session_id;
        const auto stream_id = recording.stream_id;
        Close();
        recording = RecordingSegment{};
        recording.first_ms = recording.last_ms = now;
        recording.video_seen = video_seen;
        recording.session_id = session_id;
        recording.stream_id = stream_id;
    }
    if (recording.failed_) ++context_.dropped;
    else if (recording.writer_)
    {
        if (!event.frame->IsConfigFrame()) Write(event);
    } 
    else 
    {
        auto track = recording.tracks.find(event.source.endpoint_id);
        if (track == recording.tracks.end() && Mp4Writer::Ready(*event.frame)) 
        {
            RecordingSegment::Clock clock;
            clock.first = event;
            clock.previous = static_cast<uint32_t>(event.frame->info.timestamp.dts);
            recording.tracks.emplace(event.source.endpoint_id, std::move(clock));
            track = recording.tracks.find(event.source.endpoint_id);
            LOG_INFO("[RECORD] track ready, session=", recording.session_id,
                     " stream=", recording.stream_id,
                     " endpoint=", event.source.endpoint_id,
                     " type=", event.frame->info.media_type == media::MediaType::Video ? "video" : "audio");
        }
        if (track != recording.tracks.end() && !event.frame->IsConfigFrame()) 
        {
            if (!context_.ReservePending(event.frame->size))
                Fail("codec discovery buffer limit reached");
            else 
            {
                try 
                {
                    recording.pending.push_back(event);
                } 
                catch (...) 
                {
                    context_.pending_bytes -= event.frame->size;
                    throw;
                }
                recording.pending_bytes += event.frame->size;
            }
        } else ++context_.dropped;
    }
}

bool Mp4Recorder::Tick(uint64_t now, bool stopping) 
{
    auto& recording = segment_;
    const bool idle = now - recording.last_ms >= context_.options.idle_timeout_ms;
    if (stopping || idle || now - recording.first_ms >= context_.options.discovery_ms) Open();
    if (stopping || idle) {
        if (!recording.writer_ && !recording.failed_) {
            ++context_.errors;
            LOG_ERROR("[RECORD] no playable segment: missing keyframe/SPS/PPS or AAC config");
        }
        Close();
        return true;
    }
    return false;
}

Mp4Recorder::~Mp4Recorder() 
{
    
    DiscardPending();
}

bool Mp4Recorder::HasReadyVideoTrack() const
{
    for(const auto& entry : segment_.tracks)
    {
        const auto& event = entry.second.first;
        if(event.frame && event.frame->info.media_type == media::MediaType::Video)
        {
            return true;
        }
    }
    return false;
}

bool Mp4Recorder::CanOpen(uint64_t now, bool force) const
{
    if (segment_.failed_ || segment_.writer_)
    {
        return false;
    }

    if (segment_.pending.empty() || segment_.tracks.empty())
    {
        return false;
    }

    if (segment_.video_seen && !HasReadyVideoTrack())
    {
        return false;
    }

    if (!force && now - segment_.first_ms < context_.options.discovery_ms)
    {
        return false;
    }

    return true;
}


void Mp4Recorder::EmitEvent(RecordingEventType type, RecordingSessionState state, const std::string& error)
{
    if (!context_.event_sink)
        return;

    try
    {
        context_.event_sink->OnRecordingEvent(
            RecordingEvent{
                type,
                instance_,
                state,
                RecordingStopReason::None,
                Timestamp::NowMs(),
                segment_.path,
                error
            });
    }
    catch (const std::exception& exception)
    {
        LOG_ERROR("[RECORD] segment event sink failed, path=", segment_.path, " error=", exception.what());
    }
    catch (...)
    {
        LOG_ERROR("[RECORD] segment event sink failed, path=", segment_.path, " error=unknown");
    }
}

}
