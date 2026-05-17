#ifndef _STREAM_CONTEXT_H_
#define _STREAM_CONTEXT_H_

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <vector>
#include "Sdp.h"

enum class StreamTrackType
{
    Unknown,
    Audio,
    Video
};

enum class CodecType
{
    Unknown,
    H264,
    H265,
    AAC,
    OPUS,
    PCMU,
    PCMA
};

struct PayloadTypeInfo
{
    uint8_t payload_type = 0;

    StreamTrackType track_type = StreamTrackType::Unknown;
    CodecType codec_type = CodecType::Unknown;

    int sample_rate = 0;
    int channels = 0;

    std::string codec_name;
    std::string fmtp;
};

struct StreamTrackInfo
{
    int media_index = -1;

    StreamTrackType track_type = StreamTrackType::Unknown;
    CodecType codec_type = CodecType::Unknown;

    uint8_t payload_type = 0;
    int sample_rate = 0;
    int channels = 0;

    std::string codec_name;
    std::string control;  // a=control:trackID=0
    std::string fmtp;

    // RTSP over TCP interleaved channel
    int rtp_channel = -1;
    int rtcp_channel = -1;
    std::vector<PayloadTypeInfo>  payloads;
};

class StreamContext
{
public:
    std::string stream_id;
    std::string url;

    // SDP 里解析出来的 m=audio / m=video
    std::vector<StreamTrackInfo> tracks;

    // 当前这一路流自己的 PT 映射，不能全局唯一
    std::unordered_map<uint8_t, PayloadTypeInfo> payload_type_map;

    // RTSP TCP interleaved channel -> media_index
    std::unordered_map<uint8_t, int> channel_to_media_index;
};


class StreamContextBuilder
{
public:
    /**
    * @brief Build a runtime stream context from an SDP session.
    *
    * Parses media descriptions from the SDP session and converts them into
    * runtime track information, including payload type mappings, codec type,
    * sample rate, channel count, FMTP parameters, and track metadata.
    *
    * The generated StreamContext is bound to one stream/source, so its payload
    * type mapping should only be used for this specific stream.
    *
    * @param session Parsed SDP session information.
    * @param stream_id Unique stream identifier.
    * @param url Original stream URL or source URL.
    * @return A shared pointer to the generated StreamContext.
    */
    static std::shared_ptr<StreamContext> BuildFromSdp(const sdp::SdpSession& session, const std::string& stream_id, const std::string& url);

    /**
    * @brief Convert SDP media type to internal track type.
    *
    * Converts media names from SDP, such as "audio" or "video",
    * into the internal TrackType enum.
    *
    * @param media_type SDP media type string from the m= line.
    * @return Corresponding TrackType. Returns TrackType::Unknown if unsupported.
    */
    static StreamTrackType ParseTrackType(const std::string& media_type);

    /**
    * @brief Convert SDP codec name to internal codec type.
    *
    * Converts codec names from SDP rtpmap attributes, such as "H264",
    * "MPEG4-GENERIC", "OPUS", "PCMU", or "PCMA", into the internal CodecType enum.
    *
    * @param codec Codec name parsed from a=rtpmap.
    * @return Corresponding CodecType. Returns CodecType::Unknown if unsupported.
    */
    static CodecType ParseCodecType(const std::string& codec);

    /**
    * @brief Fill payload type information from SDP rtpmap attributes.
    *
    * Searches the given SDP media section for the rtpmap entry matching the
    * specified payload type, then fills codec name, codec type, sample rate,
    * and channel count into PayloadTypeInfo.
    *
    * @param media SDP media section containing rtpmap attributes.
    * @param pt RTP payload type to look up.
    * @param info Payload type information to be updated.
    */
    static void FillRtpMap(const sdp::SdpMedia& media, uint8_t pt, PayloadTypeInfo& info);

    /**
    * @brief Fill payload type information from SDP fmtp attributes.
    *
    * Searches the given SDP media section for the fmtp entry matching the
    * specified payload type, then fills codec-specific parameters into
    * PayloadTypeInfo.
    *
    * For example, H264 may contain packetization-mode, profile-level-id,
    * and sprop-parameter-sets. AAC may contain mode, config, and related
    * MPEG4-GENERIC parameters.
    *
    * @param media SDP media section containing fmtp attributes.
    * @param pt RTP payload type to look up.
    * @param info Payload type information to be updated.
    */
    static void FillFmtp(const sdp::SdpMedia& media, uint8_t pt, PayloadTypeInfo& info);

    /**
    * @brief Parse SDP fmt payload type string into integer value.
    *
    * Converts payload type strings from the SDP m= line into an integer RTP
    * payload type value.
    *
    * @param fmt Payload type string from SdpMedia::fmts.
    * @param pt Output parameter used to store the parsed payload type.
    * @return true if parsing succeeds, false otherwise.
    */
    static bool ParsePayloadType(const std::string& fmt, int& pt);

};

#endif /* _STREAM_CONTEXT_H_ */