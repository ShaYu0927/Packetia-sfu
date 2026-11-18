// Browser measurements only; these values are not the SFU bandwidth estimate.
const number = value => Number.isFinite(value) ? value : null;
const nonnegative = value => Number.isFinite(value) && value >= 0 ? value : null;
function delta(current, previous, field) {
  const now = nonnegative(current[field]), before = nonnegative(previous?.[field]);
  return now !== null && before !== null && now >= before ? now - before : null;
}
function rate(current, previous, field, multiplier, elapsed) {
  const change = delta(current, previous, field);
  return change !== null && elapsed > 0 ? change * multiplier / elapsed : null;
}

export class RtcStatsSampler {
  previous = new Map();

  sample(report) {
    const next = new Map(), streams = [];
    let pair;
    for (const stat of report.values())
      if (stat.type === 'transport' && stat.selectedCandidatePairId)
        pair = report.get(stat.selectedCandidatePairId);
    for (const stat of report.values()) {
      if (!['inbound-rtp', 'outbound-rtp'].includes(stat.type) || stat.isRemote) continue;
      const kind = stat.kind || stat.mediaType;
      if (!['audio', 'video'].includes(kind)) continue;
      const previous = this.previous.get(stat.id);
      const elapsed = previous ? stat.timestamp - previous.timestamp : 0;
      // Reusing an ID for another SSRC must start a new measurement window.
      const before = previous?.ssrc === stat.ssrc && elapsed > 0 ? previous : undefined;
      const inbound = stat.type === 'inbound-rtp';
      const byteField = inbound ? 'bytesReceived' : 'bytesSent';
      const packetField = inbound ? 'packetsReceived' : 'packetsSent';
      const frameField = inbound ? 'framesDecoded' : 'framesEncoded';
      const packets = delta(stat, before, packetField);
      const frames = delta(stat, before, frameField);
      let loss = null;
      // packetsLost can decrease after late arrivals. A correction is not a new loss sample.
      if (inbound && before && Number.isFinite(stat.packetsLost) && Number.isFinite(before.packetsLost)) {
        const lost = stat.packetsLost - before.packetsLost;
        if (lost >= 0 && packets !== null && packets + lost > 0) loss = lost / (packets + lost);
      }
      const jitterDelay = delta(stat, before, 'jitterBufferDelay');
      const emitted = delta(stat, before, 'jitterBufferEmittedCount');
      const encodeTime = delta(stat, before, 'totalEncodeTime');
      const codec = report.get(stat.codecId);
      streams.push({id: stat.id, ssrc: stat.ssrc, direction: inbound ? 'receive' : 'send', kind,
        codec: codec?.mimeType || null, timestamp_ms: number(stat.timestamp),
        bitrate_bps: rate(stat, before, byteField, 8000, elapsed),
        packets_per_second: rate(stat, before, packetField, 1000, elapsed),
        fps: kind === 'video' ? rate(stat, before, frameField, 1000, elapsed) : null,
        width: nonnegative(stat.frameWidth), height: nonnegative(stat.frameHeight),
        loss_rate: loss, packets_lost_total: number(stat.packetsLost),
        jitter_ms: inbound && nonnegative(stat.jitter) !== null ? stat.jitter * 1000 : null,
        jitter_buffer_ms: emitted > 0 && jitterDelay !== null ? jitterDelay * 1000 / emitted : null,
        encode_ms_per_frame: frames > 0 && encodeTime !== null ? encodeTime * 1000 / frames : null,
        freezes_total: nonnegative(stat.freezeCount), nack_total: nonnegative(stat.nackCount),
        pli_total: nonnegative(stat.pliCount), quality_limitation: stat.qualityLimitationReason || null,
        bytes_total: nonnegative(stat[byteField]), frames_total: nonnegative(stat[frameField])});
      next.set(stat.id, {...stat});
    }
    this.previous = next;
    return {streams, ice_rtt_ms: nonnegative(pair?.currentRoundTripTime) !== null ? pair.currentRoundTripTime * 1000 : null,
      local_candidate: report.get(pair?.localCandidateId)?.candidateType || null,
      remote_candidate: report.get(pair?.remoteCandidateId)?.candidateType || null};
  }
}

export function sumMetric(streams, field) {
  const values = streams.map(stream => stream[field]).filter(Number.isFinite);
  return values.length ? values.reduce((sum, value) => sum + value, 0) : null;
}

export function readPublishSettings(resolution, fpsValue, bitrateValue) {
  const resolutions = {'640x360': [640, 360], '1280x720': [1280, 720], '1920x1080': [1920, 1080]};
  const size = resolutions[resolution], fps = Number(fpsValue), kbps = Number(bitrateValue);
  if (!size || !Number.isInteger(fps) || fps < 1 || fps > 120 ||
      !Number.isInteger(kbps) || kbps < 100 || kbps > 50000)
    throw new Error('Use 1–120 fps and a bitrate limit of 100–50000 kbps');
  return {width: size[0], height: size[1], fps, max_bitrate_bps: kbps * 1000};
}
