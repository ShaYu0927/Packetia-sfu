#include "SdpNegotiator.h"
#include "SdpCodec.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace sdp
{
namespace
{
bool CanSend(MediaDirection direction)
{
    return direction == MediaDirection::SendRecv || direction == MediaDirection::SendOnly;
}

bool CanReceive(MediaDirection direction)
{
    return direction == MediaDirection::SendRecv || direction == MediaDirection::RecvOnly;
}

MediaDirection Direction(bool send, bool receive)
{
    return send ? (receive ? MediaDirection::SendRecv : MediaDirection::SendOnly)
                : (receive ? MediaDirection::RecvOnly : MediaDirection::Inactive);
}

MediaDirection AnswerDirection(MediaDirection remote, MediaDirection local)
{
    return Direction(CanSend(local) && CanReceive(remote), CanReceive(local) && CanSend(remote));
}

MediaDirection Reverse(MediaDirection direction)
{
    return Direction(CanReceive(direction), CanSend(direction));
}

bool DirectionSubset(MediaDirection offer, MediaDirection answer)
{
    return (!CanSend(answer) || CanReceive(offer)) && (!CanReceive(answer) || CanSend(offer));
}

bool HasMid(const BundleParameters& bundle, const std::string& mid)
{
    return std::find(bundle.mids.begin(), bundle.mids.end(), mid) != bundle.mids.end();
}

bool Active(const SdpMedia& media) { return media.port != 0 || media.bundleOnly; }
bool RtpMedia(const SdpMedia& media) { return media.media == "audio" || media.media == "video"; }

bool ValidCredentials(const IceParameters& ice)
{
    const auto valid = [](const std::string& value, size_t minimum)
    {
        return value.size() >= minimum && value.size() <= 256 &&
            value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/") == std::string::npos;
    };
    return valid(ice.ufrag, 4) && valid(ice.pwd, 22);
}

bool ValidDtls(const DtlsParameters& dtls)
{
    return !dtls.fingerprints.empty() && std::all_of(dtls.fingerprints.begin(), dtls.fingerprints.end(),
        [](const auto& fingerprint) { return !fingerprint.algorithm.empty() && !fingerprint.value.empty(); });
}

bool SameDtls(const DtlsParameters& a, const DtlsParameters& b)
{
    return a.setup == b.setup && a.fingerprints.size() == b.fingerprints.size() &&
        std::is_permutation(a.fingerprints.begin(), a.fingerprints.end(), b.fingerprints.begin(),
            [](const auto& x, const auto& y) { return x.algorithm == y.algorithm && x.value == y.value; });
}

bool SameFeedback(const RtcpFeedback& a, const RtcpFeedback& b)
{
    return a.type == b.type && a.parameter == b.parameter;
}

std::vector<RtcpFeedback> EffectiveFeedback(const std::vector<RtcpFeedback>& common,
                                           const std::vector<RtcpFeedback>& codec)
{
    auto result = common;
    for (const auto& item : codec)
        if (std::none_of(result.begin(), result.end(), [&](const auto& value) { return SameFeedback(item, value); }))
            result.push_back(item);
    return result;
}

std::vector<RtcpFeedback> FeedbackIntersection(const std::vector<RtcpFeedback>& remote,
                                              const std::vector<RtcpFeedback>& local)
{
    std::vector<RtcpFeedback> result;
    for (const auto& item : remote)
        if (std::any_of(local.begin(), local.end(), [&](const auto& value) { return SameFeedback(item, value); }) &&
            std::none_of(result.begin(), result.end(), [&](const auto& value) { return SameFeedback(item, value); }))
            result.push_back(item);
    return result;
}

void InheritTransport(SdpMedia& media, const SdpSession& session)
{
    if (media.ice.ufrag.empty()) media.ice.ufrag = session.ice.ufrag;
    if (media.ice.pwd.empty()) media.ice.pwd = session.ice.pwd;
    if (media.ice.candidates.empty()) media.ice.candidates = session.ice.candidates;
    media.ice.iceLite = media.ice.iceLite || session.ice.iceLite;
    if (media.dtls.fingerprints.empty()) media.dtls.fingerprints = session.dtls.fingerprints;
    if (media.dtls.setup == DtlsSetup::Unspecified) media.dtls.setup = session.dtls.setup;
}

bool ValidateDescription(SdpSession& description, bool answer, bool allowEmpty, std::string& error)
{
    const auto reject = [&](const char* value) { error = value; return false; };
    if (description.type != (answer ? SdpType::Answer : SdpType::Offer) || description.medias.empty())
        return reject("Expected a normalized offer/answer with media descriptions");
    std::set<std::string> mids;
    const SdpMedia* transport = nullptr;
    size_t active = 0;
    for (auto& media : description.medias)
    {
        if (media.mid.empty() || media.mid.find_first_of(" \t\r\n") != std::string::npos ||
            !mids.insert(media.mid).second || media.fmts.empty() ||
            media.port < 0 || media.port > 65535 || media.portCount != 1)
            return reject("Invalid or duplicate mid, media port or formats");
        if (media.direction != MediaDirection::SendRecv && media.direction != MediaDirection::SendOnly &&
            media.direction != MediaDirection::RecvOnly && media.direction != MediaDirection::Inactive)
            return reject("Invalid media direction");
        if (media.bundleOnly && (answer || media.port != 0 || !HasMid(description.bundle, media.mid)))
            return reject("Invalid bundle-only media");
        if (!Active(media)) continue;
        if (!RtpMedia(media))
        {
            if (answer) return reject("Only RTP media can be accepted");
            continue; // Unknown offered media are preserved as rejected m-lines.
        }
        if (media.proto != "UDP/TLS/RTP/SAVPF" || !media.rtcpMux)
            return reject("Only UDP DTLS-SRTP with rtcp-mux is supported");
        if (!description.bundle.mids.empty() && !HasMid(description.bundle, media.mid))
            return reject("All active RTP media must share the BUNDLE transport");
        InheritTransport(media, description);
        if (!ValidCredentials(media.ice)) return reject("Invalid ICE credentials");
        if (!ValidDtls(media.dtls) || (media.dtls.setup != DtlsSetup::Active && media.dtls.setup != DtlsSetup::Passive &&
            (answer || media.dtls.setup != DtlsSetup::ActPass)))
            return reject("Missing fingerprint or unsupported DTLS setup role");
        if (transport && (media.ice.ufrag != transport->ice.ufrag || media.ice.pwd != transport->ice.pwd ||
            !SameDtls(media.dtls, transport->dtls)))
            return reject("BUNDLE media must use identical transport parameters");
        transport = &media;
        ++active;
        std::set<int> formats, codecs, extensions;
        for (const auto& format : media.fmts)
        {
            int pt = -1;
            const auto parsed = std::from_chars(format.data(), format.data() + format.size(), pt);
            if (parsed.ec != std::errc{} || parsed.ptr != format.data() + format.size() ||
                pt < 0 || pt > 127 || (pt >= 64 && pt <= 95) || format != std::to_string(pt) || !formats.insert(pt).second)
                return reject("Invalid or duplicate RTP format in m= line");
        }
        for (const auto& codec : media.codecs)
            if (!formats.count(codec.payloadType) || codec.encodingName.empty() || codec.clockRate <= 0 ||
                codec.channels < 0 || !codecs.insert(codec.payloadType).second)
                return reject("Invalid RTP codec or payload type");
        if (formats != codecs) return reject("Every active RTP format needs a codec description");
        for (const auto& extension : media.headerExtensions)
            if (extension.id < 1 || extension.id > 255 || extension.uri.empty() || !extensions.insert(extension.id).second)
                return reject("Invalid or duplicate RTP header extension");
    }
    std::set<std::string> bundled;
    for (const auto& mid : description.bundle.mids)
        if (!mids.count(mid) || !bundled.insert(mid).second) return reject("BUNDLE references an unknown or duplicate mid");
    if ((!allowEmpty && active == 0) || (active > 1 && description.bundle.mids.empty()))
        return reject("Expected one RTP transport (use BUNDLE for multiple media)");
    return true;
}

bool ValidBundlePayloads(const SdpSession& description, std::string& error)
{
    // Reused payload types or SSRCs make bundled RTP ambiguous here. A header
    // extension ID may be shared only when it denotes the same URI.
    std::set<int> payloads;
    std::map<int, std::string> extensions;
    std::set<uint32_t> ssrcs;
    for (const auto& media : description.medias)
    {
        if (!Active(media)) continue;
        for (const auto& codec : media.codecs)
            if (!payloads.insert(codec.payloadType).second)
            { error = "Ambiguous bundled payload types require MID demultiplexing"; return false; }
        for (const auto& extension : media.headerExtensions)
        {
            const auto found = extensions.emplace(extension.id, extension.uri);
            if (!found.second && found.first->second != extension.uri)
            { error = "BUNDLE header extension IDs must identify the same URI"; return false; }
        }
        for (const auto& source : media.ssrcs)
            if (!ssrcs.insert(source.ssrc).second)
            { error = "SSRCs must be unique across accepted BUNDLE media"; return false; }
    }
    return true;
}

bool SameMediaIdentity(const SdpMedia& a, const SdpMedia& b)
{
    return a.mid == b.mid && a.media == b.media && a.proto == b.proto;
}

bool KeepsMediaOrder(const SdpSession& previous, const SdpSession& next)
{
    if (next.medias.size() < previous.medias.size()) return false;
    for (size_t index = 0; index < previous.medias.size(); ++index)
        if (!SameMediaIdentity(previous.medias[index], next.medias[index])) return false;
    return true;
}

bool OriginNumber(const std::string& value, uint64_t& result)
{
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool NewRemoteOrigin(const SdpOrigin& previous, const SdpOrigin& next)
{
    uint64_t sessionId = 0, version = 0, nextVersion = 0;
    // Typed callers predating origin tracking may omit o=. Once a valid remote
    // origin is committed, descriptions must stay in that session and advance.
    if (!OriginNumber(previous.sess_id, sessionId) || !OriginNumber(previous.sess_version, version)) return true;
    return next.sess_id == previous.sess_id && OriginNumber(next.sess_version, nextVersion) && nextVersion > version;
}

bool DtlsAnswer(DtlsSetup offer, DtlsSetup answer)
{
    return (offer == DtlsSetup::ActPass && (answer == DtlsSetup::Active || answer == DtlsSetup::Passive)) ||
        (offer == DtlsSetup::Active && answer == DtlsSetup::Passive) ||
        (offer == DtlsSetup::Passive && answer == DtlsSetup::Active);
}

DtlsSetup PeerRole(DtlsSetup role)
{
    return role == DtlsSetup::Active ? DtlsSetup::Passive : DtlsSetup::Active;
}

bool ValidateAnswer(const SdpSession& offer, const SdpSession& answer, std::string& error)
{
    const auto reject = [&](const char* value) { error = value; return false; };
    if (offer.medias.size() != answer.medias.size() || !KeepsMediaOrder(offer, answer))
        return reject("Answer must retain offered m-line identities and order");
    for (const auto& mid : answer.bundle.mids)
        if (!HasMid(offer.bundle, mid)) return reject("Answer introduced a BUNDLE mid");
    for (size_t index = 0; index < offer.medias.size(); ++index)
    {
        const auto& offered = offer.medias[index];
        const auto& accepted = answer.medias[index];
        for (const auto& format : accepted.fmts)
            if (std::find(offered.fmts.begin(), offered.fmts.end(), format) == offered.fmts.end())
                return reject("Answer introduced an RTP format");
        if (!Active(accepted))
        {
            if (accepted.direction != MediaDirection::Inactive) return reject("Rejected answer media must be inactive");
            continue;
        }
        if (!Active(offered) || !RtpMedia(offered)) return reject("Answer activated a rejected or unsupported m-line");
        if (!DirectionSubset(offered.direction, accepted.direction)) return reject("Answer direction exceeds the offer");
        if (!DtlsAnswer(offered.dtls.setup, accepted.dtls.setup)) return reject("Answer DTLS role does not complement the offer");
        if (accepted.rtcpRsize && !offered.rtcpRsize) return reject("Answer introduced reduced-size RTCP");
        for (const auto& codec : accepted.codecs)
        {
            const auto remote = std::find_if(offered.codecs.begin(), offered.codecs.end(),
                [&](const auto& item) { return item.payloadType == codec.payloadType; });
            if (remote == offered.codecs.end() || !SdpCodec::IsAnswer(*remote, codec))
                return reject("Answer codec or format parameters exceed the offer");
            const auto allowed = EffectiveFeedback(offered.rtcpFeedback, remote->rtcpFeedback);
            for (const auto& feedback : EffectiveFeedback(accepted.rtcpFeedback, codec.rtcpFeedback))
                if (std::none_of(allowed.begin(), allowed.end(), [&](const auto& item) { return SameFeedback(feedback, item); }))
                    return reject("Answer introduced RTCP feedback");
        }
        for (const auto& extension : accepted.headerExtensions)
        {
            const auto remote = std::find_if(offered.headerExtensions.begin(), offered.headerExtensions.end(),
                [&](const auto& item) { return item.id == extension.id; });
            if (remote == offered.headerExtensions.end() || remote->uri != extension.uri ||
                remote->attributes != extension.attributes || !DirectionSubset(remote->direction, extension.direction) ||
                (CanSend(extension.direction) && !CanSend(accepted.direction)) ||
                (CanReceive(extension.direction) && !CanReceive(accepted.direction)))
                return reject("Answer introduced or expanded an RTP header extension");
        }
    }
    return ValidBundlePayloads(answer, error);
}

bool EffectiveOffer(const SdpSession& offer, const SdpSession& answer, SdpSession& result)
{
    // Keep the offerer's transport and source parameters, but project the
    // answer's selected media back into the offerer's direction.
    auto effective = offer;
    effective.bundle = answer.bundle;
    for (size_t index = 0; index < effective.medias.size(); ++index)
    {
        auto& media = effective.medias[index];
        const auto& selected = answer.medias[index];
        media.bundleOnly = false;
        media.port = selected.port == 0 ? 0 : (media.port == 0 ? 9 : media.port);
        media.direction = Reverse(selected.direction);
        media.rtcpRsize = selected.rtcpRsize;
        media.rtcpFeedback = selected.rtcpFeedback;
        media.codecs.clear();
        for (const auto& codec : selected.codecs)
        {
            const auto& offered = offer.medias[index].codecs;
            const auto original = std::find_if(offered.begin(), offered.end(),
                [&](const auto& item) { return item.payloadType == codec.payloadType; });
            RtpCodecParameters effectiveCodec;
            // Negotiate in the opposite direction to retain the offerer's
            // receive preferences (notably Opus and asymmetric H264 levels).
            if (original == offered.end() || !SdpCodec::Negotiate(codec, *original, effectiveCodec)) return false;
            effectiveCodec.rtcpFeedback = codec.rtcpFeedback;
            media.codecs.push_back(std::move(effectiveCodec));
        }
        media.fmts = selected.fmts;
        media.headerExtensions = selected.headerExtensions;
        for (auto& extension : media.headerExtensions) extension.direction = Reverse(extension.direction);
        if (media.port != 0)
        {
            media.dtls.setup = PeerRole(selected.dtls.setup);
            effective.dtls.setup = media.dtls.setup;
        }
        if (!CanSend(media.direction))
        {
            media.ssrcs.clear(); media.ssrcGroups.clear(); media.msids.clear();
        }
    }
    result = std::move(effective);
    return true;
}

void SessionDefaults(SdpSession& session)
{
    session.profile = SdpProfile::WebRtc;
    if (session.session_name.empty()) session.session_name = "-";
    if (session.timing.empty()) session.timing = "0 0";
    if (session.connection.empty()) session.connection = "IN IP4 0.0.0.0";
    if (session.conn.address.empty()) session.conn = {"IN", "IP4", "0.0.0.0"};
}
} // namespace

bool SdpNegotiator::Reject(const std::string& error)
{
    last_error_ = error;
    return false;
}

bool SdpNegotiator::SetLocalOrigin(SdpSession& local)
{
    if (origin_version_exhausted_) return Reject("SDP origin version is exhausted");
    auto& origin = local.origin;
    uint64_t version = next_origin_version_;
    if (local_origin_id_.empty() && !origin.sess_version.empty())
    {
        const auto parsed = std::from_chars(origin.sess_version.data(), origin.sess_version.data() + origin.sess_version.size(), version);
        if (parsed.ec != std::errc{} || parsed.ptr != origin.sess_version.data() + origin.sess_version.size())
            return Reject("Invalid local SDP origin version");
    }
    if (local_origin_id_.empty())
    {
        static std::atomic<uint64_t> nextId{1};
        local_origin_id_ = origin.sess_id.empty() ? std::to_string(nextId.fetch_add(1)) : origin.sess_id;
    }
    origin.sess_id = local_origin_id_;
    origin.sess_version = std::to_string(version);
    if (origin.username.empty()) origin.username = "-";
    if (origin.net_type.empty()) origin.net_type = "IN";
    if (origin.addr_type.empty()) origin.addr_type = "IP4";
    if (origin.unicast_address.empty()) origin.unicast_address = "0.0.0.0";
    origin_version_exhausted_ = version == std::numeric_limits<uint64_t>::max();
    if (!origin_version_exhausted_) next_origin_version_ = version + 1;
    return true;
}

bool SdpNegotiator::CreateOffer(const SdpSession& local, SdpSession& offer)
{
    if (state_ != SdpNegotiationState::Stable) return Reject("Another offer/answer transaction is pending");
    auto result = local;
    result.type = SdpType::Offer;
    SessionDefaults(result);
    std::set<std::string> mids;
    for (auto& media : result.medias)
    {
        if (media.mid.empty() || !mids.insert(media.mid).second) return Reject("Local offers require unique explicit mids");
        if (!Active(media)) media.direction = MediaDirection::Inactive;
        if (media.fmts.empty())
            for (const auto& codec : media.codecs) media.fmts.push_back(std::to_string(codec.payloadType));
    }
    if (has_current_)
    {
        // RFC 3264 keeps established m-line positions stable across reoffers.
        // A missing mid becomes a rejected line; only new mids go at the end.
        std::vector<SdpMedia> ordered;
        for (const auto& previous : current_local_.medias)
        {
            const auto replacement = std::find_if(result.medias.begin(), result.medias.end(),
                [&](const auto& media) { return media.mid == previous.mid; });
            if (replacement != result.medias.end())
            {
                if (!SameMediaIdentity(previous, *replacement)) return Reject("An established mid cannot change media kind or protocol");
                ordered.push_back(*replacement);
            }
            else
            {
                auto rejected = previous;
                rejected.port = 0; rejected.bundleOnly = false; rejected.direction = MediaDirection::Inactive;
                ordered.push_back(std::move(rejected));
            }
        }
        for (const auto& media : result.medias)
            if (std::none_of(current_local_.medias.begin(), current_local_.medias.end(),
                    [&](const auto& previous) { return previous.mid == media.mid; })) ordered.push_back(media);
        result.medias = std::move(ordered);
    }
    std::set<std::string> bundled;
    for (const auto& mid : result.bundle.mids)
        if (!bundled.insert(mid).second || std::none_of(result.medias.begin(), result.medias.end(),
                [&](const auto& media) { return media.mid == mid; })) return Reject("Invalid local BUNDLE mids");
    result.bundle.mids.clear();
    // Rebuild BUNDLE from the active RTP lines after reordering and rejection.
    const auto active = std::count_if(result.medias.begin(), result.medias.end(), [](const auto& media) { return Active(media) && RtpMedia(media); });
    if (active > 1 || !bundled.empty() || (has_current_ && !current_local_.bundle.mids.empty()))
        for (const auto& media : result.medias)
            if (Active(media) && RtpMedia(media)) result.bundle.mids.push_back(media.mid);
    std::string error;
    if (!ValidateDescription(result, false, has_current_, error) || !ValidBundlePayloads(result, error)) return Reject(error);
    if (!SetLocalOrigin(result)) return false;
    auto pending = result;
    offer = result;
    wire_offer_ = std::move(result);
    pending_local_ = std::move(pending);
    pending_remote_ = {};
    pending_ready_ = false;
    state_ = SdpNegotiationState::HaveLocalOffer;
    last_error_.clear();
    return true;
}

bool SdpNegotiator::ApplyOffer(const SdpSession& offer)
{
    if (state_ == SdpNegotiationState::HaveLocalOffer) return Reject("Offer collision: roll back the local offer first");
    auto normalized = offer;
    std::string error;
    if (!ValidateDescription(normalized, false, has_current_, error)) return Reject(error);
    if (has_current_ && !NewRemoteOrigin(current_remote_.origin, normalized.origin))
        return Reject("Remote SDP origin must retain its session ID and increase its version");
    if (has_current_ && !KeepsMediaOrder(current_remote_, normalized))
        return Reject("Reoffer must retain established m-line identities and order");
    auto wire = normalized;
    pending_remote_ = std::move(normalized);
    wire_offer_ = std::move(wire);
    pending_local_ = {};
    pending_ready_ = false;
    state_ = SdpNegotiationState::HaveRemoteOffer;
    last_error_.clear();
    return true;
}

bool SdpNegotiator::CreateAnswer(const SdpSession& local, SdpSession& answer)
{
    if (state_ != SdpNegotiationState::HaveRemoteOffer) return Reject("Apply a remote offer before creating an answer");
    std::set<std::string> mids, kinds;
    for (const auto& capability : local.medias)
        if (capability.mid.empty() ? !kinds.insert(capability.media).second : !mids.insert(capability.mid).second)
            return Reject("Duplicate local media capability");
    SdpSession result = local;
    result.type = SdpType::Answer;
    result.medias.clear();
    result.bundle.mids.clear();
    SessionDefaults(result);
    size_t accepted = 0;
    for (const auto& remote : wire_offer_.medias)
    {
        SdpMedia media;
        media.mid = remote.mid; media.media = remote.media; media.proto = remote.proto;
        media.fmts = remote.fmts;
        media.direction = MediaDirection::Inactive;
        auto capability = std::find_if(local.medias.begin(), local.medias.end(),
            [&](const auto& item) { return item.mid == remote.mid; });
        // A named capability takes precedence over a kind-wide fallback, even
        // when it explicitly rejects the m-line with port=0.
        if (capability == local.medias.end())
            capability = std::find_if(local.medias.begin(), local.medias.end(),
                [&](const auto& item) { return item.mid.empty() && item.media == remote.media; });
        if (Active(remote) && RtpMedia(remote) && capability != local.medias.end() &&
            capability->media == remote.media && (capability->mid.empty() || Active(*capability)))
        {
            for (const auto& format : remote.fmts)
            {
                const auto offered = std::find_if(remote.codecs.begin(), remote.codecs.end(),
                    [&](const auto& codec) { return std::to_string(codec.payloadType) == format; });
                if (offered == remote.codecs.end()) continue;
                for (const auto& supported : capability->codecs)
                {
                    RtpCodecParameters codec;
                    if (!SdpCodec::Negotiate(*offered, supported, codec)) continue;
                    codec.rtcpFeedback = FeedbackIntersection(EffectiveFeedback(remote.rtcpFeedback, offered->rtcpFeedback),
                        EffectiveFeedback(capability->rtcpFeedback, supported.rtcpFeedback));
                    media.codecs.push_back(std::move(codec));
                    break;
                }
            }
            if (!media.codecs.empty())
            {
                media.port = 9; media.conn = {"IN", "IP4", "0.0.0.0"};
                media.fmts.clear();
                for (const auto& codec : media.codecs) media.fmts.push_back(std::to_string(codec.payloadType));
                media.direction = AnswerDirection(remote.direction, capability->direction);
                media.ice = capability->ice;
                media.dtls = capability->dtls;
                InheritTransport(media, result);
                if (!ValidCredentials(media.ice) || media.ice.candidates.empty() || !ValidDtls(media.dtls))
                    return Reject("Configure local ICE credentials, candidates and DTLS fingerprint first");
                const auto desired = media.dtls.setup;
                media.dtls.setup = desired == DtlsSetup::Active || desired == DtlsSetup::Passive ? desired :
                    (remote.dtls.setup == DtlsSetup::Active ? DtlsSetup::Passive : DtlsSetup::Active);
                if (!DtlsAnswer(remote.dtls.setup, media.dtls.setup)) return Reject("Offer DTLS role conflicts with the local role");
                result.dtls = media.dtls;
                media.rtcpMux = true;
                media.rtcpRsize = remote.rtcpRsize && capability->rtcpRsize;
                for (const auto& extension : remote.headerExtensions)
                {
                    if (extension.id > 14) continue;
                    const auto supported = std::find_if(capability->headerExtensions.begin(), capability->headerExtensions.end(),
                        [&](const auto& item) { return item.uri == extension.uri && item.attributes == extension.attributes; });
                    if (supported == capability->headerExtensions.end()) continue;
                    auto negotiated = extension;
                    negotiated.direction = AnswerDirection(extension.direction, supported->direction);
                    negotiated.direction = Direction(CanSend(negotiated.direction) && CanSend(media.direction),
                        CanReceive(negotiated.direction) && CanReceive(media.direction));
                    if (negotiated.direction != MediaDirection::Inactive) media.headerExtensions.push_back(std::move(negotiated));
                }
                const bool transportCc = std::any_of(media.headerExtensions.begin(), media.headerExtensions.end(),
                    [](const auto& extension) { return extension.uri == "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"; });
                // transport-cc feedback requires its corresponding RTP header
                // extension to be present in the negotiated answer.
                if (!transportCc)
                    for (auto& codec : media.codecs)
                        codec.rtcpFeedback.erase(std::remove_if(codec.rtcpFeedback.begin(), codec.rtcpFeedback.end(),
                            [](const auto& item) { return item.type == "transport-cc"; }), codec.rtcpFeedback.end());
                if (CanSend(media.direction))
                {
                    media.ssrcs = capability->ssrcs;
                    media.ssrcGroups = capability->ssrcGroups;
                    media.msids = capability->msids;
                }
                ++accepted;
            }
        }
        result.medias.push_back(std::move(media));
    }
    if (accepted == 0 && !has_current_) return Reject("No compatible RTP codecs in the offer");
    for (const auto& mid : wire_offer_.bundle.mids)
        if (std::any_of(result.medias.begin(), result.medias.end(),
                [&](const auto& media) { return media.mid == mid && Active(media); })) result.bundle.mids.push_back(mid);
    std::string error;
    if (!ValidateDescription(result, true, has_current_, error) || !ValidateAnswer(wire_offer_, result, error)) return Reject(error);
    SdpSession remote;
    if (!EffectiveOffer(wire_offer_, result, remote)) return Reject("Selected codec cannot be applied to the offer direction");
    if (!SetLocalOrigin(result)) return false;
    answer = result;
    pending_local_ = std::move(result);
    pending_remote_ = std::move(remote);
    pending_ready_ = true;
    last_error_.clear();
    return true;
}

bool SdpNegotiator::ApplyAnswer(const SdpSession& answer)
{
    if (state_ != SdpNegotiationState::HaveLocalOffer || pending_ready_)
        return Reject("Create a local offer before applying one answer");
    auto normalized = answer;
    std::string error;
    if (!ValidateDescription(normalized, true, true, error) || !ValidateAnswer(wire_offer_, normalized, error)) return Reject(error);
    if (has_current_ && !NewRemoteOrigin(current_remote_.origin, normalized.origin))
        return Reject("Remote SDP origin must retain its session ID and increase its version");
    SdpSession local;
    if (!EffectiveOffer(wire_offer_, normalized, local)) return Reject("Selected codec cannot be applied to the offer direction");
    pending_local_ = std::move(local);
    pending_remote_ = std::move(normalized);
    pending_ready_ = true;
    last_error_.clear();
    return true;
}

bool SdpNegotiator::Commit() noexcept
{
    if (!pending_ready_) return false;
    current_local_ = std::move(pending_local_);
    current_remote_ = std::move(pending_remote_);
    has_current_ = true;
    Rollback();
    return true;
}

void SdpNegotiator::Rollback() noexcept
{
    pending_local_ = {};
    pending_remote_ = {};
    wire_offer_ = {};
    pending_ready_ = false;
    state_ = SdpNegotiationState::Stable;
    last_error_.clear();
}

} // namespace sdp
