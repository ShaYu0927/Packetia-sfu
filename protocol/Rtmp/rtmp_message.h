#ifndef _RTMP_MESSAGE_H_
#define _RTMP_MESSAGE_H_

#include <cstdint>


#define DEFAULT_CHUNK_LEN	128
#define HANDSHAKE_PLAINTEXT	0x03
#define RANDOM_LEN		(1536 - 8)

#define MSG_SET_CHUNK		1
#define MSG_ABORT			2	/*Abort Message (2)*/
#define MSG_ACK             3
#define MSG_USER_CONTROL	4	/*User Control Messages (4)*/
#define MSG_WIN_SIZE		5	/*Window Acknowledgement Size (5)*/
#define MSG_SET_PEER_BW		6	/*Set Peer Bandwidth (6)*/
#define MSG_AUDIO			8	/*Audio Message (8)*/
#define MSG_VIDEO           9   /*Video Message */
#define MSG_DATA			18	/*Data Message (18, 15) AMF0*/
#define MSG_DATA3			15	/*Data Message (18, 15) AMF3*/
#define MSG_CMD				20	/*Command Message AMF0 */
#define MSG_CMD3			17	/*Command Message AMF3 */
#define MSG_OBJECT3			16	/*Shared Object Message (19, 16) AMF3*/
#define MSG_OBJECT			19	/*Shared Object Message (19, 16) AMF0*/
#define MSG_AGGREGATE		22	/*Aggregate Message (22)*/

#define STREAM_CONTROL				0
#define STREAM_MEDIA				1

#define CHUNK_NETWORK                   2 /*网络相关的消息(参见 Protocol Control Messages)*/
#define CHUNK_SYSTEM                    3 /*向服务器发送控制消息(反之亦可)*/
#define CHUNK_CLIENT_REQUEST_BEFORE		3 /*客户端在createStream前,向服务器发出请求的chunkID*/
#define CHUNK_CLIENT_REQUEST_AFTER		4 /*客户端在createStream后,向服务器发出请求的chunkID*/
#define CHUNK_AUDIO						6 /*音频chunkID*/
#define CHUNK_VIDEO						7 /*视频chunkID*/


namespace protocol 
{

class RtmpHandshake
{
public:
    RtmpHandshake(uint32_t _time, uint8_t *_random = nullptr);

    uint8_t time_stamp[4];
    uint8_t zero[4] = {0};
    uint8_t random[RANDOM_LEN];

    void random_generate(char *bytes, int size);
    void create_complex_c0c1();
};

class RtmpHeader 
{
public:
#if __BYTE_ORDER == __BIG_ENDIAN
    uint8_t fmt : 2;
    uint8_t chunk_id : 6;
#else
    uint8_t chunk_id : 6;
    uint8_t fmt : 2;
#endif

    uint8_t time_stamp[3];
    uint8_t body_size[3];
    uint8_t type_id;
    uint8_t stream_index[4]; /* little-endian */
};

class FLVHeader 
{
public:
    static constexpr uint8_t kFlvVersion = 1;
    static constexpr uint8_t kFlvHeaderLength = 9;
    //FLV
    char flv[3];
    //File version (for example, 0x01 for FLV version 1)
    uint8_t version;
#if __BYTE_ORDER == __BIG_ENDIAN
    // 保留,置0
    uint8_t : 5;
    // 是否有音频 
    uint8_t have_audio: 1;
    // 保留,置0  
    uint8_t : 1;
    // 是否有视频
    uint8_t have_video: 1;
#else
    // 是否有视频 
    uint8_t have_video: 1;
    // 保留,置0  
    uint8_t : 1;
    // 是否有音频 
    uint8_t have_audio: 1;
    // 保留,置0
    uint8_t : 5;
#endif
    // The length of this header in bytes,固定为9 
    uint32_t length;
    // 固定为0
    uint32_t previous_tag_size0;
};

class RtmpTagHeader 
{
public:
    uint8_t type = 0;
    uint8_t data_size[3] = {0};
    uint8_t timestamp[3] = {0};
    uint8_t timestamp_ex = 0;
    uint8_t streamid[3] = {0}; /* Always 0. */
};

struct RtmpVideoHeaderEnhanced 
{
#if __BYTE_ORDER == __BIG_ENDIAN
    uint8_t enhanced : 1;
    uint8_t frame_type : 3;
    uint8_t pkt_type : 4;
    uint32_t fourcc;
#else
    uint8_t pkt_type : 4;
    uint8_t frame_type : 3;
    uint8_t enhanced : 1;
    uint32_t fourcc;
#endif
};

struct RtmpVideoHeaderClassic 
{
#if __BYTE_ORDER == __BIG_ENDIAN
    uint8_t frame_type : 4;
    uint8_t codec_id : 4;
    uint8_t h264_pkt_type;
#else
    uint8_t codec_id : 4;
    uint8_t frame_type : 4;
    uint8_t h264_pkt_type;
#endif
};





}



#endif /* _RTMP_MESSAGE_H_ */