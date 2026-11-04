#include <gtest/gtest.h>

#include "EncodedFrameDispatcher.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace
{
class TestPublisher final : public media::IEncodedFramePublisher
{
public:
    size_t Publish(const media::EncodedFrameEvent& event) override
    {
        last = event;
        if (on_publish) on_publish();
        return 1;
    }
    media::EncodedFrameEvent last;
    std::function<void()> on_publish;
};

struct DestructionCallback
{
    explicit DestructionCallback(std::function<void()> callback) : callback(std::move(callback)) {}
    ~DestructionCallback() { callback(); }
    std::function<void()> callback;
};

TEST(EncodedFrameDispatcher, FramePublisherReentryPreservesCallbackSnapshotAndSourceIdentity)
{
    auto publisher = std::make_shared<TestPublisher>();
    media::EncodedFrameSource source;
    source.endpoint_id = 999;
    source.media_stream_id = 17;
    source.session_id = "meeting";
    source.stream_id = "camera";
    media::EncodedFrameDispatcher frames(123, source, publisher);
    unsigned old_calls = 0, new_calls = 0;
    const auto id = frames.AddCallback([&](const auto&) { ++old_calls; });
    publisher->on_publish = [&] {
        frames.RemoveCallback(id);
        frames.AddCallback([&](const auto&) { ++new_calls; });
    };
    auto frame = std::make_shared<media::EncodedFrame>();
    frame->info.track_id = 5;
    frame->info.media_type = media::MediaType::Video;
    frame->info.codec = media::CodecType::H264;
    frame->rtp.ssrc = 77;
    frame->buffer = common::SharedBuffer::Take(std::vector<uint8_t>{0x65, 1});
    frames.Dispatch(frame);
    EXPECT_EQ(old_calls, 1U);
    EXPECT_EQ(new_calls, 0U);
    publisher->on_publish = {};
    frames.Dispatch(frame);
    EXPECT_EQ(old_calls, 1U);
    EXPECT_EQ(new_calls, 1U);
    EXPECT_EQ(publisher->last.frame.get(), frame.get());
    EXPECT_EQ(publisher->last.source.endpoint_id, 123U);
    EXPECT_EQ(publisher->last.source.media_stream_id, 17U);
    EXPECT_EQ(publisher->last.source.track_id, 5U);
    EXPECT_EQ(publisher->last.source.ssrc, 77U);
    EXPECT_EQ(publisher->last.source.session_id, "meeting");
    EXPECT_EQ(publisher->last.source.stream_id, "camera");
}

TEST(EncodedFrameDispatcher, RemovedFrameCallbackCanReenterFromItsCaptureDestructor)
{
    media::EncodedFrameDispatcher frames(123, {}, {});
    unsigned destroyed = 0;
    auto marker = std::make_shared<DestructionCallback>([&] {
        ++destroyed;
        const auto next = frames.AddCallback([](const auto&) {});
        frames.RemoveCallback(next);
    });
    const auto id = frames.AddCallback([marker](const auto&) {});
    marker.reset();
    frames.RemoveCallback(id);
    EXPECT_EQ(destroyed, 1U);
    frames.RemoveCallback(id);
    EXPECT_EQ(destroyed, 1U);
}

}
