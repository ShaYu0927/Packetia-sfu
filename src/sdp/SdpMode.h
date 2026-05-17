#ifndef _SDP_CFG_H_
#define _SDP_CFG_H_

#include <string>
#include <vector>

namespace sdp 
{

struct SdpLine
{
    char type = '\0';
    std::string value;
    std::string raw;
    int line_no = 0;
};

struct SdpAttribute
{
    std::string key;
    std::string value;
};

struct SdpConnection
{
    std::string net_type;
    std::string addr_type;
    std::string address;
};

struct SdpRtpMap
{
    int payloadType = -1;
    std::string encodingName;
    int clockRate = 0;
    int channels = 0;
};

struct SdpFmtp
{
    int payloadType = -1;
    std::string params;
};

struct SdpMedia
{
    std::string media;                              
    int port = 0;
    std::string proto;                              
    std::vector<std::string> fmts;                  
    std::vector<SdpAttribute> attributes;
    SdpConnection conn;                             

    std::vector<SdpRtpMap> rtpmaps;
    std::vector<SdpFmtp> fmtps;

    std::string GetAttribute(const std::string& key) const;
    bool HasAttribute(const std::string& key) const;
};

struct SdpOrigin
{
    std::string username;
    std::string sess_id;
    std::string sess_version;
    std::string net_type;
    std::string addr_type;
    std::string unicast_address;
};


struct SdpSession
{
    int version = 0;
    SdpOrigin origin;                // o=
    std::string session_name;          // s=
    std::string connection;            // c=
    std::string timing;                // t=
    SdpConnection conn;

    std::vector<SdpAttribute> attributes;
    std::vector<SdpMedia> medias;
};



class ISdpFieldParser
{
public:
    virtual ~ISdpFieldParser() = default;
    virtual bool Parse(const SdpLine& line,
                       SdpSession& session,
                       SdpMedia*& current_media,
                       std::string& err) = 0;
};


}


#endif /* _SDP_CFG_H_ */