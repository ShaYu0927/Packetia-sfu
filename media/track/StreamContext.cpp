#include "StreamContext.h"
#include "logger.h"


void StreamContextBuilder::FillRtpMap(const sdp::SdpMedia& media, uint8_t pt, PayloadTypeInfo& info)
{
    for (const auto& rtpmap : media.rtpmaps)
    {
        
        if (rtpmap.payloadType != static_cast<int>(pt))
        {
            continue;
        }

        info.codec_name = rtpmap.encodingName;
        info.codec_type = ParseCodecType(rtpmap.encodingName);
        info.sample_rate = rtpmap.clockRate;
        info.channels = rtpmap.channels;


        if (info.track_type == StreamTrackType::Audio && info.channels <= 0)
        {
            info.channels = 1;
        }

        if (info.track_type == StreamTrackType::Video && info.sample_rate <= 0)
        {
            info.sample_rate = 90000;
        }

        return;
    }

    if (info.track_type == StreamTrackType::Audio && pt == 0)
    {
        info.codec_name = "PCMU";
        info.codec_type = CodecType::PCMU;
        info.sample_rate = 8000;
        info.channels = 1;
        return;
    }

    if (info.track_type == StreamTrackType::Audio && pt == 8)
    {
        info.codec_name = "PCMA";
        info.codec_type = CodecType::PCMA;
        info.sample_rate = 8000;
        info.channels = 1;
        return;
    }

    LOG_INFO("[SDP] rtpmap not found", " media=", media.media, " pt=", static_cast<int>(pt));
}


void StreamContextBuilder::FillFmtp(const sdp::SdpMedia& media, uint8_t pt, PayloadTypeInfo& info)
{
    for (const auto& fmtp : media.fmtps)
    {
        if (fmtp.payloadType != static_cast<int>(pt))
        {
            continue;
        }

        info.fmtp = fmtp.params;
        return;
    }

    LOG_INFO("[SDP] fmtp not found", " media=", media.media, " pt=", static_cast<int>(pt));
}

std::shared_ptr<StreamContext> StreamContextBuilder::BuildFromSdp(const sdp::SdpSession& session, const std::string& stream_id, const std::string& url)
{
    auto ctx = std::make_shared<StreamContext>();
    ctx->stream_id = stream_id;
    ctx->url = url;


    for (size_t i = 0; i < session.medias.size(); ++i)
    {
        const sdp::SdpMedia& media = session.medias[i];
        
        StreamTrackInfo track;
        track.media_index       = static_cast<int>(i);
        track.track_type        = ParseTrackType(media.media);
        track.control           = media.GetAttribute("control");

        for (const std::string& fmt : media.fmts)
        {
            int pt_int = 0;
            if (!ParsePayloadType(fmt, pt_int))
            {
                LOG_ERROR("[SDP] invalid payload type fmt=", fmt, " media_index=", track.media_index);
                continue;
            }

            if (pt_int < 0 || pt_int > 127)
            {
                LOG_ERROR("[SDP] payload type out of range pt=", pt_int, " media_index=", track.media_index);
                continue;
            }

            uint8_t pt = static_cast<uint8_t>(pt_int);

            PayloadTypeInfo pt_info;
            pt_info.payload_type = pt;
            pt_info.track_type = track.track_type;

            FillRtpMap(media, pt, pt_info);
            FillFmtp(media, pt, pt_info);

            track.payloads.push_back(pt_info);
            ctx->payload_type_map[pt] = pt_info;
        }

        ctx->tracks.push_back(std::move(track));
    }

    return ctx;
}


StreamTrackType StreamContextBuilder::ParseTrackType(const std::string& media_type)
{
    if (media_type == "video")
    {
        return StreamTrackType::Video;
    }

    if (media_type == "audio")
    {
        return StreamTrackType::Audio;
    }

    return StreamTrackType::Unknown;
}

CodecType StreamContextBuilder::ParseCodecType(const std::string& codec)
{
    if (codec == "H264" || codec == "h264")
    {
        return CodecType::H264;
    }

    if (codec == "H265" || codec == "HEVC" || codec == "h265" || codec == "hevc")
    {
        return CodecType::H265;
    }

    if (codec == "MPEG4-GENERIC" || codec == "mpeg4-generic")
    {
        return CodecType::AAC;
    }

    if (codec == "OPUS" || codec == "opus")
    {
        return CodecType::OPUS;
    }

    if (codec == "PCMU")
    {
        return CodecType::PCMU;
    }

    if (codec == "PCMA")
    {
        return CodecType::PCMA;
    }

    return CodecType::Unknown;
}


bool StreamContextBuilder::ParsePayloadType(const std::string& fmt, int& pt)
{
    try
    {
        pt = std::stoi(fmt);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
