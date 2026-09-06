#include "AudioDepacketizer.h"
#include "logger.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

namespace media
{
namespace
{

/*

收到 AAC RTP payload
        ↓
读取 AU-headers-length
        ↓
解析 AU Header，得到 au_sizes
        ↓
au_sizes 数量？
   ┌────┴──────────────┐
 多个 AU              一个 AU
   ↓                    ↓
验证长度总和       是否有活动分片？
   ↓              ┌────┴─────┐
逐个切分输出       有          无
                  ↓           ↓
           检查 SSRC/TS/seq   比较实际长度
                  ↓        ┌────┼────────┐
              追加数据      大于  等于    小于
                  ↓          ↓    ↓       ↓
             达到完整大小？ 失败  直接输出 创建分片
              ┌───┴───┐
             是       否
             ↓        ↓
           输出帧   marker=1？
                    ↓
                   丢弃
*/

class BitReader
{
public:
    BitReader(const uint8_t* data, size_t bit_count)
        : data_(data), bit_count_(bit_count)
    {
    }

    bool Read(size_t bits, uint32_t& value)
    {
        if (!data_ || bits > 32 || bit_offset_ + bits > bit_count_)
        {
            return false;
        }

        value = 0;
        for (size_t i = 0; i < bits; ++i)
        {
            const size_t pos = bit_offset_ + i;
            value = (value << 1) | ((data_[pos / 8] >> (7 - (pos % 8))) & 0x01U);
        }
        bit_offset_ += bits;
        return true;
    }

    size_t Remaining() const { return bit_count_ - bit_offset_; }

private:
    const uint8_t* data_ = nullptr;
    size_t bit_count_ = 0;
    size_t bit_offset_ = 0;
};

std::string Trim(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseUint(const std::string& text, uint32_t& value)
{
    try
    {
        const std::string normalized = Trim(text);
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(normalized, &consumed, 10);
        if (consumed != normalized.size() || parsed > std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        value = static_cast<uint32_t>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
}

bool AudioDepacketizer::Input(const RtpView& view)
{
    if (!view.valid() || !view.payload || view.payload_len == 0)
    {
        LOG_ERROR("[AudioDepacketizer] invalid rtp view",
                  " ssrc=", view.ssrc,
                  " seq=", view.seq,
                  " ts=", view.ts,
                  " payload_len=", view.payload_len);
        return false;
    }

    switch (codec_)
    {
    case CodecType::PCMU:
    case CodecType::PCMA:
    case CodecType::Opus:
        return InputSimplePayload(view);

    case CodecType::AAC:
        return InputAac(view);

    default:
        LOG_ERROR("[AudioDepacketizer] unsupported codec",
                  " seq=", view.seq,
                  " ts=", view.ts);
        return false;
    }
}

void AudioDepacketizer::ParseAacFmtp(const std::string& fmtp)
{
    std::istringstream input(fmtp);
    std::string item;
    while (std::getline(input, item, ';'))
    {
        const size_t equal = item.find('=');
        if (equal == std::string::npos)
        {
            continue;
        }

        const std::string key = Lower(Trim(item.substr(0, equal)));
        if (key == "config")
        {
            const auto hex = Trim(item.substr(equal + 1));
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            bool valid = !hex.empty() && hex.size() % 2 == 0 && hex.size() <= 128;
            for (size_t i = 0; valid && i < hex.size(); i += 2) {
                const int high = digit(hex[i]), low = digit(hex[i + 1]);
                valid = high >= 0 && low >= 0;
                if (valid) bytes->push_back(static_cast<uint8_t>((high << 4) | low));
            }
            codec_config_ = valid ? bytes : nullptr;
            continue;
        }
        uint32_t value = 0;
        if (!ParseUint(item.substr(equal + 1), value))
        {
            continue;
        }

        if (key == "sizelength" && value <= 32) aac_config_.size_length = static_cast<uint8_t>(value);
        else if (key == "indexlength" && value <= 32) aac_config_.index_length = static_cast<uint8_t>(value);
        else if (key == "indexdeltalength" && value <= 32) aac_config_.index_delta_length = static_cast<uint8_t>(value);
        else if (key == "ctsdeltalength" && value <= 32) aac_config_.cts_delta_length = static_cast<uint8_t>(value);
        else if (key == "dtsdeltalength" && value <= 32) aac_config_.dts_delta_length = static_cast<uint8_t>(value);
        else if (key == "randomaccessindication") aac_config_.random_access_indication = value != 0;
        else if (key == "streamstateindication" && value <= 32) aac_config_.stream_state_indication = static_cast<uint8_t>(value);
        else if (key == "constantsize") aac_config_.constant_size = value;
        else if (key == "constantduration" && value > 0) aac_config_.constant_duration = value;
    }
}

bool AudioDepacketizer::InputAac(const RtpView& view)
{
    if (view.payload_len < 2)
    {
        ResetAacFragment();
        return false;
    }

    const size_t header_bits = (static_cast<size_t>(view.payload[0]) << 8) | static_cast<size_t>(view.payload[1]);
    const size_t header_bytes = (header_bits + 7) / 8;
    if (2 + header_bytes > view.payload_len)
    {
        ResetAacFragment();
        return false;
    }

    const uint8_t* au_data = view.payload + 2 + header_bytes;
    const size_t au_data_size = view.payload_len - 2 - header_bytes;
    std::vector<uint32_t> au_sizes;

    if (header_bits == 0)
    {
        au_sizes.push_back(aac_config_.constant_size > 0 ? aac_config_.constant_size : static_cast<uint32_t>(au_data_size));
    }
    else
    {
        BitReader bits(view.payload + 2, header_bits);
        bool first = true;
        while (true)
        {
            const size_t index_bits = first ? aac_config_.index_length
                                            : aac_config_.index_delta_length;
            size_t required = aac_config_.size_length + index_bits;
            if (aac_config_.cts_delta_length > 0) required += 1;
            if (aac_config_.dts_delta_length > 0) required += 1;
            if (aac_config_.random_access_indication) required += 1;
            required += aac_config_.stream_state_indication;
            if (bits.Remaining() < required || aac_config_.size_length == 0)
            {
                break;
            }

            uint32_t au_size = 0;
            uint32_t ignored = 0;
            if (!bits.Read(aac_config_.size_length, au_size) ||
                (index_bits > 0 && !bits.Read(index_bits, ignored)))
            {
                ResetAacFragment();
                return false;
            }

            if (aac_config_.cts_delta_length > 0)
            {
                uint32_t present = 0;
                if (!bits.Read(1, present) ||
                    (present && !bits.Read(aac_config_.cts_delta_length, ignored))) return false;
            }
            if (aac_config_.dts_delta_length > 0)
            {
                uint32_t present = 0;
                if (!bits.Read(1, present) ||
                    (present && !bits.Read(aac_config_.dts_delta_length, ignored))) return false;
            }
            if (aac_config_.random_access_indication && !bits.Read(1, ignored)) return false;
            if (aac_config_.stream_state_indication > 0 &&
                !bits.Read(aac_config_.stream_state_indication, ignored)) return false;

            if (au_size == 0 || au_size > (1U << 20))
            {
                ResetAacFragment();
                return false;
            }
            au_sizes.push_back(au_size);
            first = false;
        }

        if (au_sizes.empty() || bits.Remaining() != 0)
        {
            ResetAacFragment();
            return false;
        }
    }

    if (au_sizes.size() > 1)
    {
        if (aac_fragment_.active)
        {
            ResetAacFragment();
            return false;
        }
        size_t total_size = 0;
        for (uint32_t size : au_sizes)
        {
            if (size > au_data_size - total_size)
            {
                return false;
            }
            total_size += size;
        }
        if (total_size != au_data_size)
        {
            return false;
        }

        size_t offset = 0;
        for (size_t i = 0; i < au_sizes.size(); ++i)
        {
            EmitAacFrame(au_data + offset,
                         au_sizes[i],
                         view.ts + static_cast<uint32_t>(i * aac_config_.constant_duration),
                         view.ssrc,
                         view.seq,
                         view.seq);
            offset += au_sizes[i];
        }
        return true;
    }

    const uint32_t expected_size = au_sizes.front();
    if (aac_fragment_.active)
    {
        const uint16_t expected_seq = static_cast<uint16_t>(aac_fragment_.last_seq + 1);
        if (view.ssrc != aac_fragment_.ssrc || view.ts != aac_fragment_.timestamp ||
            view.seq != expected_seq || expected_size != aac_fragment_.expected_size ||
            au_data_size > expected_size - aac_fragment_.data.size())
        {
            ResetAacFragment();
            return false;
        }

        aac_fragment_.data.insert(aac_fragment_.data.end(), au_data, au_data + au_data_size);
        aac_fragment_.last_seq = view.seq;
        if (aac_fragment_.data.size() == expected_size)
        {
            EmitAacFrame(aac_fragment_.data.data(),
                         aac_fragment_.data.size(),
                         aac_fragment_.timestamp,
                         aac_fragment_.ssrc,
                         aac_fragment_.first_seq,
                         aac_fragment_.last_seq);
            ResetAacFragment();
        }
        else if (view.marker)
        {
            ResetAacFragment();
            return false;
        }
        return true;
    }

    if (au_data_size > expected_size)
    {
        return false;
    }
    if (au_data_size == expected_size)
    {
        EmitAacFrame(au_data, au_data_size, view.ts, view.ssrc, view.seq, view.seq);
        return true;
    }
    if (view.marker)
    {
        return false;
    }

    aac_fragment_.active = true;
    aac_fragment_.ssrc = view.ssrc;
    aac_fragment_.timestamp = view.ts;
    aac_fragment_.expected_size = expected_size;
    aac_fragment_.first_seq = view.seq;
    aac_fragment_.last_seq = view.seq;
    aac_fragment_.data.assign(au_data, au_data + au_data_size);
    return true;
}

void AudioDepacketizer::EmitAacFrame(const uint8_t* data,
                                     size_t len,
                                     uint32_t timestamp,
                                     uint32_t ssrc,
                                     uint16_t first_seq,
                                     uint16_t last_seq)
{
    EncodedFrame frame;
    frame.info.media_type = MediaType::Audio;
    frame.info.codec = CodecType::AAC;
    frame.info.timestamp.dts = timestamp;
    frame.info.timestamp.pts = timestamp;
    frame.info.timestamp.time_base_num = 1;
    frame.info.timestamp.time_base_den = sample_rate_ > 0 ? sample_rate_ : 1;
    frame.info.integrity = FrameIntegrity::Complete;
    frame.rtp.ssrc = ssrc;
    frame.rtp.rtp_timestamp = timestamp;
    frame.rtp.first_sequence = first_seq;
    frame.rtp.last_sequence = last_seq;
    frame.rtp.packet_count = static_cast<uint16_t>(last_seq - first_seq) + 1;
    frame.frame_type = EncodedFrameType::Audio;
    frame.sample_count = aac_config_.constant_duration;
    frame.codec_config = codec_config_;
    frame.sample_rate = sample_rate_;
    frame.channels = channels_;
    auto buffer = std::make_shared<std::vector<uint8_t>>(data, data + len);
    frame.buffer = std::move(buffer);
    frame.size = len;
    frames_.push_back(std::move(frame));
}

void AudioDepacketizer::ResetAacFragment()
{
    aac_fragment_ = AacFragment{};
}

bool AudioDepacketizer::InputSimplePayload(const RtpView& view)
{
    EncodedFrame frame;
    frame.info.media_type = MediaType::Audio;
    frame.info.codec = codec_;
    frame.info.timestamp.dts = view.ts;
    frame.info.timestamp.pts = view.ts;
    frame.info.timestamp.time_base_num = 1;
    frame.info.timestamp.time_base_den = sample_rate_ > 0 ? sample_rate_ : 1;
    frame.info.integrity = FrameIntegrity::Complete;

    frame.rtp.ssrc = view.ssrc;
    frame.rtp.rtp_timestamp = view.ts;
    frame.rtp.first_sequence = view.seq;
    frame.rtp.last_sequence = view.seq;
    frame.rtp.packet_count = 1;

    frame.frame_type = EncodedFrameType::Audio;
    frame.sample_rate = sample_rate_;
    frame.channels = channels_;
    if (codec_ == CodecType::PCMU || codec_ == CodecType::PCMA)
    {
        frame.sample_count = static_cast<uint32_t>(view.payload_len);
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>(view.payload,
                                                         view.payload + view.payload_len);
    frame.buffer = std::move(buffer);
    frame.size = view.payload_len;

    frames_.push_back(std::move(frame));
    return true;
}

bool AudioDepacketizer::HasFrame() const
{
    return !frames_.empty();
}

bool AudioDepacketizer::PopFrame(EncodedFrame& out)
{
    if (frames_.empty())
    {
        return false;
    }

    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void AudioDepacketizer::Reset()
{
    frames_.clear();
    ResetAacFragment();
}

} // namespace media
