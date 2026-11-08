#pragma once
#include <algorithm>
#include <cstdint>

namespace rtsp
{
// One instance per downstream track; all calls use the track's owner context.
// Prepare may update mapping state even if the subsequent transport send fails,
// matching the sender's existing behavior.
/*
 * 管理一个下行 RTP 流的转发状态：
 * 1. 将上游序列号映射为下游序列号。
 * 2. 在转发中断后重新设置序列号基准。
 * 3. 过滤恢复点之前的旧包。
 * 4. 为 Padding 包分配序列号，并调整后续媒体包的映射。
 *
 * 序列号和时间戳采用回绕比较，要求参与比较的实际距离小于半个周期。
 */
class RtpForwardingState
{
public:
    /*
     * 准备媒体包的输出序列号和时间戳。
     *
     * source_seq：上游 RTP 序列号。
     * source_ts：上游 RTP 时间戳。
     * out_seq：映射后的下游序列号。
     * out_ts：映射后的下游时间戳。
     * extended_source_ts：用于恢复边界判断的扩展源时间戳。
     *
     * 返回 false 表示该包早于允许的恢复边界，应当丢弃。
     * 返回 true 后，实际发送成功时还需要调用 OnMediaSent()。
     *
     * 注意：本函数会修改映射状态，并非只读检查。
     */
    bool Prepare(uint16_t source_seq, uint32_t source_ts, uint16_t& out_seq, uint32_t& out_ts, int64_t& extended_source_ts)
    {
        /* 尚未建立恢复边界时，直接使用原始 32 位时间戳。 */
        extended_source_ts = source_ts;

        /* 已完成恢复基准设置时，检查当前包是否早于恢复点。 */
        if (has_resume_floor_ && !rebase_pending_)
        {
            /*
             * 计算相对最新源时间戳的模 2^32 差值。
             * 小于半周期表示向前，否则解释为向后的差值，
             * 从而处理时间戳回绕以及迟到包。
             */
            const auto delta = uint32_t(source_ts - uint32_t(latest_source_ts_));

            extended_source_ts = latest_source_ts_ + (delta < 0x80000000U ? int64_t(delta) : int64_t(delta) - 0x100000000LL);

            /*
             * 时间戳早于恢复点：丢弃。
             * 时间戳相同时，再按序列号判断是否早于恢复点。
             *
             * 等于恢复点的序列号仍然允许通过，这里不负责去重。
             */
            if (extended_source_ts < resume_floor_ts_ ||
                (extended_source_ts == resume_floor_ts_ &&
                 uint16_t(source_seq - resume_floor_seq_) >= 0x8000))
            {
                return false;
            }
        }

        /* 转发发生中断，使用当前包重新建立序列号映射基准。 */
        if (rebase_pending_)
        {
            /* 按现有时间戳映射计算当前包的输出时间戳。 */
            const auto mapped = started_ ? base_out_ts_ + (source_ts - base_in_ts_) : source_ts;

            /*
             * 已经转发过媒体时，恢复包的时间戳不能向后退。
             * 使用无符号差值判断，同时兼容 32 位时间戳回绕。
             */
            if (started_ && has_sent_ && uint32_t(mapped - last_out_ts_) >= 0x80000000U)
            {
                return false;
            }

            /* 将当前上游序列号作为新的输入基准。 */
            base_in_seq_ = source_seq;

            /*
             * 下游从最后一个已发送包的下一个序列号继续。
             * 如果尚未发送过任何包，则沿用上游序列号。
             *
             * 这样可隐藏主动跳过的一段上游序列号。
             */
            base_out_seq_ = has_sent_
                ? uint16_t(last_out_seq_ + 1)
                : base_in_seq_;

            /* 记录恢复边界，后续拒绝比这个位置更旧的包。 */
            resume_floor_seq_ = base_in_seq_;
            resume_floor_ts_ = source_ts;
            latest_source_ts_ = extended_source_ts;
            has_resume_floor_ = true;

            /*
             * 此处不清除 rebase_pending_。
             * 等媒体包实际发送成功后，由 OnMediaSent() 清除。
             */
        }

        /* 首次准备媒体包，且没有待处理的中断恢复。 */
        if (!started_ && !rebase_pending_)
        {
            base_in_seq_ = source_seq;

            /*
             * 首个媒体包之前可能已经发送过 Padding，
             * 此时媒体包应接着 Padding 的序列号发送。
             */
            base_out_seq_ = has_sent_
                ? uint16_t(last_out_seq_ + 1)
                : source_seq;
        }

        /*
         * 输出序列号 = 输出基准 + 相对输入基准的序列号偏移。
         * 转为 uint16_t 后自然实现模 2^16 回绕。
         *
         * 正常转发期间保留上游序号间隔；
         * 只有重新设置基准时才会跳过中断区间。
         */
        out_seq = uint16_t(
            base_out_seq_ + uint16_t(source_seq - base_in_seq_));

        /* 首次准备媒体包时，建立时间戳映射基准。 */
        if (!started_)
        {
            base_in_ts_ = source_ts;
            base_out_ts_ = source_ts;
            started_ = true;
        }

        /*
         * 输出时间戳 = 输出基准 + 上游时间戳偏移。
         * 无符号运算自然处理 32 位时间戳回绕。
         */
        out_ts = base_out_ts_ + (source_ts - base_in_ts_);
        return true;
    }

    /* 媒体包实际发送成功后，提交发送状态。 */
    void OnMediaSent(uint16_t seq, uint32_t ts, int64_t source_ts)
    {
        /*
         * 首次发送，或者序列号等于/新于已记录的序列号时，
         * 更新输出位置，避免迟到包将发送状态向后推进。
         */
        if (!has_sent_ || uint16_t(seq - last_out_seq_) < 0x8000)
        {
            last_out_seq_ = seq;
            last_out_ts_ = ts;
        }

        has_sent_ = true;

        /* 恢复后的媒体包已成功发送，结束本次重新设置基准。 */
        rebase_pending_ = false;

        /* 扩展源时间戳只向前更新，避免迟到包改变回绕参考点。 */
        if (has_resume_floor_)
        {
            latest_source_ts_ =
                std::max(latest_source_ts_, source_ts);
        }
    }

    /* 标记转发中断，下一次 Prepare() 将重新建立序列号基准。 */
    void BeginDiscontinuity()
    {
        rebase_pending_ = true;
    }

    /*
     * 获取下一个 Padding 序列号。
     * 只计算候选值；发送成功后需要调用 OnPaddingSent() 提交。
     */
    uint16_t NextPaddingSequence() const
    {
        return has_sent_ ? uint16_t(last_out_seq_ + 1) : 0;
    }

    /* 获取最近记录的媒体输出时间戳，Padding 不会更新它。 */
    uint32_t LastTimestamp() const
    {
        return last_out_ts_;
    }

    /* Padding 包实际发送成功后，记录其占用的序列号。 */
    void OnPaddingSent(uint16_t seq)
    {
        /*
         * Padding 与媒体包共享下游 RTP 序列号空间。
         * 输出基准加一，让后续媒体包给这个 Padding 腾出序号。
         */
        if (started_)
        {
            ++base_out_seq_;
        }

        last_out_seq_ = seq;
        has_sent_ = true;
    }

private:
    bool started_ = false;          /* 是否已初始化媒体映射基准。 */
    bool has_sent_ = false;         /* 是否已发送过媒体或 Padding。 */
    bool rebase_pending_ = false;   /* 是否等待中断后的基准重建及发送提交。 */
    bool has_resume_floor_ = false; /* 是否已建立恢复点过滤边界。 */

    uint16_t last_out_seq_ = 0;     /* 最新记录的下游发送序列号。 */
    uint16_t resume_floor_seq_ = 0; /* 恢复点对应的上游序列号。 */
    uint16_t base_in_seq_ = 0;      /* 上游序列号映射基准。 */
    uint16_t base_out_seq_ = 0;     /* 下游序列号映射基准，包含 Padding 偏移。 */

    uint32_t last_out_ts_ = 0;      /* 最近记录的媒体输出时间戳。 */
    uint32_t base_in_ts_ = 0;       /* 上游时间戳映射基准。 */
    uint32_t base_out_ts_ = 0;      /* 下游时间戳映射基准。 */

    int64_t resume_floor_ts_ = 0;   /* 当前恢复分段的源时间戳下界。 */
    int64_t latest_source_ts_ = 0;  /* 当前恢复分段的最新扩展源时间戳参考。 */
};
}
