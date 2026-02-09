#ifndef _H264DEPACKETIZER_H_
#define _H264DEPACKETIZER_H_

#include "Depacketizer.h"

#include <vector>
#include <cstdint>

class H264Depacketizer : public Depacketizer 
{
public:
    bool input(const Ptr& pkt) override;
    bool hasFrame() const override;
    std::vector<uint8_t> popFrame() override;

private:
    std::vector<uint8_t> au_;
    std::vector<uint8_t> ready_frame_;

    bool started_ = false;
    uint32_t cur_ssrc_ = 0;
    uint32_t cur_ts_ = 0;

    bool have_last_seq_ = false;
    uint16_t last_seq_ = 0;

    bool fu_in_progress_ = false;


private:
    static inline uint8_t nal_type(uint8_t nal_hdr) { return nal_hdr & 0x1F; }
    static void append_start_code(std::vector<uint8_t>& out);
    void reset_stream(uint32_t ssrc, uint32_t ts);
    bool flush_frame(); 
    bool handle_single_nal(const uint8_t* p, size_t n);
    bool handle_stap_a(const uint8_t* p, size_t n);
    bool handle_fu_a(const uint8_t* p, size_t n);
};




#endif