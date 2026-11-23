// Real public-SFU media test. Credentials are supplied at runtime, never exported.
import assert from 'node:assert/strict';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {get as httpsGet, request as httpsRequest} from 'node:https';
import {performance} from 'node:perf_hooks';
import {chromium} from 'playwright';
import {analyzePublicTransport, renderPublicTransportReport} from './public-transport-report.mjs';

const directory = dirname(fileURLToPath(import.meta.url));
const url = process.env.PACKETIA_PUBLIC_TEST_URL || 'https://45.76.105.146:18443';
const password = process.env.PACKETIA_PUBLIC_TEST_PASSWORD;
const output = process.env.PACKETIA_PUBLIC_TEST_OUTPUT || resolve(directory, 'artifacts',
  `public-transport-${new Date().toISOString().replaceAll(':', '-').replaceAll('.', '-')}`);
const seconds = Number(process.env.PACKETIA_PUBLIC_TEST_SECONDS || 20);
assert.ok(password, 'Set PACKETIA_PUBLIC_TEST_PASSWORD to the existing test login password');
assert.ok(Number.isInteger(seconds) && seconds >= 5 && seconds <= 120, 'Use 5–120 samples per phase');
assert.equal(new URL(url).protocol, 'https:');
await mkdir(output, {recursive: true});
const result = {schema_version: 1, started_at: new Date().toISOString(), endpoint: url,
  scope: 'One simulated camera/audio publisher and one viewer on the same Mac; media traverses the public SFU. No network impairment injected.',
  passed: false, phases: [], browser_errors: [], checks: {}, artifacts: output};
const servers = [], browsers = [], pages = [];
const sleep = ms => new Promise(resolveWait => setTimeout(resolveWait, ms));
const mean = values => {
  const valid = values.filter(Number.isFinite);
  return valid.length ? valid.reduce((sum, value) => sum + value, 0) / valid.length : null;
};
const percentile = (values, fraction) => {
  const valid = values.filter(Number.isFinite).sort((a, b) => a - b);
  return valid.length ? valid[Math.min(valid.length - 1, Math.ceil(valid.length * fraction) - 1)] : null;
};
const diff = (last, first) => Number.isFinite(last) && Number.isFinite(first) && last >= first ? last - first : null;
const fmt = (value, digits = 1) => Number.isFinite(value) ? value.toFixed(digits) : '—';

async function until(operation, label, timeout = 45000) {
  const deadline = performance.now() + timeout;
  do {
    const value = await operation();
    if (value) return value;
    await sleep(500);
  } while (performance.now() < deadline);
  throw new Error(`Timed out: ${label}`);
}
async function snapshot(page) {
  return page.evaluate(async () => {
    const state = await window.demo.stats();
    const connections = [];
    for (const pc of window.__packetiaTestConnections || []) {
      if (pc.connectionState === 'closed') continue;
      const report = await pc.getStats();
      const values = [...report.values()];
      const transport = values.find(value => value.type === 'transport' && value.selectedCandidatePairId);
      const pair = transport && report.get(transport.selectedCandidatePairId);
      const candidate = id => {
        const value = report.get(id);
        return value && {type: value.candidateType, address: value.address, port: value.port,
          protocol: value.protocol, relay_protocol: value.relayProtocol};
      };
      const fields = ['id', 'type', 'kind', 'ssrc', 'timestamp', 'bytesSent', 'bytesReceived', 'packetsSent',
        'packetsReceived', 'packetsLost', 'framesEncoded', 'framesDecoded', 'nackCount', 'pliCount', 'firCount',
        'freezeCount', 'totalFreezesDuration', 'jitter', 'roundTripTime', 'fractionLost', 'totalRoundTripTime',
        'roundTripTimeMeasurements', 'jitterBufferDelay', 'jitterBufferEmittedCount', 'frameWidth', 'frameHeight'];
      const streams = values.filter(value => ['inbound-rtp', 'outbound-rtp', 'remote-inbound-rtp'].includes(value.type))
        .map(value => Object.fromEntries(fields.filter(field => value[field] !== undefined).map(field => [field, value[field]])));
      const twcc = sdp => [...(sdp || '').matchAll(/^a=extmap:(\d+)(?:\/\w+)?\s+\S*transport-wide-cc\S*/gm)].map(match => Number(match[1]));
      connections.push({state: pc.connectionState, ice: pc.iceConnectionState, streams,
        selected_pair: pair && {state: pair.state, nominated: pair.nominated, rtt_ms: pair.currentRoundTripTime * 1000,
          bytes_sent: pair.bytesSent, bytes_received: pair.bytesReceived,
          local: candidate(pair.localCandidateId), remote: candidate(pair.remoteCandidateId)},
        twcc_local_extension_ids: twcc(pc.localDescription?.sdp),
        twcc_remote_extension_ids: twcc(pc.remoteDescription?.sdp)});
    }
    return {state, connections};
  });
}
function streamDelta(samples, side, type, kind, field) {
  const points = samples.map(sample => sample[side].connections.flatMap(pc => pc.streams)
    .find(stream => stream.type === type && stream.kind === kind));
  const first = points.find(Boolean), last = points.findLast(Boolean);
  return first && last && first.ssrc === last.ssrc ? diff(last[field], first[field]) : null;
}
function rate(samples, side, type, field, multiplier) {
  const points = samples.map(sample => sample[side].connections.flatMap(pc => pc.streams)
    .find(stream => stream.type === type && stream.kind === 'video'));
  const first = points.find(Boolean), last = points.findLast(Boolean);
  if (!first || !last || first.ssrc !== last.ssrc || last.timestamp <= first.timestamp) return null;
  const change = diff(last[field], first[field]);
  return change === null ? null : change * multiplier / (last.timestamp - first.timestamp);
}
function summarize(phase) {
  const {samples} = phase;
  if (!samples.length) return {};
  const rtts = samples.map(sample => sample.receive.state.subscribe?.metrics?.ice_rtt_ms);
  const received = streamDelta(samples, 'receive', 'inbound-rtp', 'video', 'packetsReceived');
  const lost = streamDelta(samples, 'receive', 'inbound-rtp', 'video', 'packetsLost');
  return {sample_count: samples.length,
    measured_seconds: (Date.parse(samples.at(-1).at) - Date.parse(samples[0].at)) / 1000,
    send_video_kbps: rate(samples, 'send', 'outbound-rtp', 'bytesSent', 8),
    receive_video_kbps: rate(samples, 'receive', 'inbound-rtp', 'bytesReceived', 8),
    encode_fps: rate(samples, 'send', 'outbound-rtp', 'framesEncoded', 1000),
    decode_fps: rate(samples, 'receive', 'inbound-rtp', 'framesDecoded', 1000),
    video_received_packets: received, video_lost_packets_delta: lost,
    receive_video_loss_percent: received !== null && lost !== null && received + lost > 0 ? lost * 100 / (received + lost) : null,
    ice_rtt_mean_ms: mean(rtts), ice_rtt_p95_ms: percentile(rtts, 0.95),
    receive_jitter_mean_ms: mean(samples.flatMap(sample => sample.receive.state.subscribe?.metrics?.streams || [])
      .filter(stream => stream.kind === 'video').map(stream => stream.jitter_ms)),
    jitter_buffer_mean_ms: mean(samples.flatMap(sample => sample.receive.state.subscribe?.metrics?.streams || [])
      .filter(stream => stream.kind === 'video').map(stream => stream.jitter_buffer_ms)),
    receiver_nack_delta: streamDelta(samples, 'receive', 'inbound-rtp', 'video', 'nackCount'),
    receiver_pli_delta: streamDelta(samples, 'receive', 'inbound-rtp', 'video', 'pliCount'),
    receiver_freeze_delta: streamDelta(samples, 'receive', 'inbound-rtp', 'video', 'freezeCount'),
    decoded_frames_delta: streamDelta(samples, 'receive', 'inbound-rtp', 'video', 'framesDecoded'),
    received_audio_bytes_delta: streamDelta(samples, 'receive', 'inbound-rtp', 'audio', 'bytesReceived'),
    last_capture: samples.at(-1).send.state.publish?.capture,
    last_receive_video: samples.at(-1).receive.state.subscribe?.metrics?.streams.find(stream => stream.kind === 'video'),
    selected_receive_pair: samples.at(-1).receive.connections[0]?.selected_pair};
}
async function recordPhase(name, count = seconds) {
  const phase = {name, started_at: new Date().toISOString(), samples: []};
  result.phases.push(phase);
  for (let i = 0; i < count; ++i) {
    const [send, receive] = await Promise.all(pages.map(snapshot));
    phase.samples.push({at: new Date().toISOString(), send, receive});
    if (i < count - 1) await sleep(1000);
  }
  phase.summary = summarize(phase);
  // Persist completed measurements before browser cleanup or subsequent phases.
  await writeFile(resolve(output, 'result.json'), JSON.stringify(result, null, 2));
  assert.ok(phase.summary.send_video_kbps > 0, `${name}: no video transmission`);
  assert.ok(phase.summary.receive_video_kbps > 0 && phase.summary.decoded_frames_delta > 0,
    `${name}: no video decoding progress`);
  assert.ok(phase.summary.received_audio_bytes_delta > 0, `${name}: no audio reception progress`);
  console.log(`${name}: tx=${fmt(phase.summary.send_video_kbps)} kbps rx=${fmt(phase.summary.receive_video_kbps)} kbps ` +
    `decode=${fmt(phase.summary.decode_fps)} fps RTT=${fmt(phase.summary.ice_rtt_mean_ms)} ms`);
}
function markdown() {
  return renderPublicTransportReport(result);
}

try {
  const ca = await readFile(process.env.PACKETIA_PUBLIC_TEST_CA || resolve(directory, '../../ca.crt'));
  const started = performance.now();
  result.health = await new Promise((resolveHealth, reject) => {
    const request = httpsGet(`${url}/healthz`, {ca, timeout: 10000}, response => {
      let text = ''; response.on('data', chunk => {text += chunk;});
      response.on('end', () => resolveHealth({status: response.statusCode, body: JSON.parse(text),
        elapsed_ms: performance.now() - started, certificate_verified: true}));
    });
    request.on('timeout', () => request.destroy(new Error('HTTPS health timeout')));
    request.on('error', reject);
  });
  assert.equal(result.health.status, 200);
  for (let i = 0; i < 2; ++i) {
    const server = await chromium.launchServer({channel: 'chrome', headless: true,
      args: ['--use-fake-device-for-media-stream=fps=60', '--use-fake-ui-for-media-stream',
        '--autoplay-policy=no-user-gesture-required', '--no-proxy-server']});
    servers.push(server);
    const browser = await chromium.connect(server.wsEndpoint());
    browsers.push(browser); result.browser_version = browser.version();
    const context = await browser.newContext({permissions: ['camera', 'microphone'],
      viewport: {width: 1440, height: 1100}});
    await context.addInitScript(() => {
      window.__packetiaTestConnections = [];
      window.RTCPeerConnection = new Proxy(window.RTCPeerConnection, {
        construct(target, args) {
          const pc = Reflect.construct(target, args);
          window.__packetiaTestConnections.push(pc); return pc;
        }
      });
    });
    const login = await new Promise((resolveLogin, reject) => {
      const body = new URLSearchParams({password}).toString();
      const request = httpsRequest(`${url}/login`, {method: 'POST', ca, timeout: 10000,
        headers: {Origin: new URL(url).origin, 'Content-Type': 'application/x-www-form-urlencoded',
          'Content-Length': Buffer.byteLength(body)}}, response => {
        response.resume();
        response.on('end', () => resolveLogin({status: response.statusCode, cookies: response.headers['set-cookie']}));
      });
      request.on('timeout', () => request.destroy(new Error('HTTPS login timeout')));
      request.on('error', reject); request.end(body);
    });
    assert.equal(login.status, 303, 'Test password login failed');
    const match = login.cookies?.[0]?.match(/^packetia_browser_test=([a-f0-9]+);/);
    assert.ok(match, 'Test login did not return its session cookie');
    await context.addCookies([{name: 'packetia_browser_test', value: match[1], url,
      secure: true, httpOnly: true, sameSite: 'Strict'}]);
    const page = await context.newPage(); pages.push(page);
    page.on('pageerror', error => result.browser_errors.push(error.message));
    await page.goto(url); await page.waitForFunction(() => Boolean(window.demo));
    await page.locator('#role').selectOption(i === 0 ? 'publisher' : 'viewer');
    await page.locator('#join').click();
    await until(async () => {
      const error = await page.locator('#error').textContent(); if (error) throw new Error(error);
      return (await page.locator('#connection').textContent()) === 'Joined';
    }, 'join public room');
  }
  const [publisher, viewer] = pages;
  result.room_id = (await snapshot(publisher)).state.roomId;
  await publisher.locator('#resolution').selectOption('640x360');
  await publisher.locator('#framerate').selectOption('30');
  await publisher.locator('#max-bitrate').fill('900');
  await publisher.locator('#publish').click();
  await until(async () => {
    const error = await publisher.locator('#error').textContent(); if (error) throw new Error(error);
    return (await publisher.locator('#local-status').textContent()) === 'Publishing';
  }, 'publish media');
  await until(async () => !await viewer.locator('#watch').isDisabled(), 'published track availability');
  await viewer.locator('#watch').click();
  await until(async () => {
    const state = await snapshot(viewer);
    return state.state.subscribe?.state === 'connected' && state.state.subscribe.frames >= 30;
  }, 'public video decoding');
  result.checks.initial_media = true;
  await sleep(3000);
  await recordPhase('360p30 / 编码上限 900 kbps');
  for (const kbps of [300, 1500]) {
    await publisher.locator('#max-bitrate').fill(String(kbps));
    await publisher.locator('#apply-bitrate').click();
    await until(async () => (await snapshot(publisher)).state.publish?.encoding.max_bitrate_bps === kbps * 1000,
      'live encoder parameter update');
    await sleep(3000);
    await recordPhase(`360p30 / 编码上限 ${kbps} kbps`);
  }
  await viewer.screenshot({path: resolve(output, 'receiver-360p30.png'), fullPage: true});
  const before = await snapshot(viewer), restartAt = performance.now();
  await viewer.evaluate(() => window.demo.restartIce('subscribe'));
  const recovered = await until(async () => {
    const current = await snapshot(viewer);
    return current.state.subscribe?.state === 'connected' &&
      current.state.subscribe.sessionId === before.state.subscribe.sessionId &&
      current.state.subscribe.frames > before.state.subscribe.frames + 15 && current;
  }, 'ICE restart decoding progress');
  result.checks.ice_restart = {observed_recovery_ms: performance.now() - restartAt,
    session_preserved: true, frames_before: before.state.subscribe.frames, frames_after: recovered.state.subscribe.frames};
  await recordPhase('ICE 重启后 / 360p30', Math.min(seconds, 10));
  await viewer.evaluate(() => window.demo.stopMedia('subscribe'));
  await publisher.evaluate(() => window.demo.stopMedia('publish'));
  await publisher.locator('#resolution').selectOption('1280x720');
  await publisher.locator('#framerate').selectOption('60');
  await publisher.locator('#max-bitrate').fill('1500');
  await publisher.locator('#publish').click();
  await until(async () => (await publisher.locator('#local-status').textContent()) === 'Publishing', '720p60 publication');
  await until(async () => !await viewer.locator('#watch').isDisabled(), '720p track availability');
  await viewer.locator('#watch').click();
  await until(async () => (await snapshot(viewer)).state.subscribe?.frames >= 30, '720p decoding');
  await sleep(3000);
  await recordPhase('请求 720p60 / 编码上限 1500 kbps');
  await viewer.screenshot({path: resolve(output, 'receiver-720p60.png'), fullPage: true});
  result.reports = await Promise.all(pages.map(page => page.evaluate(() => window.demo.qualityReport())));
  result.final_states = await Promise.all(pages.map(snapshot));
  for (const page of pages) {
    assert.equal(await page.locator('#error').textContent(), '');
    assert.equal(await page.locator('#quality-error').textContent(), '');
  }
  assert.deepEqual(result.browser_errors, []);
  result.passed = true;
} catch (error) {
  result.error = error.message;
  result.failure_states = await Promise.all(pages.map(page => snapshot(page).catch(() => null)));
  for (const [index, page] of pages.entries())
    await page.screenshot({path: resolve(output, `failure-${index}.png`), fullPage: true}).catch(() => {});
  console.error(`Public media test failed: ${error.message}`); process.exitCode = 1;
} finally {
  await Promise.race([Promise.allSettled(pages.map(page => page.evaluate(() => window.demo?.leave()))), sleep(12000)]);
  await Promise.race([Promise.allSettled(browsers.map(browser => browser.close())), sleep(5000)]);
  await Promise.race([Promise.allSettled(servers.map(server => server.close())), sleep(5000)]);
  for (const server of servers) {
    const child = server.process();
    if (child.exitCode === null && child.signalCode === null) {
      try {child.kill('SIGKILL');}
      catch (error) {if (error.code !== 'ESRCH') result.cleanup_warning = error.message;}
    }
  }
  result.finished_at = new Date().toISOString();
  for (const phase of result.phases) phase.summary ||= summarize(phase);
  result.analysis = analyzePublicTransport(result);
  await writeFile(resolve(output, 'result.json'), JSON.stringify(result, null, 2));
  await writeFile(resolve(output, 'REPORT.md'), markdown());
  const keys = ['name', 'sample_count', 'measured_seconds', 'send_video_kbps', 'receive_video_kbps',
    'decode_fps', 'receive_video_loss_percent', 'ice_rtt_mean_ms', 'ice_rtt_p95_ms', 'receiver_nack_delta', 'receiver_freeze_delta'];
  const rows = result.phases.map(phase => ({name: phase.name, ...phase.summary}));
  await writeFile(resolve(output, 'summary.csv'), [keys.join(','), ...rows.map(row => keys.map(key =>
    JSON.stringify(row[key] ?? '')).join(','))].join('\n') + '\n');
  console.log(`Report: ${resolve(output, 'REPORT.md')}`);
}
