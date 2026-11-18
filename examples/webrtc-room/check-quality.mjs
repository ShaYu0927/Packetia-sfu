import {chromium} from 'playwright';
import {readQosLog, selectRoom, summarizeQos} from './qos-log.mjs';
import assert from 'node:assert/strict';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const directory = dirname(fileURLToPath(import.meta.url)), artifacts = resolve(directory, 'artifacts');
await mkdir(artifacts, {recursive: true});
const url = process.env.PACKETIA_DEMO_URL || 'http://127.0.0.1:18000';
const expectTwcc = process.env.PACKETIA_EXPECT_TWCC === '1';
const expectServerLogs = expectTwcc || process.env.PACKETIA_EXPECT_QOS_LOG === '1';
const servers = [], pages = [], errors = [], phases = [];
const result = {generated_at: new Date().toISOString(), passed: false,
  scope: 'Loopback browser-to-SFU forwarding with fake camera/audio; encoder limits are not network shaping', phases};
const options = {headless: true, channel: 'chrome', args: ['--use-fake-device-for-media-stream=fps=60',
  '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required']};
if (process.env.PACKETIA_CHROME_EXECUTABLE) {
  delete options.channel; options.executablePath = process.env.PACKETIA_CHROME_EXECUTABLE;
}
const snapshot = page => page.evaluate(() => window.demo.stats());
async function until(operation, timeout = 40000) {
  const end = Date.now() + timeout;
  do {
    if (await operation()) return;
    await new Promise(resolveWait => setTimeout(resolveWait, 500));
  } while (Date.now() < end);
  throw new Error('Quality condition timed out');
}
async function windowSamples(name, publisher, viewer) {
  const samples = [];
  for (let i = 0; i < 6; ++i) {
    await new Promise(resolveWait => setTimeout(resolveWait, 1000));
    const [send, receive] = await Promise.all([snapshot(publisher), snapshot(viewer)]);
    samples.push({send, receive});
  }
  const videos = (state, role) => state[role]?.metrics?.streams.filter(stream => stream.kind === 'video') || [];
  assert.ok(samples.some(({send}) => videos(send, 'publish').some(stream => stream.bitrate_bps > 0 && stream.fps > 0)), 'No measured video transmission');
  assert.ok(samples.some(({receive}) => videos(receive, 'subscribe').some(stream => stream.bitrate_bps > 0 && stream.fps > 0)), 'No measured video decoding');
  assert.ok(samples.at(-1).receive.subscribe.frames > samples[0].receive.subscribe.frames, 'Decoder stopped progressing');
  phases.push({name, samples});
}
try {
  for (let i = 0; i < 2; ++i) {
    const server = await chromium.launchServer(options); servers.push(server);
    const browser = await chromium.connect(server.wsEndpoint());
    const context = await browser.newContext({permissions: ['camera', 'microphone'], viewport: {width: 1440, height: 1000}});
    const page = await context.newPage(); pages.push(page);
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(url); await page.waitForFunction(() => Boolean(window.demo));
  }
  const [publisher, viewer] = pages, room = `quality-${Date.now()}`;
  for (const [index, page] of pages.entries()) {
    await page.locator('#room').fill(room);
    await page.locator('#role').selectOption(index === 0 ? 'publisher' : 'viewer');
    await page.locator('#join').click();
    await page.waitForFunction(() => document.getElementById('connection').textContent === 'Joined');
  }
  await publisher.locator('#framerate').selectOption('30');
  await publisher.locator('#max-bitrate').fill('900');
  await publisher.locator('#publish').click();
  await publisher.waitForFunction(() => document.getElementById('local-status').textContent === 'Publishing');
  await viewer.waitForFunction(() => !document.getElementById('watch').disabled);
  await viewer.locator('#watch').click();
  await until(async () => (await snapshot(viewer)).subscribe?.frames >= 30);
  assert.equal((await snapshot(publisher)).publish.encoding.max_bitrate_bps, 900000);
  assert.equal((await snapshot(publisher)).publish.encoding.max_framerate, 30);
  await windowSamples('360p30-encoder-limit-900kbps', publisher, viewer);

  for (const kbps of [300, 1500]) {
    await publisher.locator('#max-bitrate').fill(String(kbps));
    await publisher.locator('#apply-bitrate').click();
    await until(async () => (await snapshot(publisher)).publish.encoding.max_bitrate_bps === kbps * 1000);
    await windowSamples(`live-encoder-limit-${kbps}kbps`, publisher, viewer);
  }
  await viewer.screenshot({path: resolve(artifacts, 'quality-desktop.png'), fullPage: true});
  await viewer.setViewportSize({width: 390, height: 844});
  await viewer.screenshot({path: resolve(artifacts, 'quality-mobile.png'), fullPage: true});
  assert.equal(await viewer.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, 'Mobile horizontal overflow');

  const beforeRestart = await snapshot(viewer);
  await viewer.evaluate(() => window.demo.restartIce('subscribe'));
  await until(async () => {
    const state = await snapshot(viewer);
    return state.subscribe.sessionId === beforeRestart.subscribe.sessionId && state.subscribe.state === 'connected' &&
      state.subscribe.frames > beforeRestart.subscribe.frames + 15;
  });
  phases.push({name: 'ice-restart-keeps-decoding', state: await snapshot(viewer)});

  await viewer.evaluate(() => window.demo.stopMedia('subscribe'));
  await publisher.evaluate(() => window.demo.stopMedia('publish'));
  await publisher.locator('#resolution').selectOption('1280x720');
  await publisher.locator('#framerate').selectOption('60');
  await publisher.locator('#max-bitrate').fill('1500');
  await publisher.locator('#publish').click();
  await publisher.waitForFunction(() => document.getElementById('local-status').textContent === 'Publishing');
  await viewer.waitForFunction(() => !document.getElementById('watch').disabled);
  await viewer.evaluate(() => window.demo.watch('*'));
  await until(async () => (await snapshot(viewer)).subscribe?.frames >= 30);
  const highRequest = (await snapshot(publisher)).publish;
  assert.equal(highRequest.requested.fps, 60); assert.equal(highRequest.encoding.max_framerate, 60);
  assert.ok(highRequest.capture.frameRate >= 50 && highRequest.capture.frameRate <= 60, 'Fake capture did not provide the requested high frame rate');
  await windowSamples('720p60-request-actual-values-recorded', publisher, viewer);
  assert.ok(phases.at(-1).samples.some(({receive}) => receive.subscribe.metrics.streams.some(
    stream => stream.kind === 'video' && stream.fps >= 45)), 'High-frame-rate video did not reach the decoder');

  const downloadPromise = viewer.waitForEvent('download');
  await viewer.locator('#export-report').click();
  const download = await downloadPromise;
  await download.saveAs(resolve(artifacts, 'quality-receiver-export.json'));
  const exported = JSON.parse(await readFile(resolve(artifacts, 'quality-receiver-export.json'), 'utf8'));
  assert.equal(exported.source, 'browser-getStats'); assert.ok(exported.samples.length >= 10);
  assert.equal(/resume_token|apiSecret|credential/.test(JSON.stringify(exported)), false);
  result.reports = await Promise.all(pages.map(page => page.evaluate(() => window.demo.qualityReport())));
  const traceFile = process.env.PACKETIA_QOS_TRACE_FILE || resolve(artifacts, 'server-qos.jsonl');
  let serverRecords;
  try {serverRecords = selectRoom(await readQosLog(traceFile), room);}
  catch (error) {if (expectServerLogs) throw error;}
  result.server_diagnostics = serverRecords?.length ? summarizeQos(serverRecords) : {collected: false};
  if (serverRecords?.length) {
    await writeFile(resolve(artifacts, 'quality-server-qos.jsonl'), serverRecords.map(record => JSON.stringify(record)).join('\n') + '\n');
    await writeFile(resolve(artifacts, 'quality-server-summary.json'), JSON.stringify(result.server_diagnostics, null, 2));
  }
  if (expectServerLogs) {
    assert.ok(serverRecords?.some(record => record.event === 'qos_sample' && record.data.sent_rtp_bps > 0), 'No actual server send-rate samples');
    assert.ok(serverRecords.some(record => record.event === 'qos_sample' &&
      record.data.rr_reports_total + record.data.twcc_reports_total > 0), 'No actual RTCP feedback observed');
    assert.ok(serverRecords.some(record => record.event === 'qos_allocation'), 'No allocation diagnostics observed');
  }
  if (expectTwcc) {
    const subscribers = serverRecords.filter(record => record.event === 'qos_session' && record.data.role === 'subscribe');
    assert.ok(subscribers.length >= 2, 'Expected both original and replacement subscriber sessions');
    for (const session of subscribers) {
      const records = serverRecords.filter(record => record.pid === session.pid && record.node_id === session.node_id &&
        record.data.transport_id === session.data.transport_id);
      assert.ok(records.some(record => record.event === 'qos_subscription' && record.data.twcc_extension_id > 0),
        'No negotiated downstream TWCC extension');
      assert.ok(records.some(record => record.event === 'qos_sample' && record.data.feedback_source === 'twcc' &&
        record.data.twcc_reports_total > 0 && record.data.matched_feedback_packets_total > 0 &&
        record.data.bwe_estimate_valid && Number.isFinite(record.data.delay_slope)),
        'TWCC did not reach sending history, trend detection and bandwidth estimation');
      assert.ok(records.some(record => record.event === 'qos_allocation' && record.data.budget_estimate_valid),
        'No valid TWCC allocation budget');
    }
  }
  for (const page of pages) {
    assert.equal(await page.locator('#quality-error').textContent(), '');
    assert.equal(await page.locator('#error').textContent(), '');
    await page.locator('#leave').click();
    await page.waitForFunction(() => document.getElementById('connection').textContent === 'Disconnected');
    assert.equal((await snapshot(page)).mode, '');
    assert.equal(await page.locator('#apply-bitrate').isDisabled(), true);
  }
  assert.deepEqual(errors, []); result.passed = true;
  console.log('PASS quality dashboard: real forwarding, measured per-track rates, live encoder limits, ICE restart, capture requests and JSON export');
} catch (error) {
  result.error = error.stack; result.browser_errors = errors;
  result.states = await Promise.all(pages.map(page => snapshot(page).catch(() => null)));
  for (const [index, page] of pages.entries())
    await page.screenshot({path: resolve(artifacts, `quality-failure-${index}.png`), fullPage: true}).catch(() => {});
  console.error(error.stack); process.exitCode = 1;
} finally {
  await writeFile(resolve(artifacts, 'quality-result.json'), JSON.stringify(result, null, 2));
  await Promise.race([Promise.allSettled(servers.map(server => server.close())),
    new Promise(resolveWait => setTimeout(resolveWait, 5000))]);
  for (const server of servers) {
    const process = server.process();
    if (process.exitCode === null && process.signalCode === null) {try {process.kill('SIGKILL');} catch {}}
  }
  process.exit(process.exitCode || 0);
}
