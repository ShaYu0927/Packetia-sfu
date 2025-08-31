#ifndef _H265_H_
#define _H265_H_

#include <vector>
#include <string>

struct H265Fmtp {
    std::string sprop_vps;
    std::string sprop_sps;
    std::vector<std::string> sprop_pps; // 可以有多个 PPS，用 vector 存储
};



#endif