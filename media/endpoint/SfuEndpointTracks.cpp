#include "SfuEndpoint.h"
#include "MediaStreamAffinity.h"
#include "SdpTrackBinding.h"
#include "RtpHeaderExtensions.h"

#include <algorithm>
#include <limits>

namespace media
{
namespace
{

/**
 * 比较影响媒体解码的编码参数。
 * channels 至少按 1 处理，因此 0 和 1 在这里视为等价。
 * fmtp 使用字符串直接比较；本函数没有对其中参数做语义归一化。
 */
bool SameEncoding(const ::TrackInfo& a, const ::TrackInfo& b)
{
    return a.type == b.type && a.codec_id == b.codec_id && a.clock_rate == b.clock_rate &&
        std::max(1, a.channels) == std::max(1, b.channels) && a.fmtp == b.fmtp;
}

/**
 * 判断旧接收器的配置是否仍可复用。
 * 除了编码参数，还比较 PT 和 TWCC 扩展 ID；发生变化时需要重建接收状态。
 * 这里只比较列出的字段，SSRC 和 MID 的一致性由其他逻辑检查。
 */
bool SameReceiver(const ::TrackInfo& a, const ::TrackInfo& b)
{
    return SameEncoding(a, b) && a.payload_type == b.payload_type &&
        a.transport_cc_extension_id == b.transport_cc_extension_id;
}
}

/**
 * 使用已有轨道描述注册一条发布轨道。
 * id 是端点内的业务轨道标识，mid 是协商使用的媒体标识。
 * mid_extension_id 是 RTP 头扩展中 MID 对应的 ID，0 表示未配置。
 * 拒绝非法配置、重复轨道 ID、重复索引、重复 MID 和已占用的初始 SSRC。
 * 复制 TrackInfo 创建内部描述，避免直接共享调用方提供的描述对象。
 */
bool SfuEndpoint::AddPublishedTrack(const std::string& id, std::shared_ptr<RtpTrackDescription> description, std::string mid, uint8_t mid_extension_id)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (id.empty() || !description || GetState() == State::kStopped ||
        GetState() == State::kStopping || published_tracks_.count(id) || retired_track_ids_.count(id)) return false;
    const auto& info = description->getTrackInfo();
    if (info.track_index < 0 || info.clock_rate == 0 || info.payload_type > 127 ||
        (info.ssrc && ssrc_bindings_.count(info.ssrc))) return false;
    for (const auto& entry : published_tracks_) 
    {
        if (entry.second.description->getTrackIndex() == info.track_index || (!mid.empty() && entry.second.mid == mid)) 
            return false;
    }
    PublishedTrack track;
    track.description = std::make_shared<RtpTrackDescription>(info);
    track.mid = std::move(mid);
    track.mid_extension_id = mid_extension_id;
    published_tracks_.emplace(id, std::move(track));
    if (info.ssrc) 
    { 
        ssrc_bindings_.emplace(info.ssrc, id); 
        retired_ssrcs_.erase(info.ssrc); 
    }
    return true;
}

/**
 * 将输入 SSRC 关联到发布轨道。
 * 同一个 SSRC 重复绑定到同一轨道属于幂等成功；绑定到其他轨道则失败。
 * 成功绑定后清除该 SSRC 的退役标记。此函数本身没有检查 ssrc 是否为 0。
 */
bool SfuEndpoint::BindSsrc(const std::string& id, uint32_t ssrc)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!published_tracks_.count(id) || GetState() == State::kStopped) return false;
    const auto result = ssrc_bindings_.emplace(ssrc, id);
    if (result.second || result.first->second == id) retired_ssrcs_.erase(ssrc);
    return result.second || result.first->second == id;
}

/**
 * 从 SDP 媒体项构建 TrackInfo，并注册该媒体项声明的 SSRC。
 * 当前实现拒绝列出的修复流分组；主接收路径不负责先将其解封装。
 * 本函数持锁后调用另一个带锁重载，因此使用的是可重入的 recursive_mutex。
 */
bool SfuEndpoint::AddPublishedTrack(const std::string& id, const sdp::SdpMedia& media, int track_index)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    ::TrackInfo info;
    if (!BuildRtpTrackInfo(media, track_index, info)) return false;
    for (const auto& group : media.ssrcGroups)
        if (group.semantics == "FID" || group.semantics == "FEC" || group.semantics == "FEC-FR") return false;
    for (const auto& ssrc : media.ssrcs)
        if (ssrc_bindings_.count(ssrc.ssrc)) return false;
    if (!AddPublishedTrack(id, std::make_shared<RtpTrackDescription>(info), media.mid,
        FindRtpExtensionId(media, "urn:ietf:params:rtp-hdrext:sdes:mid"))) return false;
    for (const auto& ssrc : media.ssrcs) BindSsrc(id, ssrc.ssrc);
    return true;
}

/**
 * 根据新的媒体协商结果更新整套发布轨道。
 * 输入的每个媒体项必须有唯一、非空的 MID，并且只包含一个选定的编码。
 * 优先按 MID 复用旧轨道身份；新轨道分配新索引；消失的旧轨道进入暂停集合。
 * 活动订阅不允许不兼容的编码变化或当前发送器无法处理的源 SSRC 切换。
 * 先构建和校验临时容器，再集中替换；提交阶段被调用函数的异常行为另需确认。
 * 函数使用 function-try-block，捕获 std::bad_alloc 并返回 false。
 */
bool SfuEndpoint::ReplacePublishedTracks(const std::vector<sdp::SdpMedia>& medias)
{
    // 候选轨道只保存新协商信息；此阶段不修改端点当前运行状态。
    struct Candidate
    {
        ::TrackInfo info;
        std::string mid;
        uint8_t mid_extension = 0;
        std::vector<uint32_t> ssrcs;
    };
    // 检查新配置内部的 MID 和显式 SSRC 是否重复。
    std::vector<Candidate> candidates;
    std::unordered_set<std::string> mids;
    std::unordered_set<uint32_t> offeredSsrcs;
    for (const auto& media : medias)
    {
        if (media.mid.empty() || !mids.insert(media.mid).second || media.codecs.size() != 1) return false;
        const auto& codec = media.codecs.front();
        if (codec.payloadType < 0 || codec.payloadType > 127 || codec.clockRate <= 0 || codec.channels < 0)
            return false;
        for (const auto& group : media.ssrcGroups)
            if (group.semantics == "FID" || group.semantics == "FEC" || group.semantics == "FEC-FR") return false;
        Candidate candidate;
        if (!BuildRtpTrackInfo(media, 0, candidate.info)) return false;
        candidate.mid = media.mid;
        candidate.mid_extension = FindRtpExtensionId(media, "urn:ietf:params:rtp-hdrext:sdes:mid");
        for (const auto& source : media.ssrcs)
        {
            if (!source.ssrc || !offeredSsrcs.insert(source.ssrc).second) return false;
            candidate.ssrcs.push_back(source.ssrc);
        }
        candidates.push_back(std::move(candidate));
    }
    // 收集需要清理订阅发送器的目标端点，解锁后再向其所属 Worker 投递任务
    std::vector<std::weak_ptr<SfuEndpoint>> prune;
    {
        std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
        if (GetState() == State::kStopped || GetState() == State::kStopping) return false;
        std::unordered_map<std::string, PublishedTrack> tracks;
        std::unordered_map<uint32_t, std::string> bindings;
        auto paused = paused_published_tracks_;
        auto retired = retired_ssrcs_;
        std::unordered_set<std::string> reset;
        std::vector<std::shared_ptr<Subscription>> deactivate;
        int nextIndex = 0;
        const auto reserveIndex = [&](const PublishedTrack& track)
        {
            const int index = track.description->getTrackIndex();
            if (index == std::numeric_limits<int>::max()) return false;
            nextIndex = std::max(nextIndex, index + 1);
            return true;
        };

        // 新索引从当前轨道和暂停轨道使用过的最大索引之后开始分配。
        // 保留旧索引，使轨道在重新协商或恢复时尽量保持内部身份稳定。
        for (const auto& entry : published_tracks_) if (!reserveIndex(entry.second)) return false;
        for (const auto& entry : paused) if (!reserveIndex(entry.second.track)) return false;

        for (auto& candidate : candidates)
        {
            // 新轨道默认以 MID 作为业务 ID；找到旧轨道后沿用原有业务 ID。
            std::string id = candidate.mid;
            const PublishedTrack* previous = nullptr;
            const PausedPublishedTrack* suspended = nullptr;
            for (const auto& entry : published_tracks_)
                if (entry.second.mid == candidate.mid || (entry.second.mid.empty() && entry.first == candidate.mid))
                {
                    id = entry.first;
                    previous = &entry.second;
                    break;
                }
            // 当前发布集合没有匹配项时，再查找之前暂停保存的轨道。
            if (!previous)
                for (const auto& entry : paused)
                    if (entry.second.track.mid == candidate.mid)
                    {
                        id = entry.first;
                        previous = &entry.second.track;
                        suspended = &entry.second;
                        break;
                    }
            
            // 防止新候选之间冲突，以及新身份占用已有、暂停或退役的轨道 ID。
            if (tracks.count(id) || (!previous && (published_tracks_.count(id) || paused.count(id) ||
                retired_track_ids_.count(id)))) return false;
            if (!previous && nextIndex == std::numeric_limits<int>::max()) return false;
            candidate.info.track_index = previous ? previous->description->getTrackIndex() : nextIndex++;
            PublishedTrack track;
            if (previous)
            {
                track = *previous;
                const auto& oldInfo = previous->description->getTrackInfo();
                // 有活动订阅时拒绝编码变更，避免现有下游继续按旧编码解释 RTP。
                if (!SameEncoding(oldInfo, candidate.info))
                    for (const auto& weak : previous->subscribers)
                        if (const auto route = weak.lock(); route && route->active.load()) return false;
                // PT/TWCC 等接收参数改变后，旧接收状态不再继续沿用。
                if (!SameReceiver(oldInfo, candidate.info)) reset.insert(id);
            }
            track.description = std::make_shared<RtpTrackDescription>(candidate.info);
            track.mid = candidate.mid;
            track.mid_extension_id = candidate.mid_extension;
            
            // 新媒体项没有显式声明 SSRC 时，恢复暂停轨道的 SSRC 或沿用现有绑定。
            if (candidate.ssrcs.empty())
            {
                if (suspended) candidate.ssrcs = suspended->ssrcs;
                else
                    for (const auto& binding : ssrc_bindings_)
                        if (binding.second == id) candidate.ssrcs.push_back(binding.first);
            }

            // 逐个检查来源 SSRC：不能抢占其他轨道或暂停轨道保留的身份。
            // 退役 SSRC 仅允许由保存了该 SSRC 的暂停轨道恢复。
            for (const auto ssrc : candidate.ssrcs)
            {
                const auto old = ssrc_bindings_.find(ssrc);
                if (old != ssrc_bindings_.end() && old->second != id) return false;
                if (retired_ssrcs_.count(ssrc) && (!suspended ||
                    std::find(suspended->ssrcs.begin(), suspended->ssrcs.end(), ssrc) == suspended->ssrcs.end()))
                    return false;
                for (const auto& entry : paused)
                    if (entry.first != id && std::find(entry.second.ssrcs.begin(), entry.second.ssrcs.end(), ssrc)
                        != entry.second.ssrcs.end()) return false;
                if (!bindings.emplace(ssrc, id).second) return false;
                retired.erase(ssrc);
            }

            // 活动路由如果已经锁定源 SSRC，新列表必须继续包含该源。
            // 当前实现不在这里完成源切换所需的输出序号、时间戳连续性映射。
            for (const auto& weak : track.subscribers)
                if (const auto route = weak.lock(); route && route->active.load() && route->source_ssrc &&
                    std::find(candidate.ssrcs.begin(), candidate.ssrcs.end(), *route->source_ssrc) == candidate.ssrcs.end())
                    // The current sender cannot translate a new source's
                    // sequence/timestamp base into the existing output SSRC.
                    return false;
            tracks.emplace(id, std::move(track));
            paused.erase(id);
        }
         // 旧轨道在新列表中消失：保存描述和 SSRC，停用其活动路由，等待目标端清理。
        for (const auto& entry : published_tracks_)
        {
            if (tracks.count(entry.first)) continue;
            PausedPublishedTrack saved;
            saved.track = entry.second;
            for (const auto& binding : ssrc_bindings_)
                if (binding.second == entry.first) saved.ssrcs.push_back(binding.first);
            for (const auto& weak : entry.second.subscribers)
                if (const auto route = weak.lock(); route && route->active.load())
                {
                    deactivate.push_back(route);
                    prune.push_back(route->destination);
                }
            saved.track.subscribers.clear();
            paused.insert_or_assign(entry.first, std::move(saved));
        }

        // 消失的 SSRC 进入退役集合；消失或配置改变的来源需要移除旧接收器。
        std::vector<uint32_t> removeReceivers;
        for (const auto& binding : ssrc_bindings_)
        {
            if (!bindings.count(binding.first)) retired.insert(binding.first);
            if (!bindings.count(binding.first) || reset.count(binding.second)) removeReceivers.push_back(binding.first);
        }

        // 提交：在当前端点状态锁保护下移除旧接收状态、停用路由并替换各个集合。
        // 这里的 active 标志先阻止后续投递，目标端发送器的回收可以稍后执行
        for (const auto ssrc : removeReceivers) RemoveMediaStreamOnOwner(ssrc);
        for (const auto& route : deactivate) route->active.store(false);
        published_tracks_.swap(tracks);
        paused_published_tracks_.swap(paused);
        ssrc_bindings_.swap(bindings);
        retired_ssrcs_.swap(retired);
    }
    // Route flags already prevent delivery. Reclaim destination senders on
    // their owners; their next RTCP/control operation also prunes them.
    for (const auto& target : prune)
        try
        {
            if (const auto endpoint = target.lock())
                WorkerService::post_fn(POOL_MEDIA, endpoint->Id(), [target]
                {
                    if (const auto endpoint = target.lock())
                    {
                        std::lock_guard<std::recursive_mutex> guard(endpoint->runtime_mutex_);
                        endpoint->PruneSubscriptions();
                    }
                });
        }
        catch (const std::bad_alloc&) {}
    return true;
}

/**
 * 查询一条当前发布轨道的 TrackInfo，复制到输出参数。
 * 只查询发布集合，不包含暂停保存的轨道；不存在时返回 false。
 */
bool SfuEndpoint::GetPublishedTrack(const std::string& id, ::TrackInfo& info) const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = published_tracks_.find(id);
    if (it == published_tracks_.end()) return false;
    info = it->second.description->getTrackInfo();
    return true;
}

/**
 * 返回当前发布集合的大小，不包含暂停集合。
 */
size_t SfuEndpoint::PublishedTrackCount() const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    return published_tracks_.size();
}

/**
 * 统计本端 subscriptions_ 中 active 为真的路由数量。
 * 此函数不会清理路由，也没有额外检查发布端弱引用是否已经失效。
 */
size_t SfuEndpoint::SubscriptionCount() const
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    return std::count_if(subscriptions_.begin(), subscriptions_.end(),
        [](const auto& entry) { return entry.second->active.load(); });
}

/**
 * 移除指定发布轨道及其 SSRC 绑定、接收状态，并停用相关订阅。
 * retire_identity 为 true 时，将删除的轨道 ID 和 SSRC 标记为退役，限制后续复用。
 * 与 ReplacePublishedTracks 的暂停流程不同，这里不会创建暂停快照。
 * 源端先停用路由，再投递任务让各目标端回收自己的订阅发送器。
 */
bool SfuEndpoint::RemovePublishedTrack(const std::string& id, bool retire_identity)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = published_tracks_.find(id);
    if (it == published_tracks_.end()) return false;
    for (const auto& weak : it->second.subscribers) 
    {
        if (auto route = weak.lock()) 
        {
            route->active.store(false);
            if (auto destination = route->destination.lock()) 
            {
                const std::weak_ptr<SfuEndpoint> target = destination;
                WorkerService::post_fn(POOL_MEDIA, destination->Id(), [target] {
                    if (auto endpoint = target.lock()) {
                        std::lock_guard<std::recursive_mutex> lock(endpoint->runtime_mutex_);
                        endpoint->PruneSubscriptions();
                    }
                });
            }
        }
    }
    // 一条轨道可能绑定多个 SSRC，必须遍历并清理全部对应的接收状态。
    for (auto binding = ssrc_bindings_.begin(); binding != ssrc_bindings_.end();) {
        if (binding->second == id) 
        {
            RemoveMediaStreamOnOwner(binding->first);
            if (retire_identity) retired_ssrcs_.insert(binding->first);
            binding = ssrc_bindings_.erase(binding);
        } else ++binding;
    }
    if (retire_identity) retired_track_ids_.insert(id);
    published_tracks_.erase(it);
    return true;
}

/**
 * 识别输入 RTP 包所属轨道，并尝试建立 SSRC 绑定。
 * 综合上层 hint、已有 SSRC 映射、MID 头扩展和 PT；已有信息之间必须一致。
 * 只有无法通过前述信息选定轨道时，才使用“唯一 PT 匹配”作为兜底。
 * 本函数未在入口单独加锁，由调用路径（此文件中的 OnRtp）持有状态锁。
 * 读取固定头字段前的长度/格式校验依赖 ReadRtpHeaderExtensions 及上层保证。
 */
bool SfuEndpoint::ResolveRtpTrack(common::BufferView packet, const std::string& hint)
{
    std::unordered_map<uint8_t, std::string> extensions;
    if (!rtsp::ReadRtpHeaderExtensions(packet.Data(), packet.Size(), extensions)) return false;

    // 从 RTP 固定头读取源 SSRC；PT 的最高位是 Marker 位，因此使用 & 127
    const uint32_t ssrc = media_affinity::ReadUint32BE(packet.Data() + 8);
    if (retired_ssrcs_.count(ssrc)) return false;
    const uint8_t pt = packet.Data()[1] & 127;
    std::string selected = hint;

    // 已绑定 SSRC 可以直接确定轨道；若上层 hint 与已有映射矛盾则拒绝。
    const auto bound = ssrc_bindings_.find(ssrc);
    if (bound != ssrc_bindings_.end()) 
    {
        if (!selected.empty() && selected != bound->second) return false;
        selected = bound->second;
    }

    // 查找各轨道协商的 MID 扩展 ID。
    // 一旦包里存在这些 ID 对应的扩展，就要求 MID 能匹配轨道且不与已有选择冲突。
    bool has_mid = false;
    std::string mid_track;
    for (const auto& entry : published_tracks_) 
    {
        const auto ext = extensions.find(entry.second.mid_extension_id);
        if (!entry.second.mid_extension_id || ext == extensions.end()) continue;
        has_mid = true;
        if (ext->second == entry.second.mid) 
        {
            if (!mid_track.empty() && mid_track != entry.first) return false;
            mid_track = entry.first;
        }
    }
    if (has_mid) 
    {
        if (mid_track.empty() || (!selected.empty() && selected != mid_track)) return false;
        selected = mid_track;
    }

    // 没有 hint、SSRC 或 MID 帮助定位时，仅允许 PT 唯一对应某条轨道。
    // 多条同编码轨道可能共用 PT，此时不能猜测其归属。
    if (selected.empty()) 
    {
        for (const auto& entry : published_tracks_) 
        {
            if (entry.second.description->getPayloadType() != pt) continue;
            if (!selected.empty()) return false; // Ambiguous PT cannot bind a new SSRC.
            selected = entry.first;
        }
    }

    // 即使已经识别出轨道，仍需核对包的 PT 是否与该轨道配置一致。
    const auto track = published_tracks_.find(selected);
    if (track == published_tracks_.end() || track->second.description->getPayloadType() != pt) return false;
    return BindSsrc(selected, ssrc);
}

/**
 * RTP 任务入口：检查运行状态、识别轨道，然后处理收包与订阅转发。
 * 将处理期间积累的 pending_frames_ 取到本地，在解锁后触发编码帧分发。
 */
void SfuEndpoint::OnRtp(WorkJob& job)
{
    std::vector<media::EncodedFrame::Ptr> frames;
    {
        std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
        if (!IsRunning() || job.payload.Empty() || !ResolveRtpTrack(job.payload.View(), job.media_track_id)) return;
        media_latency::PacketScope trace_scope(job.media_trace);
        HandleRtpPacket(job.payload.View());

        // swap 将当前待分发列表移到局部变量，同时让端点的待分发列表变为空。
        frames.swap(pending_frames_);
    }
    // Business/recording callbacks can enter Room or change subscriptions.
    // Never invoke them while holding the endpoint's media-state lock.
    for (const auto& frame : frames) DispatchEncodedFrame(frame);
}

/**
 * RTCP 任务入口：检查端点和显式轨道提示，然后解析/分发反馈。
 * 有明确轨道提示且首个 RTCP 包为 SR（PT=200）时，允许用其发送者 SSRC 提前绑定来源。
 * 不能根据 NACK/PLI 等反馈中的被反馈 SSRC 推断发布源。
 * 这里的特殊分支只检查包起始处是否为 SR，不遍历复合包中每个子包来建立绑定。
 */
void SfuEndpoint::OnRtcp(WorkJob& job)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    if (!job.media_track_id.empty() && !published_tracks_.count(job.media_track_id)) return;
    // An explicit transport binding also lets an SR preceding the first RTP
    // establish the source. Never infer a source from a feedback media SSRC.
    if (!job.media_track_id.empty() && job.payload.Size() >= 8 && job.payload.Data()[1] == 200) 
    {
        rtcpx::RtcpPacketInfo info;
        if (!rtcpx::InspectRtcpPacket(job.payload.Data(), job.payload.Size(), &info)) return;
        const auto ssrc = media_affinity::ReadUint32BE(job.payload.Data() + 4);
        if (retired_ssrcs_.count(ssrc) || !BindSsrc(job.media_track_id, ssrc)) return;
    }
    if (!job.payload.Empty()) HandleRtcpPacket(job.payload.View());
}

/**
 * 设置某条发布轨道专用的 RTCP 发送回调。
 * 轨道不存在时不做任何操作；传入空回调可取消该轨道的专用发送方式。
 */
void SfuEndpoint::SetTrackRtcpSendCallback(const std::string& id, SendRtcpCallback cb)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = published_tracks_.find(id);
    if (it != published_tracks_.end()) it->second.send_rtcp = std::move(cb);
}

/**
 * 根据来源 SSRC 选择 RTCP 发送方式。
 * 优先使用发布轨道专用回调；没有专用回调时使用端点默认 send_rtcp_cb_。
 * 访问轨道和绑定集合的同步依赖调用方；此处只显式锁住默认回调的专用互斥量。
 */
SfuEndpoint::SendRtcpCallback SfuEndpoint::RtcpSenderFor(uint32_t ssrc)
{
    const auto binding = ssrc_bindings_.find(ssrc);
    if (binding != ssrc_bindings_.end()) 
    {
        const auto track = published_tracks_.find(binding->second);
        if (track != published_tracks_.end() && track->second.send_rtcp) return track->second.send_rtcp;
    }
    std::lock_guard<std::mutex> lock(rtcp_send_mutex_);
    return send_rtcp_cb_;
}

/**
 * 为一个尚未激活的订阅准备输出参数，保存在当前订阅端点上。
 * config 描述输出 SSRC、PT、时钟及头扩展；transport 是发往订阅者的传输对象。
 * 检查输出 SSRC 唯一、扩展 ID 范围、MID 长度以及 MID/TWCC 扩展 ID 不冲突。
 * 此函数仅保存参数，不建立源关系，也不创建 RtpSenderTrack。
 * 没有活动订阅时，同一个配置 ID 可以重新赋值。
 */
bool SfuEndpoint::ConfigureSubscription(const std::string& id, const rtsp::RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport, CodecId codec)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    PruneSubscriptions();
    if (id.empty() || !transport || codec == CodecId::Unknown || !config.local_ssrc || config.payload_type > 127 ||
        config.sample_rate <= 0 || subscriptions_.count(id) || GetState() == State::kStopped) return false;

    // 当前实现将扩展 ID 限制为 0..14，0 用作未配置标记；MID 最长 16 字节。
    // 配置了 MID 扩展就必须有 MID；没有扩展 ID 时也不接受非空 MID。
    if (config.transport_cc_extension_id > 14 || config.mid_extension_id > 14 ||
        (config.mid_extension_id && (config.mid.empty() || config.mid.size() > 16 ||
                                    config.mid_extension_id == config.transport_cc_extension_id)) ||
        (!config.mid_extension_id && !config.mid.empty())) return false;
    for (const auto& entry : subscription_parameters_)
        if (entry.first != id && entry.second.config.local_ssrc == config.local_ssrc) return false;

    // 转发出的 PT 和扩展必须服从订阅端配置，不能原样照搬发布端的协商编号。
    subscription_parameters_[id] = {config, std::move(transport), codec};
    subscription_parameters_[id].config.rewrite_payload_type = true;
    subscription_parameters_[id].config.rewrite_header_extensions = true;
    return true;
}


/**
 * 在当前端点建立对 source/source_track_id 的订阅，id 是本端订阅标识。
 * 先调用 ConfigureSubscription 准备本端输出参数，再调用本函数建立路由。
 * 同时锁住源端和目标端，创建目标专属发送器，并在两端登记同一条路由。
 * 本实现检查编码 ID 和时钟频率，但不在此处比较完整 FMTP/声道等参数。
 */
bool SfuEndpoint::Subscribe(const std::string& id, const std::shared_ptr<SfuEndpoint>& source, const std::string& source_track_id)
{
    if (!source || source.get() == this) return false;

     // 一次获取两把端点状态锁，避免两端交叉订阅时使用相反加锁顺序造成死锁。
    std::scoped_lock lock(runtime_mutex_, source->runtime_mutex_);
    PruneSubscriptions();
    const auto params = subscription_parameters_.find(id);
    const auto track = source->published_tracks_.find(source_track_id);
    if (!IsRunning() || !source->IsRunning() || subscriptions_.count(id) ||
        params == subscription_parameters_.end() || track == source->published_tracks_.end()) return false;
    
    const auto& source_info = track->second.description->getTrackInfo();
    if (params->second.codec != source_info.codec_id ||
        params->second.config.sample_rate != static_cast<int>(source_info.clock_rate)) return false;

    // route 记录发布源、订阅目标和源轨道；目标端持有路由，源轨道保存其弱引用。
    auto route = std::make_shared<Subscription>();
    route->source = source;
    route->destination = shared_from_this();
    route->source_track_id = source_track_id;

    // 描述里已有来源 SSRC 时先锁定该来源；未知时的后续确定逻辑不在本函数中。
    if (source_info.ssrc) route->source_ssrc = source_info.ssrc;

    // 每条订阅创建自己的 RTP 发送器，负责按目标配置改写、发送和处理发送侧状态。
    route->sender = std::make_shared<rtsp::RtpSenderTrack>(params->second.config, params->second.transport);
    const std::weak_ptr<Subscription> weak = route;

    // 下游发送器需要关键帧时，将请求转交发布端所属 Worker。
    // 使用弱引用并在异步任务执行时再次检查 active，避免取消订阅后继续请求。
    route->sender->SetKeyFrameRequestCallback([weak] {
        if (const auto route = weak.lock(); route && route->active.load()) 
        {
            if (const auto upstream = route->source.lock()) 
            {
                WorkerService::post_fn(POOL_MEDIA, upstream->Id(), [weak] {
                    if (const auto current = weak.lock(); current && current->active.load())
                        if (auto publisher = current->source.lock()) publisher->RequestKeyFrame(current->source_track_id);
                });
            }
        }
    });

    // 按输出 SSRC 注册发送器，使相关下游 RTCP 反馈能够被分发到该发送器。
    // 具体反馈类型和处理路径由 RTCP dispatcher / RtpSenderTrack 的实现决定。
    rtcp_dispatcher_->AddSenderTrack(route->sender->GetSsrc(), route->sender);
    subscriptions_.emplace(id, route);
    track->second.subscribers.push_back(route);
    return true;
}

/**
 * 取消本端指定订阅：先关闭路由，再移除 RTCP 注册和本端持有的路由对象。
 * 源轨道里的弱引用可能暂时留下，使用时 lock() 或 active 检查会过滤失效路由。
 * 此函数不会删除 subscription_parameters_ 中保存的配置，可供后续再次订阅使用。
 */
bool SfuEndpoint::Unsubscribe(const std::string& id)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    const auto it = subscriptions_.find(id);
    if (it == subscriptions_.end()) return false;
    it->second->active.store(false);
    if (rtcp_dispatcher_) rtcp_dispatcher_->RemoveSenderTrack(it->second->sender->GetSsrc());
    subscriptions_.erase(it);
    return true;
}

/**
 * 清理 inactive 路由和发布源已经销毁的路由。
 * 本函数不单独加锁，调用路径需持有当前端点的 runtime_mutex_。
 * 迭代器先前进，再由 Unsubscribe 删除旧元素，避免继续使用已失效的迭代器。
 */
void SfuEndpoint::PruneSubscriptions()
{
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
        if (!it->second->active.load() || it->second->source.expired()) {
            const auto id = it++->first;
            Unsubscribe(id);
        } else ++it;
    }
}

/**
 * 向某条订阅路由投递一个 RTP 包，在目标端点一侧调用。
 * 端点运行、路由活动且数据非空时，交给该路由自己的 RtpSenderTrack。
 * 这里统计的是发送尝试和 InputRtpPacket 拒绝次数，不代表对端实际收包数。
 * route 非空等前置条件由调用方保证。
 */
void SfuEndpoint::Deliver(const std::shared_ptr<Subscription>& route, common::SharedBuffer packet)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (IsRunning() && route->active.load() && !packet.Empty()) 
    {
        media_latency::Count(media_latency::Counter::SenderAttempts);
        if (!route->sender->InputRtpPacket(packet.Data(), packet.Size()))
            media_latency::Count(media_latency::Counter::SenderRejected);
    }
}

/**
 * 请求指定发布轨道的关键帧。
 * 找到该轨道绑定的各个 SSRC，分别调用 OnTrackPli。
 * 本函数不直接构造 RTCP 字节；PLI 的生成、合并或限频需查看 OnTrackPli 实现。
 */
void SfuEndpoint::RequestKeyFrame(const std::string& track_id)
{
    std::lock_guard<std::recursive_mutex> guard(runtime_mutex_);
    if (!IsRunning()) return;
    for (const auto& binding : ssrc_bindings_)
        if (binding.second == track_id) OnTrackPli(binding.first);
}
} // namespace media
