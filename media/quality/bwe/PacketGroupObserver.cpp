#include "PacketGroupObserver.h"

namespace media
{
PacketGroupObservation PacketGroupObserver::OnFeedback(const TransportFeedback&,
                                                       const PacketGroupContext&)
{
    // TODO(algorithm): 定义分组边界、跨反馈组装和样本去重，保存有界窗口。
    // TODO(algorithm): 输出已完成的组及跨组趋势/速率/丢包判断。
    // 尚无算法结果时返回空值，现有逐包控制继续工作。
    return {};
}

void PacketGroupObserver::Reset()
{
    // TODO(algorithm): 清理未完成组、上一组参考及趋势历史。
}
}
