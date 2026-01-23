#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <iostream>
#include "Rtp.h"


static void write_u16_be(uint8_t* p, uint16_t v) 
{
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v & 0xFF);
}
static void write_u32_be(uint8_t* p, uint32_t v) 
{
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t((v >> 16) & 0xFF);
    p[2] = uint8_t((v >> 8) & 0xFF);
    p[3] = uint8_t(v & 0xFF);
}

// 构造一个“看起来完整”的 RTP 包（bytes）
// 参数：
//  - cc: CSRC 个数
//  - x:  是否携带 extension
//  - p:  是否携带 padding
//  - payload_len: 负载长度（不含 header/csrc/ext/padding）
//  - ext_words: extension length（单位 words=4字节），仅 x=true 时有效
static std::vector<uint8_t> make_rtp_packet(
    uint8_t cc,
    bool x,
    bool p,
    uint8_t pt,
    bool marker,
    uint16_t seq,
    uint32_t ts,
    uint32_t ssrc,
    size_t payload_len,
    uint16_t ext_words = 0,
    uint8_t pad_bytes = 0
) 
{
    // RTP fixed header 12B
    size_t header_len = 12 + 4u * cc;

    size_t ext_len = 0;
    if (x) 
    {
        // ext header: profile(2B) + length(2B) + data(words*4)
        ext_len = 4 + size_t(ext_words) * 4;
    }

    size_t padding_len = 0;
    if (p) 
    {
        // padding bytes 至少 1，最后一个字节等于 padding bytes
        if (pad_bytes == 0) pad_bytes = 4;
        padding_len = pad_bytes;
    }

    size_t total = header_len + ext_len + payload_len + padding_len;
    std::vector<uint8_t> pkt(total, 0);

    // vpxcc: V=2, P, X, CC
    uint8_t vpxcc = 0;
    vpxcc |= (2u << 6);
    if (p) vpxcc |= 0x20;
    if (x) vpxcc |= 0x10;
    vpxcc |= (cc & 0x0F);
    pkt[0] = vpxcc;

    // mpt: M + PT
    uint8_t mpt = (pt & 0x7F);
    if (marker) mpt |= 0x80;
    pkt[1] = mpt;

    write_u16_be(&pkt[2], seq);
    write_u32_be(&pkt[4], ts);
    write_u32_be(&pkt[8], ssrc);

    // CSRC list
    for (uint8_t i = 0; i < cc; ++i) 
    {
        write_u32_be(&pkt[12 + 4u * i], 0x11111111u + i);
    }

    // Extension
    size_t off = header_len;
    if (x) 
    {
        // profile
        pkt[off + 0] = 0xBE;
        pkt[off + 1] = 0xDE;
        // length in words
        write_u16_be(&pkt[off + 2], ext_words);
        // ext data area 填点花样
        for (size_t i = 0; i < size_t(ext_words) * 4; ++i) 
        {
            pkt[off + 4 + i] = uint8_t(i & 0xFF);
        }
        off += ext_len;
    }

    // Payload
    for (size_t i = 0; i < payload_len; ++i) 
    {
        pkt[off + i] = uint8_t(0xA0 + (i & 0x0F));
    }
    off += payload_len;

    // Padding
    if (p) {
        // 最后一个字节 = padding_len
        // 中间 padding 内容随便填
        for (size_t i = 0; i < padding_len; ++i) {
            pkt[off + i] = 0x00;
        }
        pkt[total - 1] = uint8_t(padding_len);
    }

    return pkt;
}


static void test_accept_minimal(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(
        /*cc=*/0, /*x=*/false, /*p=*/false,
        /*pt=*/96, /*marker=*/false,
        /*seq=*/123, /*ts=*/456789, /*ssrc=*/0x01020304,
        /*payload_len=*/20
    );

    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(pkt && "should accept minimal RTP");
    assert(pkt->size == raw.size());
    assert(pkt->type == TrackVideo);
    assert(pkt->sample_rate == 90000);
    assert(std::memcmp(pkt->data.get(), raw.data(), raw.size()) == 0);
}

static void test_accept_with_csrc(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(
        /*cc=*/2, /*x=*/false, /*p=*/false,
        /*pt=*/97, /*marker=*/true,
        /*seq=*/10, /*ts=*/11, /*ssrc=*/0x0A0B0C0D,
        /*payload_len=*/5
    );
    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(pkt && "should accept RTP with CSRC");
}

static void test_accept_with_extension(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(
        /*cc=*/0, /*x=*/true, /*p=*/false,
        /*pt=*/98, /*marker=*/false,
        /*seq=*/1, /*ts=*/2, /*ssrc=*/3,
        /*payload_len=*/10,
        /*ext_words=*/3
    );
    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(pkt && "should accept RTP with extension");
}

static void test_accept_with_padding(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(
        /*cc=*/0, /*x=*/false, /*p=*/true,
        /*pt=*/99, /*marker=*/false,
        /*seq=*/1, /*ts=*/2, /*ssrc=*/3,
        /*payload_len=*/10,
        /*ext_words=*/0,
        /*pad_bytes=*/8
    );
    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(pkt && "should accept RTP with padding");
}

// 非法：version != 2
static void test_reject_bad_version(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(0, false, false, 96, false, 1, 2, 3, 10);
    raw[0] &= 0x3F;          // 清掉版本位
    raw[0] |= (1u << 6);     // V=1

    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(!pkt && "should reject bad version");
}

// 非法：len < header_len（CC 导致）
static void test_reject_truncated_csrc(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(3, false, false, 96, false, 1, 2, 3, 10);
    raw.resize(12 + 4u * 3 - 1); // 少1字节

    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(!pkt && "should reject truncated CSRC");
}

// 非法：extension 截断
static void test_reject_truncated_extension(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(0, true, false, 96, false, 1, 2, 3, 10, /*ext_words=*/4);
    raw.resize(12 + 4 + 4 * 4 - 2); // extension 少2字节（只截断 extension 部分）

    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(!pkt && "should reject truncated extension");
}

// 非法：padding 字节不合法（pad=0 或 >len）
static void test_reject_bad_padding(RtpVideoTracker& trk) 
{
    auto raw = make_rtp_packet(0, false, true, 96, false, 1, 2, 3, 10, 0, 8);
    raw.back() = 0; // pad=0 非法

    auto pkt = trk.inputRtp(TrackVideo, 90000, raw.data(), raw.size());
    assert(!pkt && "should reject bad padding=0");
}

int main() 
{
    RtpVideoTracker trk(
        TrackVideo,
        "H264",
        96,
        0x12345678,
        90000
    );

    

    test_accept_minimal(trk);
    test_accept_with_csrc(trk);
    test_accept_with_extension(trk);
    test_accept_with_padding(trk);

    test_reject_bad_version(trk);
    test_reject_truncated_csrc(trk);
    test_reject_truncated_extension(trk);
    test_reject_bad_padding(trk);

    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
