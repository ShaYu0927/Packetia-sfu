#ifndef PACKETIA_MEDIA_TRACK_METADATA_H
#define PACKETIA_MEDIA_TRACK_METADATA_H

#include "MediaTypes.h"
#include <cstdint>
#include <string>
#include <vector>

// Value types describing media tracks. Independent of transports and runtime objects.
namespace media
{
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
        * RID，例如 low/mid/high 或 q/h/f
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
        * SDP 里的 mid
        */
        std::string mid;

        /**
        * 客户端 Track ID / SDP CID
        */
        std::string cid;

        std::vector<VideoLayer> layers;
    };


    struct TrackMetadata
    {
        /**
        * 服务端生成的 Track 唯一 ID
        * 例如 TR_xxx
        */
        std::string sid;

        /**
        * 音频 / 视频。
        */
        MediaType type = MediaType::Unknown;

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
}

#endif // PACKETIA_MEDIA_TRACK_METADATA_H
