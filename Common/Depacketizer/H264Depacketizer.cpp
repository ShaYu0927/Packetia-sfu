#include "H264Depacketizer.h"
#include "logger.h"
#include <bits/stdint-uintn.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>


static std::string DumpPayloadHead(const uint8_t* payload, size_t payload_len, size_t max_bytes = 16)
{
    if (!payload || payload_len == 0)
    {
        return "empty";
    }

    std::ostringstream oss;
    size_t n = std::min(payload_len, max_bytes);

    for (size_t i = 0; i < n; ++i)
    {
        if (i > 0)
        {
            oss << " ";
        }

        oss << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(payload[i]);
    }

    return oss.str();
}


static void DumpRtpView(const RtpView& view, const char* tag = "RtpView")
{
    // LOG_INFO("[", tag, "]",
    //          " valid=", view.valid(),
    //          " ssrc=", view.ssrc,
    //          " seq=", view.seq,
    //          " ts=", view.ts,
    //          " marker=", view.marker,
    //          " payload_len=", view.payload_len,
    //          " payload_head=", DumpPayloadHead(view.payload, view.payload_len));
}

bool H264Depacketizer::input(const RtpView& pkt)
{
    if (!pkt.valid()) return false;

    DumpRtpView(pkt, "BeforeH264Depacketizer");

    const uint8_t* payload = pkt.payload;
    size_t payload_len = pkt.payload_len;


    const uint32_t ts  = pkt.ts;
    const uint16_t seq = pkt.seq;
    const bool marker  = pkt.marker;
    const uint32_t ssrc = pkt.ssrc;

    if(!started_  || pkt.ts != cur_ts_ || pkt.ssrc != cur_ssrc_)
    {
        // LOG_INFO("Starting new stream: ssrc=" + std::to_string(ssrc) + "ts=" + std::to_string(ts));
        reset_stream(pkt.ts, pkt.ts);
        started_ = true;
        cur_ssrc_ = ssrc;
        cur_ts_ = ts;
        have_last_seq_ = false;
    }


    if(ts != cur_ts_)
    {
        LOG_INFO(std::string("[H264Depack][TS_SWITCH] from=") + std::to_string(cur_ts_) + " to=" + std::to_string(ts) + " -> flush");
        if (!flush_frame())
        {
            LOG_ERROR("Failed to flush frame for ts %u", cur_ts_);
            reset_stream(ssrc, ts);
            return false;
        }
        cur_ts_ = ts;
    }

    if(have_last_seq_ && !seq_contiguous(last_seq_, seq))
    {
        LOG_INFO(std::string("[H264Depack][SEQ_GAP]") + " got=" + std::to_string(seq) + " last=" + std::to_string(last_seq_));
        reset_stream(ssrc, ts);
        return false;
    }

    last_seq_ = seq;
    have_last_seq_ = true;

    const uint8_t nalhdr = payload[0];
    const uint8_t type = nal_type(nalhdr);

    /* sigle NAL 1 ~ 23 */
    if(type >= 1 && type <= 23)
    {
        return handle_single_nal(payload, payload_len);
    }
    else if(type == 24) /* STAP-A */
    {
        return handle_stap_a(payload, payload_len);
    }
    else if(type == 28) /* FU-A */
    {
        return handle_fu_a(payload, payload_len);
    }
    else
    {
        LOG_ERROR("Unsupported NAL type",
              " type=", static_cast<int>(type),
              " ssrc=", pkt.ssrc,
              " seq=", pkt.seq,
              " ts=", pkt.ts,
              " marker=", pkt.marker,
              " payload_len=", pkt.payload_len,
              " payload0=", pkt.payload_len > 0 ? static_cast<int>(pkt.payload[0]) : -1,
              " payload1=", pkt.payload_len > 1 ? static_cast<int>(pkt.payload[1]) : -1,
              " payload2=", pkt.payload_len > 2 ? static_cast<int>(pkt.payload[2]) : -1,
              " payload3=", pkt.payload_len > 3 ? static_cast<int>(pkt.payload[3]) : -1);
        return false;
    }

    return true;
}

bool H264Depacketizer::flush_frame()
{
    return true;
}

void H264Depacketizer::reset_stream(uint32_t ssrc, uint32_t ts)
{

}


bool H264Depacketizer::hasFrame() const
{
    return false;
}

std::vector<uint8_t> H264Depacketizer::popFrame()
{
    return {};
}


bool H264Depacketizer::handle_single_nal(const uint8_t* p, size_t n)
{
    iVideoFrame f;
    append_start_code(au_);
    append_bytes(au_, p, n);
    if(maker_received_)
    {
        if (!flush_frame())
        {
            LOG_ERROR("Failed to flush frame for ts %u", cur_ts_);
            reset_stream(cur_ssrc_, cur_ts_);
            return false;
        }
        iVideoFrame f;
        f.ssrc = cur_ssrc_;
        f.ts   = cur_ts_;
        f.annexb = std::move(au_); 
        au_.clear();               
    }
    frameSource.publish(std::move(f));
    return true;
}
bool H264Depacketizer::handle_stap_a(const uint8_t* p, size_t n)
{
    iVideoFrame f;
    size_t off = 1;
    while (off + 2 <= n)
    {
        uint16_t nal_size = (p[off] << 8) | p[off + 1];

        if(nal_size == 0)
        {
           continue;
        }

        off += 2;
        if (off + nal_size > n)
        {
            LOG_ERROR("STAP-A NAL size exceeds payload: nal_size=%u, remaining=%zu", nal_size, n - off);
            return false;
        }
        append_start_code(au_);
        append_bytes(au_, p + off, nal_size);
        off += nal_size;
    }

    if(maker_received_)
    {
        if (!flush_frame())
        {
            LOG_ERROR("Failed to flush frame for ts %u", cur_ts_);
            reset_stream(cur_ssrc_, cur_ts_);
            return false;
        }
        iVideoFrame f;
        f.ssrc = cur_ssrc_;
        f.ts   = cur_ts_;
        f.annexb = std::move(au_); 
        au_.clear();               
    }
    frameSource.publish(std::move(f));
    return true;
}

bool H264Depacketizer::handle_fu_a(const uint8_t* p, size_t n)
{
    iVideoFrame f;
    if (n < 2)
    {
        LOG_ERROR("FU-A payload too small: %zu", n);
        return false;
    }

    const uint8_t fu_indicator = p[0];
    const uint8_t fu_header    = p[1];
    bool start_bit = (fu_header & 0x80) != 0;
    bool end_bit   = (fu_header & 0x40) != 0;

   
    const uint8_t fu_type = (fu_header & 0x1F);
    const uint8_t nri = (fu_indicator & 0x60);
    const uint8_t fbit= (fu_indicator & 0x80);
    uint8_t nal_type = fu_header & 0x1F;

    const uint8_t reconstructed_nal = static_cast<uint8_t>(fbit | nri | fu_type);
    const uint8_t* fu_payload = p + 2;
    const size_t fu_payload_len = n - 2;

    if (start_bit)
    {
        if (fu_in_progress_)
        {
            // LOG_INFO("Start bit received while FU already in progress, resetting AU state");
            reset_au_state();
        }
        fu_nal_type_ = nal_type;
        append_start_code(au_);
        uint8_t nal_hdr = (fu_indicator & 0xE0) | nal_type;
        au_.push_back(nal_hdr);
        append_bytes(au_, p + 2, n - 2);
        fu_in_progress_ = true;
    }
    else
    {
        if (!fu_in_progress_)
        {
            LOG_INFO("Non-start FU-A packet received without FU in progress, ignoring");
            return false;
        }
        if (nal_type != fu_nal_type_)
        {
            LOG_INFO("NAL type mismatch in FU-A packet: expected %u, got %u", fu_nal_type_, nal_type);
            reset_au_state();
            return false;
        }
        append_bytes(au_, p + 2, n - 2);
    }
    f.ssrc = cur_ssrc_;
    f.ts   = cur_ts_;
    f.annexb = std::move(au_); 
    au_.clear();
    frameSource.publish(std::move(f));               
    return true;
}