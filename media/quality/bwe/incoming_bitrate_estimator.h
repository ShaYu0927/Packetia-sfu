#ifndef _INCOMING_BITRATE_ESTIMATOR_H_
#define _INCOMING_BITRATE_ESTIMATOR_H_

#include <cstdint>
#include <deque>

/*
    R_hat(i) = 1 / T * sum(L(j))
*/

class IncomingBitrateEstimator 
{
public:
    uint32_t Update(size_t packet_size, int64_t now_ms);

private:
    struct Sample 
    {
        int64_t time_ms;
        size_t bytes;
    };

    std::deque<Sample> samples_;
    int64_t window_ms_ = 500;
};


#endif /* _INCOMING_BITRATE_ESTIMATOR_H_ */