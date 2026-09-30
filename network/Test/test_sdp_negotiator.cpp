#include "SdpNegotiator.h"
#include "SdpCodec.h"
#include "Sdp.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

using namespace sdp;

namespace
{
const char* kMidExtension = "urn:ietf:params:rtp-hdrext:sdes:mid";

RtpCodecParameters VideoCodec(int payload = 96, uint32_t profile = 0x42e01f,
                              bool asymmetry = false)
{
    H264CodecConfig config;
    config.profileLevelId = profile;
    config.levelAsymmetryAllowed = asymmetry;
    auto codec = SdpCodec::H264(payload, config);
    codec.rtcpFeedback = {{"nack", ""}, {"nack", "pli"}};
    return codec;
}

RtpCodecParameters AudioCodec(int payload = 111)
{
    OpusCodecConfig config;
    config.minPtime = 10;
    config.useInbandFec = true;
    auto codec = SdpCodec::Opus(payload, config);
    codec.rtcpFeedback = {{"nack", ""}};
    return codec;
}

SdpMedia Media(const std::string& kind, const std::string& mid,
               RtpCodecParameters codec)
{
    SdpMedia media;
    media.media = kind;
    media.mid = mid;
    media.port = 9;
    media.proto = "UDP/TLS/RTP/SAVPF";
    media.conn = {"IN", "IP4", "0.0.0.0"};
    media.direction = MediaDirection::SendRecv;
    media.rtcpMux = true;
    media.headerExtensions.push_back({1, kMidExtension, MediaDirection::SendRecv, ""});
    Sdp::SetCodecs(media, {std::move(codec)});
    return media;
}

SdpSession Description(const std::string& identity, uint64_t version = 0)
{
    SdpSession session;
    session.profile = SdpProfile::WebRtc;
    session.type = SdpType::Offer;
    session.origin = {"-", identity == "local" ? "1001" : "2001", std::to_string(version),
                      "IN", "IP4", "127.0.0.1"};
    session.session_name = "-";
    session.timing = "0 0";
    session.conn = {"IN", "IP4", "0.0.0.0"};
    session.ice.ufrag = identity;
    session.ice.pwd = std::string(24, identity.front());
    session.ice.candidates = {"1 1 udp 2130706431 192.0.2.1 5000 typ host"};
    session.ice.endOfCandidates = true;
    session.dtls.setup = DtlsSetup::ActPass;
    session.dtls.fingerprints.push_back({"sha-256", identity + "-fingerprint"});
    session.bundle.mids = {"audio", "video"};
    session.medias = {Media("audio", "audio", AudioCodec()),
                      Media("video", "video", VideoCodec())};
    for (auto& media : session.medias)
    {
        media.ice = session.ice;
        media.dtls = session.dtls;
    }
    return session;
}

SdpSession Capabilities(const std::string& identity = "local")
{
    auto local = Description(identity);
    local.dtls.setup = DtlsSetup::Unspecified;
    for (auto& media : local.medias) media.dtls.setup = DtlsSetup::Unspecified;
    return local;
}

const SdpMedia& FindMedia(const SdpSession& session, const std::string& mid)
{
    for (const auto& media : session.medias)
        if (media.mid == mid) return media;
    throw std::runtime_error("Missing MID: " + mid);
}

std::string Snapshot(const SdpSession& session)
{
    return std::to_string(static_cast<int>(session.type)) + ":" + Sdp::Serialize(session);
}

SdpSession AnswerFor(const SdpSession& offer, SdpSession local = Capabilities("remote"))
{
    SdpNegotiator peer;
    SdpSession answer;
    CHECK(peer.ApplyOffer(offer));
    CHECK(peer.CreateAnswer(local, answer));
    CHECK(peer.PendingReady());
    return answer;
}

void Establish(SdpNegotiator& negotiator)
{
    SdpSession answer;
    CHECK(negotiator.ApplyOffer(Description("remote")));
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.Commit());
    CHECK(negotiator.HasCurrent());
}

void Transactions()
{
    SdpNegotiator negotiator;
    CHECK(negotiator.State() == SdpNegotiationState::Stable);
    CHECK(!negotiator.HasCurrent());
    CHECK(!negotiator.PendingReady());
    CHECK(!negotiator.Commit());

    SdpSession answer = Description("sentinel");
    const auto untouchedOutput = Snapshot(answer);
    CHECK(!negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(Snapshot(answer) == untouchedOutput);
    CHECK(negotiator.ApplyOffer(Description("remote")));
    CHECK(negotiator.State() == SdpNegotiationState::HaveRemoteOffer);
    CHECK(!negotiator.HasCurrent());
    CHECK(!negotiator.PendingReady());
    CHECK(negotiator.PendingRemote().ice.ufrag == "remote");
    CHECK(!negotiator.Commit());
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.PendingReady());
    CHECK(!negotiator.HasCurrent());
    CHECK(negotiator.CurrentLocal().medias.empty());
    CHECK(negotiator.CurrentRemote().medias.empty());
    CHECK(answer.type == SdpType::Answer);
    CHECK(negotiator.Commit());
    CHECK(negotiator.State() == SdpNegotiationState::Stable);
    CHECK(negotiator.HasCurrent());
    CHECK(!negotiator.PendingReady());
    CHECK(negotiator.CurrentLocal().ice.ufrag == "local");
    CHECK(negotiator.CurrentRemote().ice.ufrag == "remote");
    CHECK(!negotiator.Commit());

    const auto currentLocal = Snapshot(negotiator.CurrentLocal());
    const auto currentRemote = Snapshot(negotiator.CurrentRemote());
    auto replacement = Description("replacement");
    replacement.origin.sess_version = "1";
    CHECK(negotiator.ApplyOffer(replacement));
    CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
    CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.PendingReady());
    negotiator.Rollback();
    CHECK(negotiator.State() == SdpNegotiationState::Stable);
    CHECK(!negotiator.PendingReady());
    CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
    CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    negotiator.Rollback();
    CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
    CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
}

void PendingReplacementAndGlare()
{
    SdpNegotiator negotiator;
    Establish(negotiator);
    const auto currentLocal = Snapshot(negotiator.CurrentLocal());
    const auto currentRemote = Snapshot(negotiator.CurrentRemote());
    SdpSession answer;
    CHECK(negotiator.ApplyOffer(Description("first", 1)));
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.PendingReady());
    const auto firstPendingLocal = Snapshot(negotiator.PendingLocal());
    const auto firstPendingRemote = Snapshot(negotiator.PendingRemote());
    const auto firstAnswer = Snapshot(answer);
    auto invalidLocal = Capabilities();
    invalidLocal.dtls.fingerprints.clear();
    for (auto& media : invalidLocal.medias) media.dtls.fingerprints.clear();
    CHECK(!negotiator.CreateAnswer(invalidLocal, answer));
    CHECK(negotiator.PendingReady());
    CHECK(Snapshot(answer) == firstAnswer);
    CHECK(Snapshot(negotiator.PendingLocal()) == firstPendingLocal);
    CHECK(Snapshot(negotiator.PendingRemote()) == firstPendingRemote);
    auto second = Description("second", 2);
    second.medias[0].direction = MediaDirection::SendOnly;
    CHECK(negotiator.ApplyOffer(second));
    CHECK(!negotiator.PendingReady());
    CHECK(negotiator.PendingRemote().ice.ufrag == "second");
    CHECK(negotiator.PendingRemote().medias[0].direction == MediaDirection::SendOnly);
    const auto pendingRemote = Snapshot(negotiator.PendingRemote());
    auto invalid = second;
    invalid.medias[1].mid = invalid.medias[0].mid;
    CHECK(!negotiator.ApplyOffer(invalid));
    CHECK(!negotiator.LastError().empty());
    CHECK(Snapshot(negotiator.PendingRemote()) == pendingRemote);
    CHECK(negotiator.State() == SdpNegotiationState::HaveRemoteOffer);
    CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
    CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    negotiator.Rollback();

    SdpSession offer;
    CHECK(negotiator.CreateOffer(Description("local"), offer));
    CHECK(negotiator.State() == SdpNegotiationState::HaveLocalOffer);
    const auto pendingLocal = Snapshot(negotiator.PendingLocal());
    CHECK(!negotiator.ApplyOffer(second));
    CHECK(negotiator.State() == SdpNegotiationState::HaveLocalOffer);
    CHECK(Snapshot(negotiator.PendingLocal()) == pendingLocal);
    CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
    CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    negotiator.Rollback();
}

void AnswerCapabilitiesAndEffectiveViews()
{
    auto offered = Description("remote");
    offered.medias[0].direction = MediaDirection::SendOnly;
    offered.medias[1].direction = MediaDirection::RecvOnly;
    offered.medias[0].ssrcs.push_back({1234, {{"cname", "remote-audio"}}});
    offered.medias[0].msids = {"stream track-audio"};
    offered.medias[0].rtcpFeedback = {{"nack", ""}, {"transport-cc", ""}};
    Sdp::SetCodecs(offered.medias[0], {AudioCodec(111),
                   {0, "PCMU", 8000, 1, "", {}}});
    offered.medias[0].headerExtensions.push_back(
        {2, "urn:ietf:params:rtp-hdrext:ssrc-audio-level", MediaDirection::SendRecv, ""});

    auto local = Capabilities();
    local.medias[0].mid.clear();
    local.medias[1].mid.clear();
    local.medias[0].rtcpFeedback = {{"nack", ""}};
    OpusCodecConfig localOpus;
    localOpus.minPtime = 20;
    localOpus.maxAverageBitrate = 64000;
    localOpus.useInbandFec = false;
    Sdp::SetCodecs(local.medias[0], {SdpCodec::Opus(109, localOpus)});
    local.medias[1].ssrcs.push_back({5678, {{"cname", "local-video"}}});
    SdpNegotiator negotiator;
    SdpSession answer;
    CHECK(negotiator.ApplyOffer(offered));
    CHECK(negotiator.CreateAnswer(local, answer));
    const auto& audio = FindMedia(answer, "audio");
    const auto& video = FindMedia(answer, "video");
    CHECK(audio.port != 0 && video.port != 0);
    CHECK(audio.codecs.size() == 1 && audio.codecs[0].payloadType == 111);
    CHECK(audio.fmts == std::vector<std::string>{"111"});
    CHECK(audio.direction == MediaDirection::RecvOnly);
    CHECK(video.direction == MediaDirection::SendOnly);
    CHECK(audio.headerExtensions.size() == 1);
    CHECK(audio.headerExtensions[0].uri == kMidExtension);
    CHECK(audio.codecs[0].rtcpFeedback.size() == 1 &&
          audio.codecs[0].rtcpFeedback[0].type == "nack");
    CHECK(audio.codecs[0].fmtp.find("minptime=20") != std::string::npos);
    CHECK(audio.codecs[0].fmtp.find("maxaveragebitrate=64000") != std::string::npos);
    CHECK(audio.codecs[0].fmtp.find("useinbandfec=0") != std::string::npos);
    CHECK(audio.dtls.setup == DtlsSetup::Active || audio.dtls.setup == DtlsSetup::Passive);
    CHECK(negotiator.PendingLocal().medias[0].direction == MediaDirection::RecvOnly);
    const auto& remoteAudio = FindMedia(negotiator.PendingRemote(), "audio");
    CHECK(remoteAudio.direction == MediaDirection::SendOnly);
    CHECK(remoteAudio.codecs.size() == 1 && remoteAudio.codecs[0].payloadType == 111);
    CHECK(remoteAudio.ssrcs.size() == 1 && remoteAudio.ssrcs[0].ssrc == 1234);
    CHECK(remoteAudio.ssrcs[0].attributes[0].value == "remote-audio");
    CHECK(remoteAudio.msids == std::vector<std::string>{"stream track-audio"});
    CHECK(remoteAudio.ice.ufrag == "remote");
    CHECK(FindMedia(negotiator.PendingLocal(), "video").ssrcs[0].ssrc == 5678);
    CHECK(negotiator.Commit());
    CHECK(FindMedia(negotiator.CurrentRemote(), "audio").ssrcs[0].ssrc == 1234);
    const auto& remoteOpus = FindMedia(negotiator.CurrentRemote(), "audio").codecs[0];
    CHECK(remoteOpus.fmtp.find("minptime=10") != std::string::npos);
    CHECK(remoteOpus.fmtp.find("useinbandfec=1") != std::string::npos);
    CHECK(remoteOpus.fmtp.find("maxaveragebitrate") == std::string::npos);

    auto disabled = Capabilities();
    disabled.medias[0].mid.clear();
    disabled.medias[1].port = 0;
    auto genericVideo = disabled.medias[1];
    genericVideo.mid.clear();
    genericVideo.port = 9;
    disabled.medias.push_back(genericVideo);
    offered.origin.sess_version = "1";
    CHECK(negotiator.ApplyOffer(offered));
    CHECK(negotiator.CreateAnswer(disabled, answer));
    CHECK(FindMedia(answer, "audio").port != 0);
    CHECK(FindMedia(answer, "video").port == 0);
    CHECK(FindMedia(answer, "video").direction == MediaDirection::Inactive);
    CHECK(FindMedia(negotiator.PendingRemote(), "video").direction == MediaDirection::Inactive);
}

void LocalOfferAndAnswer()
{
    SdpNegotiator negotiator;
    auto local = Description("local");
    local.medias[0].direction = MediaDirection::SendRecv;
    Sdp::SetCodecs(local.medias[0], {AudioCodec(), {0, "PCMU", 8000, 1, "", {}}});
    local.medias[1].direction = MediaDirection::RecvOnly;
    auto remote = Capabilities("remote");
    remote.medias[0].direction = MediaDirection::RecvOnly;
    remote.medias[1].direction = MediaDirection::SendOnly;
    remote.medias[1].ssrcs.push_back({7777, {{"cname", "remote-video"}}});
    remote.medias[1].ssrcs.push_back({7778, {{"cname", "remote-video"}}});
    remote.medias[1].ssrcGroups.push_back({"SIM", {7777, 7778}});
    SdpSession offer;
    CHECK(negotiator.CreateOffer(local, offer));
    CHECK(offer.type == SdpType::Offer);
    CHECK(!negotiator.PendingReady());
    auto answer = AnswerFor(offer, remote);
    CHECK(negotiator.ApplyAnswer(answer));
    CHECK(negotiator.PendingReady());
    CHECK(!negotiator.HasCurrent());
    const auto& localAudio = FindMedia(negotiator.PendingLocal(), "audio");
    CHECK(localAudio.direction == MediaDirection::SendOnly);
    CHECK(localAudio.codecs.size() == 1 && localAudio.codecs[0].payloadType == 111);
    CHECK(FindMedia(negotiator.PendingRemote(), "audio").direction == MediaDirection::RecvOnly);
    const auto& remoteVideo = FindMedia(negotiator.PendingRemote(), "video");
    CHECK(remoteVideo.direction == MediaDirection::SendOnly);
    CHECK(remoteVideo.ssrcs.size() == 2 && remoteVideo.ssrcs[0].ssrc == 7777);
    CHECK(remoteVideo.ssrcGroups.size() == 1 && remoteVideo.ssrcGroups[0].ssrcs[0] == 7777);
    CHECK(remoteVideo.ice.ufrag == "remote");
    CHECK(FindMedia(negotiator.PendingLocal(), "video").direction == MediaDirection::RecvOnly);
    CHECK(negotiator.Commit());
    CHECK(FindMedia(negotiator.CurrentRemote(), "video").ssrcs[0].ssrc == 7777);
}

void OfferLayoutAndOrigin()
{
    SdpNegotiator negotiator;
    SdpSession initial;
    CHECK(negotiator.CreateOffer(Description("local"), initial));
    CHECK(!initial.origin.sess_id.empty() && !initial.origin.sess_version.empty());
    CHECK(negotiator.ApplyAnswer(AnswerFor(initial)));
    CHECK(negotiator.Commit());

    auto next = Description("local");
    next.origin.sess_id = "caller-tries-changing-origin";
    next.origin.sess_version = "0";
    auto added = next.medias[0];
    added.mid = "audio-new";
    next.medias = {added, next.medias[1]};
    next.bundle.mids = {"audio-new", "video"};
    SdpSession offer;
    CHECK(negotiator.CreateOffer(next, offer));
    CHECK(offer.medias.size() == 3);
    CHECK(offer.medias[0].mid == "audio");
    CHECK(offer.medias[0].port == 0);
    CHECK(offer.medias[0].direction == MediaDirection::Inactive);
    CHECK(offer.medias[1].mid == "video" && offer.medias[1].port != 0);
    CHECK(offer.medias[2].mid == "audio-new" && offer.medias[2].port != 0);
    CHECK(offer.origin.sess_id == initial.origin.sess_id);
    CHECK(std::stoull(offer.origin.sess_version) > std::stoull(initial.origin.sess_version));
    const auto priorVersion = std::stoull(offer.origin.sess_version);
    negotiator.Rollback();
    CHECK(negotiator.CreateOffer(Description("local"), offer));
    CHECK(offer.origin.sess_id == initial.origin.sess_id);
    CHECK(std::stoull(offer.origin.sess_version) > priorVersion);
    negotiator.Rollback();

    SdpNegotiator answerer;
    SdpSession firstAnswer, secondAnswer;
    CHECK(answerer.ApplyOffer(Description("remote")));
    CHECK(answerer.CreateAnswer(Capabilities(), firstAnswer));
    CHECK(answerer.Commit());
    CHECK(answerer.ApplyOffer(Description("remote", 1)));
    CHECK(answerer.CreateAnswer(Capabilities(), secondAnswer));
    CHECK(firstAnswer.origin.sess_id == secondAnswer.origin.sess_id);
    CHECK(std::stoull(secondAnswer.origin.sess_version) >
          std::stoull(firstAnswer.origin.sess_version));
}

void RejectRemoteWithoutMutation(SdpNegotiator& negotiator, const SdpSession& description)
{
    const auto state = negotiator.State();
    const auto hasCurrent = negotiator.HasCurrent();
    const auto ready = negotiator.PendingReady();
    const auto currentLocal = Snapshot(negotiator.CurrentLocal());
    const auto currentRemote = Snapshot(negotiator.CurrentRemote());
    const auto pendingLocal = Snapshot(negotiator.PendingLocal());
    const auto pendingRemote = Snapshot(negotiator.PendingRemote());
    CHECK(description.type == SdpType::Answer ? !negotiator.ApplyAnswer(description)
                                             : !negotiator.ApplyOffer(description));
    CHECK(!negotiator.LastError().empty());
    CHECK(negotiator.State() == state);
    CHECK(negotiator.HasCurrent() == hasCurrent);
    CHECK(negotiator.PendingReady() == ready);
    CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
    CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    CHECK(Snapshot(negotiator.PendingLocal()) == pendingLocal);
    CHECK(Snapshot(negotiator.PendingRemote()) == pendingRemote);
}

void RemoteOriginFreshness()
{
    SdpNegotiator negotiator;
    Establish(negotiator);
    const auto staleOffer = Description("remote");
    auto changedSession = Description("remote", 1);
    changedSession.origin.sess_id = "3001";
    auto missingOrigin = Description("remote", 1);
    missingOrigin.origin = {};
    RejectRemoteWithoutMutation(negotiator, staleOffer);
    RejectRemoteWithoutMutation(negotiator, changedSession);
    RejectRemoteWithoutMutation(negotiator, missingOrigin);

    SdpSession answer;
    CHECK(negotiator.ApplyOffer(Description("remote", 4)));
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.PendingReady());
    RejectRemoteWithoutMutation(negotiator, staleOffer);
    RejectRemoteWithoutMutation(negotiator, changedSession);
    RejectRemoteWithoutMutation(negotiator, missingOrigin);
    // Freshness is relative to committed CurrentRemote, not an uncommitted
    // replacement. Version 2 may replace pending version 4 when current is 0.
    CHECK(negotiator.ApplyOffer(Description("remote", 2)));
    CHECK(!negotiator.PendingReady());
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.Commit());
    CHECK(negotiator.CurrentRemote().origin.sess_version == "2");
    RejectRemoteWithoutMutation(negotiator, Description("remote", 2));
    RejectRemoteWithoutMutation(negotiator, Description("remote", 1));

    SdpSession offer;
    CHECK(negotiator.CreateOffer(Description("local"), offer));
    auto freshAnswer = AnswerFor(offer);
    freshAnswer.origin.sess_id = "2001";
    freshAnswer.origin.sess_version = "3";
    auto staleAnswer = freshAnswer;
    staleAnswer.origin.sess_version = "2";
    auto changedAnswerSession = freshAnswer;
    changedAnswerSession.origin.sess_id = "3001";
    auto missingAnswerOrigin = freshAnswer;
    missingAnswerOrigin.origin = {};
    RejectRemoteWithoutMutation(negotiator, staleAnswer);
    staleAnswer.origin.sess_version = "1";
    RejectRemoteWithoutMutation(negotiator, staleAnswer);
    RejectRemoteWithoutMutation(negotiator, changedAnswerSession);
    RejectRemoteWithoutMutation(negotiator, missingAnswerOrigin);
    CHECK(negotiator.ApplyAnswer(freshAnswer));
    CHECK(negotiator.PendingReady());
    RejectRemoteWithoutMutation(negotiator, staleAnswer);
    CHECK(negotiator.Commit());
    CHECK(negotiator.CurrentRemote().origin.sess_version == "3");
    RejectRemoteWithoutMutation(negotiator, freshAnswer); // Late answer in Stable.

    CHECK(negotiator.CreateOffer(Description("local"), offer));
    // A late answer from the committed transaction must not answer a new
    // pending offer, even if its media layout would otherwise be compatible.
    RejectRemoteWithoutMutation(negotiator, freshAnswer);
    auto retryableAnswer = AnswerFor(offer);
    retryableAnswer.origin.sess_id = "2001";
    retryableAnswer.origin.sess_version = "4";
    CHECK(negotiator.ApplyAnswer(retryableAnswer));
    negotiator.Rollback();
    CHECK(negotiator.CurrentRemote().origin.sess_version == "3");
    CHECK(negotiator.CreateOffer(Description("local"), offer));
    // Rollback does not commit a remote version. Matching responses to a
    // particular rolled-back offer requires the signaling negotiation_id.
    CHECK(negotiator.ApplyAnswer(retryableAnswer));
    CHECK(negotiator.Commit());
    CHECK(negotiator.CurrentRemote().origin.sess_version == "4");
}

void LegacyEmptyOrigins()
{
    // Typed callers predating the normalized SDP model do not supply o=.
    SdpNegotiator negotiator;
    auto remote = Description("remote");
    remote.origin = {};
    SdpSession answer;
    CHECK(negotiator.ApplyOffer(remote));
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.Commit());
    CHECK(negotiator.CurrentRemote().origin.sess_id.empty());
    CHECK(negotiator.CurrentRemote().origin.sess_version.empty());
    CHECK(negotiator.ApplyOffer(remote));
    CHECK(negotiator.CreateAnswer(Capabilities(), answer));
    CHECK(negotiator.Commit());

    SdpSession offer;
    CHECK(negotiator.CreateOffer(Description("local"), offer));
    answer = AnswerFor(offer);
    answer.origin = {};
    CHECK(negotiator.ApplyAnswer(answer));
    CHECK(negotiator.Commit());
    CHECK(negotiator.CurrentRemote().origin.sess_id.empty());
    CHECK(negotiator.CurrentRemote().origin.sess_version.empty());
}

using Mutation = std::function<void(SdpSession&)>;

void RejectAnswer(const std::string& name, const Mutation& mutate,
                  const Mutation& prepare = {})
{
    try
    {
        SdpNegotiator negotiator;
        Establish(negotiator);
        const auto currentLocal = Snapshot(negotiator.CurrentLocal());
        const auto currentRemote = Snapshot(negotiator.CurrentRemote());
        auto local = Description("local");
        if (prepare) prepare(local);
        SdpSession offer;
        CHECK(negotiator.CreateOffer(local, offer));
        const auto pendingLocal = Snapshot(negotiator.PendingLocal());
        const auto pendingRemote = Snapshot(negotiator.PendingRemote());
        auto remote = Capabilities("remote");
        for (size_t i = 0; i < remote.medias.size(); ++i)
            if (!offer.medias[i].codecs.empty())
                Sdp::SetCodecs(remote.medias[i], offer.medias[i].codecs);
        auto valid = AnswerFor(offer, remote);
        valid.origin.sess_id = negotiator.CurrentRemote().origin.sess_id;
        valid.origin.sess_version = std::to_string(
            std::stoull(negotiator.CurrentRemote().origin.sess_version) + 1);
        auto invalid = valid;
        mutate(invalid);
        CHECK(!negotiator.ApplyAnswer(invalid));
        CHECK(!negotiator.LastError().empty());
        CHECK(negotiator.State() == SdpNegotiationState::HaveLocalOffer);
        CHECK(!negotiator.PendingReady());
        CHECK(Snapshot(negotiator.PendingLocal()) == pendingLocal);
        CHECK(Snapshot(negotiator.PendingRemote()) == pendingRemote);
        CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
        CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
        CHECK(negotiator.ApplyAnswer(valid));
        CHECK(negotiator.PendingReady());
        negotiator.Rollback();
        CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
        CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(name + ": " + error.what());
    }
}

void StrictAnswers()
{
    RejectAnswer("new payload", [](SdpSession& answer) {
        Sdp::SetCodecs(answer.medias[0], {AudioCodec(112)});
    });
    RejectAnswer("codec identity", [](SdpSession& answer) {
        auto codec = answer.medias[0].codecs[0];
        codec.encodingName = "PCMA";
        codec.clockRate = 8000;
        codec.channels = 1;
        codec.fmtp.clear();
        Sdp::SetCodecs(answer.medias[0], {codec});
    });
    RejectAnswer("clock rate", [](SdpSession& answer) {
        auto codec = answer.medias[0].codecs[0];
        codec.clockRate = 16000;
        Sdp::SetCodecs(answer.medias[0], {codec});
    });
    RejectAnswer("channel count", [](SdpSession& answer) {
        auto codec = answer.medias[0].codecs[0];
        codec.channels = 1;
        Sdp::SetCodecs(answer.medias[0], {codec});
    });
    RejectAnswer("unknown fmtp", [](SdpSession& answer) {
        auto codec = answer.medias[0].codecs[0];
        codec.fmtp += ";unoffered-parameter=1";
        Sdp::SetCodecs(answer.medias[0], {codec});
    });
    RejectAnswer("generic fmtp change", [](SdpSession& answer) {
        auto codec = answer.medias[1].codecs[0];
        codec.fmtp = "x-format=2";
        Sdp::SetCodecs(answer.medias[1], {codec});
    }, [](SdpSession& offer) {
        Sdp::SetCodecs(offer.medias[1], {{96, "VP8", 90000, 0, "x-format=1", {}}});
    });
    RejectAnswer("H264 level increase without asymmetry", [](SdpSession& answer) {
        Sdp::SetCodecs(answer.medias[1], {VideoCodec(96, 0x42e028)});
    });
    RejectAnswer("H264 profile change", [](SdpSession& answer) {
        Sdp::SetCodecs(answer.medias[1], {VideoCodec(96, 0x64001f)});
    });
    RejectAnswer("H264 packetization change", [](SdpSession& answer) {
        H264CodecConfig config;
        config.packetizationMode = 0;
        config.levelAsymmetryAllowed = false;
        Sdp::SetCodecs(answer.medias[1], {SdpCodec::H264(96, config)});
    });
    RejectAnswer("codec feedback addition", [](SdpSession& answer) {
        answer.medias[1].codecs[0].rtcpFeedback.push_back({"transport-cc", ""});
    });
    RejectAnswer("wildcard feedback addition", [](SdpSession& answer) {
        answer.medias[1].rtcpFeedback.push_back({"transport-cc", ""});
    });
    RejectAnswer("header extension addition", [](SdpSession& answer) {
        answer.medias[0].headerExtensions.push_back(
            {2, "urn:ietf:params:rtp-hdrext:ssrc-audio-level", MediaDirection::SendRecv, ""});
    });
    RejectAnswer("header extension ID change", [](SdpSession& answer) {
        answer.medias[0].headerExtensions[0].id = 2;
    });
    RejectAnswer("sendonly answer cannot receive and send", [](SdpSession& answer) {
        answer.medias[0].direction = MediaDirection::SendRecv;
    }, [](SdpSession& offer) {
        offer.medias[0].direction = MediaDirection::SendOnly;
    });
    RejectAnswer("recvonly answer cannot receive", [](SdpSession& answer) {
        answer.medias[0].direction = MediaDirection::RecvOnly;
    }, [](SdpSession& offer) {
        offer.medias[0].direction = MediaDirection::RecvOnly;
    });
    RejectAnswer("inactive offer cannot send", [](SdpSession& answer) {
        answer.medias[0].direction = MediaDirection::SendOnly;
    }, [](SdpSession& offer) {
        offer.medias[0].direction = MediaDirection::Inactive;
    });
    RejectAnswer("rejected offer cannot be activated", [](SdpSession& answer) {
        answer.medias[1].port = 9;
        answer.medias[1].direction = MediaDirection::SendRecv;
    }, [](SdpSession& offer) {
        offer.medias.pop_back();
        offer.bundle.mids.pop_back();
    });
    RejectAnswer("answer DTLS actpass", [](SdpSession& answer) {
        answer.dtls.setup = DtlsSetup::ActPass;
        for (auto& media : answer.medias) media.dtls.setup = DtlsSetup::ActPass;
    });
    RejectAnswer("answer DTLS holdconn", [](SdpSession& answer) {
        answer.dtls.setup = DtlsSetup::HoldConn;
        for (auto& media : answer.medias) media.dtls.setup = DtlsSetup::HoldConn;
    });
    RejectAnswer("BUNDLE unknown MID", [](SdpSession& answer) {
        answer.bundle.mids.push_back("unoffered");
    });
    RejectAnswer("mline append", [](SdpSession& answer) {
        auto extra = answer.medias[0];
        extra.mid = "new";
        answer.medias.push_back(extra);
        answer.bundle.mids.push_back("new");
    });
    RejectAnswer("mline removed", [](SdpSession& answer) {
        answer.medias.pop_back();
        answer.bundle.mids.pop_back();
    });
    RejectAnswer("mline reordered", [](SdpSession& answer) {
        std::swap(answer.medias[0], answer.medias[1]);
    });
    RejectAnswer("mline MID changed", [](SdpSession& answer) {
        answer.medias[0].mid = "renamed";
        answer.bundle.mids[0] = "renamed";
    });
    RejectAnswer("mline kind changed", [](SdpSession& answer) {
        answer.medias[0].media = "video";
    });
    RejectAnswer("mline protocol changed", [](SdpSession& answer) {
        answer.medias[0].proto = "RTP/AVP";
    });
}

void LegalCodecAnswers()
{
    for (bool asymmetry : {false, true})
    {
        SdpNegotiator negotiator;
        auto local = Description("local");
        Sdp::SetCodecs(local.medias[1], {VideoCodec(96, 0x42e01f, asymmetry)});
        auto remote = Capabilities("remote");
        Sdp::SetCodecs(remote.medias[1],
                       {VideoCodec(110, asymmetry ? 0x42e028 : 0x42e015, asymmetry)});
        OpusCodecConfig opus;
        opus.maxAverageBitrate = 32000;
        opus.stereo = false;
        Sdp::SetCodecs(remote.medias[0], {SdpCodec::Opus(109, opus)});
        SdpSession offer;
        CHECK(negotiator.CreateOffer(local, offer));
        const auto answer = AnswerFor(offer, remote);
        CHECK(answer.medias[1].port != 0);
        CHECK(answer.medias[1].codecs[0].fmtp.find(
            asymmetry ? "profile-level-id=42e028" : "profile-level-id=42e015") != std::string::npos);
        CHECK(negotiator.ApplyAnswer(answer));
        CHECK(negotiator.Commit());
        CHECK(negotiator.CurrentRemote().medias[0].codecs[0].fmtp.find(
                  "maxaveragebitrate=32000") != std::string::npos);
        const auto& localOpus = negotiator.CurrentLocal().medias[0].codecs[0];
        CHECK(localOpus.fmtp.find("maxaveragebitrate") == std::string::npos);
        CHECK(localOpus.fmtp.find("minptime=10") != std::string::npos);
        CHECK(localOpus.fmtp.find("useinbandfec=1") != std::string::npos);
        CHECK(negotiator.CurrentLocal().medias[1].codecs[0].fmtp.find(
            asymmetry ? "profile-level-id=42e01f" : "profile-level-id=42e015") != std::string::npos);
        CHECK(negotiator.CurrentRemote().medias[1].codecs[0].fmtp.find(
            asymmetry ? "profile-level-id=42e028" : "profile-level-id=42e015") != std::string::npos);
    }
}

void InvalidOffers()
{
    const std::vector<std::pair<std::string, Mutation>> mutations = {
        {"missing mid", [](SdpSession& offer) { offer.medias[0].mid.clear(); }},
        {"duplicate mid", [](SdpSession& offer) { offer.medias[1].mid = "audio"; }},
        {"missing bundle", [](SdpSession& offer) { offer.bundle.mids.clear(); }},
        {"missing rtcp mux", [](SdpSession& offer) { offer.medias[0].rtcpMux = false; }},
        {"invalid transport", [](SdpSession& offer) { offer.medias[0].proto = "RTP/AVP"; }},
        {"short ufrag", [](SdpSession& offer) {
            offer.ice.ufrag = "bad";
            for (auto& media : offer.medias) media.ice.ufrag = "bad";
        }},
        {"short password", [](SdpSession& offer) {
            offer.ice.pwd = "short";
            for (auto& media : offer.medias) media.ice.pwd = "short";
        }},
        {"missing fingerprint", [](SdpSession& offer) {
            offer.dtls.fingerprints.clear();
            for (auto& media : offer.medias) media.dtls.fingerprints.clear();
        }},
        {"missing fingerprint value", [](SdpSession& offer) {
            offer.dtls.fingerprints[0].value.clear();
            for (auto& media : offer.medias) media.dtls.fingerprints[0].value.clear();
        }}
    };
    for (const auto& mutation : mutations)
    {
        SdpNegotiator negotiator;
        Establish(negotiator);
        const auto currentLocal = Snapshot(negotiator.CurrentLocal());
        const auto currentRemote = Snapshot(negotiator.CurrentRemote());
        auto invalid = Description("remote", 1);
        mutation.second(invalid);
        if (negotiator.ApplyOffer(invalid))
            throw std::runtime_error("Accepted invalid offer: " + mutation.first);
        CHECK(negotiator.State() == SdpNegotiationState::Stable);
        CHECK(Snapshot(negotiator.CurrentLocal()) == currentLocal);
        CHECK(Snapshot(negotiator.CurrentRemote()) == currentRemote);
    }
}
} // namespace

int main()
{
    try
    {
        Transactions();
        PendingReplacementAndGlare();
        AnswerCapabilitiesAndEffectiveViews();
        LocalOfferAndAnswer();
        OfferLayoutAndOrigin();
        RemoteOriginFreshness();
        LegacyEmptyOrigins();
        StrictAnswers();
        LegalCodecAnswers();
        InvalidOffers();
        std::cout << "SDP negotiation transactions, effective views and validation tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SdpNegotiator test failed: " << error.what() << '\n';
        return 1;
    }
}
