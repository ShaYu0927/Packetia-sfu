#ifndef _H264_PARAMETER_SET_TRACKER_H_
#define _H264_PARAMETER_SET_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "H264Payload.h"

namespace media
{

struct SpsInfo
{
    uint32_t sps_id = 0;
    int width = -1;
    int height = -1;
    std::vector<uint8_t> payload;
};

struct PpsInfo
{
    uint32_t pps_id = 0;
    uint32_t sps_id = 0;
    std::vector<uint8_t> payload;
};

class H264ParameterSetTracker
{
public:
    bool UpdateAccessUnit(const H264AccessUnit& frame);
    bool UpdateSps(const uint8_t* nalu, size_t size);
    bool UpdatePps(const uint8_t* nalu, size_t size);

    const SpsInfo* GetSps(uint32_t sps_id) const;
    const PpsInfo* GetPps(uint32_t pps_id) const;
    const SpsInfo* LatestSps() const;
    const PpsInfo* LatestPps() const;
    void Reset();

private:
    std::unordered_map<uint32_t, SpsInfo> sps_by_id_;
    std::unordered_map<uint32_t, PpsInfo> pps_by_id_;
    uint32_t latest_sps_id_ = 0;
    uint32_t latest_pps_id_ = 0;
    bool has_latest_sps_ = false;
    bool has_latest_pps_ = false;
};

} // namespace media
#endif // _H264_PARAMETER_SET_TRACKER_H_
