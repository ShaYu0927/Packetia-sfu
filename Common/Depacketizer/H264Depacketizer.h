#ifndef _H264DEPACKETIZER_H_
#define _H264DEPACKETIZER_H_

#include "Depacketizer.h"

#include <cstddef>
#include <vector>
#include <cstdint>

#include "logger.h"



class H264Depacketizer : public Depacketizer 
{
public:
    bool input(const RtpView& pkt) override;
    bool hasFrame() const override;
    std::vector<uint8_t> popFrame() override;

private:
    std::vector<uint8_t> au_;     /* the frame being assembled */
    std::vector<uint8_t> ready_frame_;

    bool started_ = false;
    uint32_t cur_ssrc_ = 0;
    uint32_t cur_ts_ = 0;

    bool have_last_seq_  = false;
    uint16_t last_seq_   = 0;
    bool has_ts          = false;
    bool assembling_fu_  = false;
    uint8_t fu_nal_type_ = 0;
    bool fu_in_progress_ = false;
    bool au_ready_ = false;
    bool maker_received_ = false;


private:
    void reset_stream(uint32_t ssrc, uint32_t ts);
    bool flush_frame(); 
    bool handle_single_nal(const uint8_t* p, size_t n);
    bool handle_stap_a(const uint8_t* p, size_t n);
    bool handle_fu_a(const uint8_t* p, size_t n);

    static inline void append_start_code(std::vector<uint8_t>& v)
    {
        static const uint8_t sc[4] = {0,0,0,1};
        v.insert(v.end(), sc, sc + 4);
    }

    static inline uint8_t nal_type(uint8_t nal_hdr)
    {
        return nal_hdr & 0x1F;
    }

    static inline void append_bytes(std::vector<uint8_t>& v, const uint8_t* p, size_t n)
    {
        if (!p || n == 0) return;
        v.insert(v.end(), p, p + n);
    }

    inline void reset_au_state()
    {
        assembling_fu_ = false;
        fu_in_progress_ = false;
        fu_nal_type_ = 0;
    }

    inline bool seq_contiguous(uint16_t prev, uint16_t cur)
    {
        return static_cast<uint16_t>(prev + 1) == cur;
    }
};




#endif