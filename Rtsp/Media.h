#ifndef _MEDIA_H_
#define _MEDIA_H_

#include <string>
#include <memory>

enum MediaChannelId
{
    channel_0,
    channel_1,
};

enum FrameType
{
    FRAME_TYPE_I = 0x01,
    FRAME_TYPE_P,
    FRAME_TYPE_B,
    AUDIO_TyPE,
};

struct AVFrame
{
    AVFrame(uint8_t* data, int size)
        : data_(data), size_(size) {}


    std::shared_ptr<uint8_t> data_;   //数据存储区
    int size_;                        //数据大小
    uint8_t  type;				     /* 帧类型 */	
	uint32_t timestamp;		  	     /* 时间戳 */                      
};

typedef uint32_t MediaSessionId;

#endif // _MEDIA_H_