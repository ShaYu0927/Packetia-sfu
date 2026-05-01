#ifndef _TRACKINFO_H_
#define _TRACKINFO_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace media
{
    enum class TrackType
    {
        Unknown = 0,
        Audio,
        Video,
    };

    enum class TrackSource
    {
        Unknown = 0,
        Camera,
        Microphone,
        ScreenShare,
        ScreenShareAudio,
    };

    enum class EncryptionType
    {
        None = 0,
        Gcm,
        Custom,
    };

    enum class AudioTrackFeature
    {
        Unknown = 0,
        Stereo,
        Dtx,
        Red,
    };

    enum class BackupCodecPolicy
    {
        None = 0,
        PreferPrimary,
        PreferBackup,
    };

    struct TimedVersion
    {
        uint32_t version = 0;
        int64_t timestampMs = 0;
    };

    struct VideoLayer
    {
        int quality = 0;

        uint32_t width = 0;
        uint32_t height = 0;

        uint32_t bitrate = 0;

        /**
        * RID，例如 low/mid/high 或 q/h/f。
        */
        std::string rid;
    };

    struct CodecInfo
    {
        /**
        * 例如:
        * audio/opus
        * video/H264
        * video/VP8
        * video/AV1
        */
        std::string mimeType;

        /**
        * SDP 里的 mid。
        */
        std::string mid;

        /**
        * 客户端 Track ID / SDP CID。
        */
        std::string cid;

        std::vector<VideoLayer> layers;
    };


    struct TrackInfo
    {
        /**
        * 服务端生成的 Track 唯一 ID。
        * 例如 TR_xxx。
        */
        std::string sid;

        /**
        * 音频 / 视频。
        */
        TrackType type = TrackType::Unknown;

        /**
        * Track 名称。
        * 例如 camera / microphone / screen_share。
        */
        std::string name;

        /**
        * 是否静音 / 暂停。
        */
        bool muted = false;

        /**
        * 视频原始宽高。
        * 音频可以保持 0。
        */
        uint32_t width = 0;
        uint32_t height = 0;

        /**
        * 媒体来源。
        * 例如 Camera / Microphone / ScreenShare。
        */
        TrackSource source = TrackSource::Unknown;

        /**
        * 简单 codec 描述。
        * 例如 audio/opus, video/H264。
        */
        std::string mimeType;

        /**
        * SDP media id。
        */
        std::string mid;

        /**
        * 更详细的 codec / simulcast / layer 描述。
        */
        std::vector<CodecInfo> codecs;

        /**
        * 是否禁用 Audio RED。
        */
        bool disableRed = false;

        /**
        * Track 加密类型。
        */
        EncryptionType encryption = EncryptionType::None;

        /**
        * 所属 stream。
        * 例如 camera / screen。
        */
        std::string stream;

        /**
        * Track 信息版本。
        */
        TimedVersion version;

        /**
        * 音频特性。
        */
        std::vector<AudioTrackFeature> audioFeatures;

        /**
        * 备用 codec 策略。
        */
        BackupCodecPolicy backupCodecPolicy = BackupCodecPolicy::None;
    };

class MediaTrack
{
public:
    explicit MediaTrack(TrackInfo info)
        : info_(std::move(info))
    {
    }

    const std::string& id() const { return info_.sid;}

    TrackType type() const { return info_.type;}

    TrackSource source() const { return info_.source;}

    std::string mimeType() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.mimeType;
    }

    bool muted() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.muted;
    }

    void setMuted(bool muted)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.muted = muted;
        ++info_.version.version;
    }

    TrackInfo info() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }

    void updateInfo(const TrackInfo& info)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_ = info;
    }

private:
    mutable std::mutex mutex_;
    TrackInfo info_;
};

using MediaTrackPtr = std::shared_ptr<MediaTrack>;

}


#endif /* _TRACKINFO_H_ */