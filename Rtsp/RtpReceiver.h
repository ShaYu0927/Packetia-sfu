#ifndef _RTPRECEIVER_H_
#define _RTPRECEIVER_H_

#include <map>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include "Rtsp.h"
#include "Rtp.h"

template<typename Packet, typename Seq = uint16_t>
class EnhancedPacketSortor {
public:
    using Callback = std::function<void(Seq seq, const Packet& pkt)>;

    EnhancedPacketSortor(uint16_t max_gap = 1000, size_t max_cache = 50, uint32_t flush_timeout_ms = 100)
        : _max_gap(max_gap), _max_cache(max_cache), _flush_timeout(flush_timeout_ms) {}

    void setOnPacketSorted(Callback cb) {
        _cb = std::move(cb);
    }

    void inputPacket(Seq seq, Packet pkt) {
        auto now = std::chrono::steady_clock::now();

        if (!_started) {
            _next_seq = seq;
            _last_flush_time = now;
            _started = true;
            emit(seq, pkt);
            return;
        }

        // 强制 flush（时间间隔大于指定时间）
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_flush_time).count() > _flush_timeout) {
            flushBuffered();
            _last_flush_time = now;
        }

        if (seq == _next_seq) {
            emit(seq, pkt);
            flushBuffered();
        } else if (distance(seq, _next_seq) < _max_gap) {
            _buffer[seq] = std::move(pkt);
            if (_buffer.size() > _max_cache) {
                ++_lost_count;
                std::cout << "[PacketSortor] too much cache, force drop seq=" << _next_seq << std::endl;
                ++_next_seq;
                flushBuffered();
            }
        } else {
            // 包太旧或太远，不缓存
            ++_drop_count;
        }
    }

    void flushBuffered() {
        while (!_buffer.empty()) {
            auto it = _buffer.find(_next_seq);
            if (it == _buffer.end())
                break;

            emit(it->first, it->second);
            _buffer.erase(it);
        }
    }

    size_t getLostCount() const { return _lost_count; }
    size_t getDropCount() const { return _drop_count; }

private:
    void emit(Seq seq, const Packet& pkt) {
        if (_cb) {
            _cb(seq, pkt);
        }
        ++_next_seq;
    }

    uint32_t distance(Seq a, Seq b) const {
        return static_cast<uint16_t>(a - b); // 支持回绕
    }

private:
    bool _started = false;
    Seq _next_seq = 0;

    std::map<Seq, Packet> _buffer;
    Callback _cb;

    uint16_t _max_gap;
    size_t _max_cache;
    uint32_t _flush_timeout; // milliseconds

    size_t _lost_count = 0;
    size_t _drop_count = 0;

    std::chrono::steady_clock::time_point _last_flush_time;
};


class RtpTrack : public EnhancedPacketSortor<RtpTrack, uint16_t>
{
public:
    RtpTrack();

    uint32_t getSSRC() const;
    RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len);
    void setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms);
    void setPayloadType(uint8_t pt);

protected:
    virtual void onRtpSorted(RtpPacket::Ptr rtp) {}
    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &rtp) {}
};

class RtpTrackImp : public RtpTrack{
public:
    using OnSorted = std::function<void(RtpPacket::Ptr)>;
    using BeforeSorted = std::function<void(const RtpPacket::Ptr &)>;

    void setOnSorted(OnSorted cb);
    void setBeforeSorted(BeforeSorted cb);

protected:
    void onRtpSorted(RtpPacket::Ptr rtp) override;
    void onBeforeRtpSorted(const RtpPacket::Ptr &rtp) override;

private:
    OnSorted _on_sorted;
    BeforeSorted _on_before_sorted;
};

#endif // _RTPRECEIVER_H_