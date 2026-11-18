import {test} from 'node:test';
import assert from 'node:assert/strict';
import {RtcStatsSampler, sumMetric, readPublishSettings} from './quality-stats.js';

const inbound = (changes = {}) => ({id: 'v1', type: 'inbound-rtp', kind: 'video', ssrc: 1, timestamp: 1000,
  bytesReceived: 1000, packetsReceived: 10, packetsLost: 1, framesDecoded: 5,
  jitter: 0.012, jitterBufferDelay: 0.1, jitterBufferEmittedCount: 5, ...changes});
const report = (...items) => new Map(items.map(item => [item.id, item]));

test('first sample distinguishes unknown rates from supported instantaneous measurements', () => {
  const sample = new RtcStatsSampler().sample(report(inbound())).streams[0];
  assert.equal(sample.bitrate_bps, null); assert.equal(sample.fps, null);
  assert.equal(sample.loss_rate, null); assert.equal(sample.jitter_ms, 12);
  assert.equal(sample.jitter_buffer_ms, null); assert.equal(sample.freezes_total, null);
});
test('rates use each stream timestamp and losses use interval packets, not lifetime totals', () => {
  const sampler = new RtcStatsSampler(); sampler.sample(report(inbound()));
  const sample = sampler.sample(report(inbound({timestamp: 3000, bytesReceived: 201000,
    packetsReceived: 109, packetsLost: 2, framesDecoded: 65,
    jitterBufferDelay: 0.7, jitterBufferEmittedCount: 65}))).streams[0];
  assert.equal(sample.bitrate_bps, 800000); assert.equal(sample.fps, 30);
  assert.equal(sample.packets_per_second, 49.5); assert.equal(sample.loss_rate, 0.01);
  assert.ok(Math.abs(sample.jitter_buffer_ms - 10) < 1e-8);
});
test('late loss corrections and counter resets do not generate negative rates', () => {
  const sampler = new RtcStatsSampler(); sampler.sample(report(inbound()));
  const sample = sampler.sample(report(inbound({timestamp: 2000, bytesReceived: 10,
    packetsReceived: 20, packetsLost: -1, framesDecoded: 1}))).streams[0];
  assert.equal(sample.bitrate_bps, null); assert.equal(sample.fps, null);
  assert.equal(sample.loss_rate, null); assert.equal(sample.packets_lost_total, -1);
});
test('changed SSRC, removed tracks, and backwards clocks start fresh intervals', () => {
  for (const changes of [{ssrc: 2, timestamp: 2000}, {timestamp: 900}, {timestamp: 1000}]) {
    const sampler = new RtcStatsSampler(); sampler.sample(report(inbound()));
    assert.equal(sampler.sample(report(inbound(changes))).streams[0].bitrate_bps, null);
  }
  const sampler = new RtcStatsSampler(); sampler.sample(report(inbound())); sampler.sample(report());
  assert.equal(sampler.sample(report(inbound({timestamp: 2000}))).streams[0].fps, null);
});
test('audio/video remain separate and missing metrics are not fabricated as zero', () => {
  const sampler = new RtcStatsSampler();
  const audio = {id: 'a1', type: 'inbound-rtp', kind: 'audio', ssrc: 2, timestamp: 1000, bytesReceived: 100};
  sampler.sample(report(inbound(), audio));
  const sample = sampler.sample(report(inbound({timestamp: 2000, bytesReceived: 101000}),
    {...audio, timestamp: 2000, bytesReceived: 8100}));
  assert.equal(sample.streams[0].bitrate_bps, 800000); assert.equal(sample.streams[1].bitrate_bps, 64000);
  assert.equal(sample.streams[1].fps, null); assert.equal(sample.streams[1].jitter_ms, null);
  assert.equal(sumMetric(sample.streams, 'bitrate_bps'), 864000);
  assert.equal(sumMetric([], 'bitrate_bps'), null);
});
test('outbound encode costs and selected ICE RTT have distinct meanings and units', () => {
  const sampler = new RtcStatsSampler();
  const sent = {id: 'out', type: 'outbound-rtp', kind: 'video', ssrc: 3, timestamp: 1000,
    bytesSent: 1000, framesEncoded: 10, totalEncodeTime: 0.1, codecId: 'codec', qualityLimitationReason: 'cpu'};
  sampler.sample(report(sent));
  const sample = sampler.sample(report({...sent, timestamp: 2000, bytesSent: 101000, framesEncoded: 40, totalEncodeTime: 0.4},
    {id: 'transport', type: 'transport', selectedCandidatePairId: 'pair'},
    {id: 'pair', currentRoundTripTime: 0.08, localCandidateId: 'local'},
    {id: 'local', candidateType: 'relay'}, {id: 'codec', mimeType: 'video/H264'}));
  assert.equal(sample.streams[0].bitrate_bps, 800000);
  assert.ok(Math.abs(sample.streams[0].encode_ms_per_frame - 10) < 1e-8);
  assert.equal(sample.streams[0].codec, 'video/H264'); assert.equal(sample.streams[0].loss_rate, null);
  assert.equal(sample.ice_rtt_ms, 80); assert.equal(sample.local_candidate, 'relay');
});
test('idle streams report zero bitrate and fps after a valid measurement window', () => {
  const sampler = new RtcStatsSampler(); sampler.sample(report(inbound()));
  const sample = sampler.sample(report(inbound({timestamp: 2000}))).streams[0];
  assert.equal(sample.bitrate_bps, 0); assert.equal(sample.fps, 0); assert.equal(sample.loss_rate, null);
});
test('publish requests are bounded and convert kbps to bps', () => {
  assert.deepEqual(readPublishSettings('1280x720', '60', '8000'),
    {width: 1280, height: 720, fps: 60, max_bitrate_bps: 8000000});
  for (const args of [['bad', 30, 800], ['640x360', 0, 800], ['640x360', 121, 800],
    ['640x360', 30, ''], ['640x360', 30, Infinity], ['640x360', 30, 50001]])
    assert.throws(() => readPublishSettings(...args));
});
