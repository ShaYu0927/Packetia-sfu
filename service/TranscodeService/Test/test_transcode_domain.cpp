#include "TranscodeTask.h"

#include <gtest/gtest.h>

namespace {
using namespace service::transcode;

TranscodeTaskConfig Config()
{
    TranscodeTaskConfig config;
    config.task_id = "camera";
    config.input.endpoint_id = 10;
    config.output = {20, "room", "camera"};
    return config;
}

TEST(TranscodeDomain, TaskOwnsSnapshotAndReservesSourceDespiteOptionalFilters)
{
    auto config = Config();
    const TranscodeTask task(config);
    config.task_id = "other";
    config.video.renditions.front().encoding.width = 640;
    EXPECT_EQ(task.Id(), "camera");
    EXPECT_EQ(task.Config().video.renditions.front().encoding.width, 1280);
    std::string error;
    EXPECT_TRUE(task.Validate(error)) << error; // Endpoint-local track zero is valid.
    config.input.ssrc = 99;
    config.input.session_id = "session";
    config.output.endpoint_id = 21;
    EXPECT_TRUE(task.ConflictsWith(TranscodeTask(config)));
    config.input.track_id = 1;
    EXPECT_FALSE(task.ConflictsWith(TranscodeTask(config)));
    config.output.endpoint_id = 20;
    EXPECT_TRUE(task.ConflictsWith(TranscodeTask(config)));
    config.output.endpoint_id = config.input.endpoint_id;
    EXPECT_FALSE(TranscodeTask(config).Validate(error));
}

TEST(TranscodeDomain, SourceMatchesIdentityAndOnlySpecifiedConstraints)
{
    const TranscodeSource actual{10, 0, "session", "camera", 99};
    TranscodeSource filter{10, 0};
    EXPECT_TRUE(filter.Matches(actual));
    filter.session_id = "session";
    filter.stream_id = "camera";
    filter.ssrc = 99;
    EXPECT_TRUE(filter.Matches(actual));
    auto other = actual;
    other.endpoint_id = 11;
    EXPECT_FALSE(filter.Matches(other));
    other = actual;
    other.track_id = 1;
    EXPECT_FALSE(filter.Matches(other));
    other = actual;
    other.session_id = "other";
    EXPECT_FALSE(filter.Matches(other));
    other = actual;
    other.stream_id = "other";
    EXPECT_FALSE(filter.Matches(other));
    other = actual;
    other.ssrc = 100;
    EXPECT_FALSE(filter.Matches(other));
}
} // namespace
