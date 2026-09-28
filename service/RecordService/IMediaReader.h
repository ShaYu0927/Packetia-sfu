#ifndef PACKETIA_SERVICE_RECORDSERVICE_IMEDIAREADER_H_
#define PACKETIA_SERVICE_RECORDSERVICE_IMEDIAREADER_H_

#include <string>

struct AVPacket;
struct AVStream;

namespace service
{

enum class ReadResult
{
    Frame,  /* 成功读出一个编码包 */
    End,    /* 正常读取结束 */
    Error   /* 读取失败，查看 Error() */
};

// 编码媒体读取接口；每个实例由单个调用线程使用，不负责播放调度。
// 打开方式由具体实现决定，例如 Mp4Reader 接收 ISeekableFile。
class IMediaReader
{
public:
    virtual ~IMediaReader() = default;

    // packet 必须由调用方初始化（例如 av_packet_alloc），读取器会先释放其旧引用。
    // Frame 时输出有效包；End/Error 时包为空。调用方用完后 av_packet_unref。
    // 时间戳保留对应轨道的 time_base，包数据在读取器 Close 后仍有效。
    virtual ReadResult Read(AVPacket& packet) = 0;

    virtual int TrackCount() const = 0;
    // 无效索引或未打开时返回 nullptr；返回值仅在 Close/再次 Open 前有效。
    virtual const AVStream* Track(int index) const = 0;

    // 释放所持资源，允许重复调用。
    virtual void Close() = 0;

    virtual const std::string& Error() const = 0;
};

}

#endif // PACKETIA_SERVICE_RECORDSERVICE_IMEDIAREADER_H_
