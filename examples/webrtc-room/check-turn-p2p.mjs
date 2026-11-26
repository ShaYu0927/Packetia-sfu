// Two independent Chrome processes; SDP is exchanged only by this test runner.
// Both ends require relay candidates, so a successful direct path cannot hide a TURN failure.
import {chromium} from 'playwright';
import {spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {createServer} from 'node:http';
import {mkdir, writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {once} from 'node:events';

const directory = dirname(fileURLToPath(import.meta.url));
const reportPath = resolve(directory, 'artifacts', `turn-p2p-${new Date().toISOString().replace(/[:.]/g, '-')}.json`);
const servers = [], pages = [];
let turn, turnExited, turnFailure;
const report = {passed: false, mode: process.env.PACKETIA_TURN_URL ? 'external-turn' : 'local-turn', phases: []};
const http = createServer((_request, response) => {
  response.writeHead(200, {'Content-Type': 'text/html'});
  response.end('<!doctype html><title>TURN P2P verification</title>');
});
function require(condition, message) { if (!condition) throw new Error(message); }
async function until(operation, label, timeout = 30000) {
  const deadline = Date.now() + timeout;
  do {
    if (turnFailure) throw new Error(turnFailure);
    if (await operation()) return;
    await new Promise(resolve => setTimeout(resolve, 200));
  } while (Date.now() < deadline);
  throw new Error(`Timed out: ${label}`);
}
async function startTurn() {
  if (process.env.PACKETIA_TURN_URL) {
    require(process.env.PACKETIA_TURN_USER && process.env.PACKETIA_TURN_PASSWORD,
      'External TURN requires PACKETIA_TURN_USER and PACKETIA_TURN_PASSWORD');
    return {urls: process.env.PACKETIA_TURN_URL, username: process.env.PACKETIA_TURN_USER,
      credential: process.env.PACKETIA_TURN_PASSWORD};
  }
  const username = `p2p-${randomBytes(8).toString('hex')}`, credential = randomBytes(24).toString('hex');
  const binary = process.env.PACKETIA_TURN_BINARY || resolve(directory, '../../build/protocol/PacketiaTurn');
  turn = spawn(binary, ['--listen-port', '0', '--relay-min-port', '0', '--relay-max-port', '0', '--local-test'],
    {env: {...process.env, PACKETIA_TURN_USER: username, PACKETIA_TURN_PASSWORD: credential,
      PACKETIA_LOG_MODE: 'console'}, stdio: ['ignore', 'pipe', 'pipe']});
  turnExited = new Promise(resolve => {
    turn.once('error', () => {turnFailure = 'Cannot start PacketiaTurn; build it or set PACKETIA_TURN_BINARY'; resolve();});
    turn.once('exit', code => {turnFailure = `PacketiaTurn exited (${code})`; resolve();});
  });
  let output = '', port;
  turn.stdout.on('data', data => {
    output = (output + data).slice(-8192);
    const match = output.match(/TURN UDP listening on 127\.0\.0\.1:(\d+)/);
    if (match) port = Number(match[1]);
  });
  // Consume stderr but never copy credentials or arbitrary server output into artifacts.
  turn.stderr.resume();
  await until(() => port > 0, 'TURN listener startup', 10000);
  return {urls: `turn:127.0.0.1:${port}?transport=udp`, username, credential};
}
async function negotiate(restart = false) {
  const offer = await pages[0].evaluate(async restart => {
    await pc.setLocalDescription(await pc.createOffer({iceRestart: restart}));
    await gathered();
    return {type: pc.localDescription.type, sdp: pc.localDescription.sdp};
  }, restart);
  const answer = await pages[1].evaluate(async offer => {
    await pc.setRemoteDescription(offer);
    await pc.setLocalDescription(await pc.createAnswer());
    await gathered();
    return {type: pc.localDescription.type, sdp: pc.localDescription.sdp};
  }, offer);
  await pages[0].evaluate(answer => pc.setRemoteDescription(answer), answer);
}
async function snapshot(page) {
  return page.evaluate(async () => {
    const stats = await pc.getStats();
    const transport = [...stats.values()].find(item => item.type === 'transport' && item.selectedCandidatePairId);
    const pair = stats.get(transport?.selectedCandidatePairId);
    const local = stats.get(pair?.localCandidateId), remote = stats.get(pair?.remoteCandidateId);
    const media = [...stats.values()].filter(item => item.type === 'inbound-rtp' && !item.isRemote);
    const selected = pc.getSenders().find(sender => sender.transport)?.transport.iceTransport.getSelectedCandidatePair();
    return {state: pc.connectionState, iceState: pc.iceConnectionState,
      iceTransitions: [...iceTransitions],
      selectedLocalType: selected?.local.type,
      pair: pair && {state: pair.state, localType: local?.candidateType, remoteType: remote?.candidateType,
        localAddress: local?.address, localPort: local?.port, remoteAddress: remote?.address, remotePort: remote?.port,
        protocol: local?.protocol, relayProtocol: local?.relayProtocol, rttSeconds: pair.currentRoundTripTime},
      audioBytes: media.filter(item => item.kind === 'audio').reduce((n, item) => n + (item.bytesReceived || 0), 0),
      audioSamples: media.filter(item => item.kind === 'audio').reduce((n, item) => n + (item.totalSamplesReceived || 0), 0),
      videoBytes: media.filter(item => item.kind === 'video').reduce((n, item) => n + (item.bytesReceived || 0), 0),
      framesDecoded: media.reduce((n, item) => n + (item.framesDecoded || 0), 0),
      messages: receivedMessages, invalidMessages, dataState: channel?.readyState,
      iceErrors: [...new Set(iceErrors)]};
  });
}
function relayed(states) {
  return states.every((state, index) => {
    const other = states[1 - index];
    // Chrome can label the remote as prflx, hide its IP and leave the stats pair
    // state in-progress after restart. Use the ICE transport's actual selection;
    // both relay-only clients must select a local relay and keep decoding media.
    return state.state === 'connected' && ['connected', 'completed'].includes(state.iceState) &&
      state.selectedLocalType === 'relay' && state.pair?.localType === 'relay' &&
      other.selectedLocalType === 'relay' && state.pair.remotePort === other.pair?.localPort &&
      (!state.pair.remoteAddress || state.pair.remoteAddress === other.pair.localAddress);
  });
}
async function verify(phase) {
  console.log(`Checking ${phase}...`);
  let states;
  await until(async () => {
    states = await Promise.all(pages.map(snapshot));
    return relayed(states) && states.every(state => state.audioBytes > 0 && state.audioSamples > 0 &&
      state.framesDecoded >= 30 && state.dataState === 'open');
  }, `${phase}: relay pair and decoded audio/video`, 45000);
  const before = states;
  for (const page of pages) await page.evaluate(() => {
    const bytes = Uint8Array.from({length: 4096}, (_, index) => (index + marker) & 255);
    channel.send(bytes);
  });
  await until(async () => {
    states = await Promise.all(pages.map(snapshot));
    return relayed(states) && states.every((state, index) => state.messages > before[index].messages && state.invalidMessages === 0 &&
      state.audioBytes > before[index].audioBytes && state.videoBytes > before[index].videoBytes &&
      state.audioSamples > before[index].audioSamples &&
      state.framesDecoded >= before[index].framesDecoded + 30);
  }, `${phase}: bidirectional binary data and continuing media`);
  report.phases.push({phase, states});
}
try {
  const iceServer = await startTurn();
  console.log(`TURN ready (${report.mode}); starting browsers...`);
  http.listen(0, '127.0.0.1'); await once(http, 'listening');
  const options = {headless: true, channel: 'chrome', args: ['--use-fake-device-for-media-stream',
    '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required']};
  if (process.env.PACKETIA_CHROME_EXECUTABLE) {
    delete options.channel; options.executablePath = process.env.PACKETIA_CHROME_EXECUTABLE;
  }
  for (let index = 0; index < 2; ++index) {
    const server = await chromium.launchServer(options); servers.push(server);
    const browser = await chromium.connect(server.wsEndpoint());
    const context = await browser.newContext({permissions: ['camera', 'microphone']});
    const page = await context.newPage(); pages.push(page);
    await page.goto(`http://127.0.0.1:${http.address().port}`);
    await page.evaluate(async ({iceServer, index}) => {
      window.marker = index + 1;
      window.receivedMessages = 0; window.invalidMessages = 0; window.iceErrors = []; window.iceTransitions = [];
      window.channel = null;
      window.pc = new RTCPeerConnection({iceServers: [iceServer], iceTransportPolicy: 'relay'});
      pc.oniceconnectionstatechange = () => iceTransitions.push(pc.iceConnectionState);
      pc.onicecandidateerror = event => iceErrors.push(event.errorCode);
      window.gathered = () => new Promise((resolve, reject) => {
        if (pc.iceGatheringState === 'complete') return resolve();
        const timer = setTimeout(() => {pc.removeEventListener('icegatheringstatechange', changed);
          reject(new Error('ICE gathering timed out'));}, 15000);
        function changed() {
          if (pc.iceGatheringState !== 'complete') return;
          clearTimeout(timer); pc.removeEventListener('icegatheringstatechange', changed); resolve();
        }
        pc.addEventListener('icegatheringstatechange', changed);
      });
      function attachChannel(value) {
        window.channel = value; channel.binaryType = 'arraybuffer';
        channel.onmessage = event => {
          const bytes = new Uint8Array(event.data), otherMarker = index === 0 ? 2 : 1;
          if (bytes.length !== 4096 || bytes.some((byte, offset) => byte !== ((offset + otherMarker) & 255)))
            ++invalidMessages;
          ++receivedMessages;
        };
      }
      pc.ondatachannel = event => attachChannel(event.channel);
      if (index === 0) attachChannel(pc.createDataChannel('binary-check'));
      pc.ontrack = event => {
        const element = document.createElement(event.track.kind === 'video' ? 'video' : 'audio');
        element.autoplay = true; element.muted = true;
        element.srcObject = new MediaStream([event.track]); document.body.append(element);
      };
      window.stream = await navigator.mediaDevices.getUserMedia({audio: true,
        video: {width: 320, height: 240, frameRate: 15}});
      for (const track of stream.getTracks()) pc.addTrack(track, stream);
    }, {iceServer, index});
  }
  await negotiate();
  console.log('SDP exchanged.');
  await verify('initial-relay');
  const oldCredentials = await Promise.all(pages.map(page => page.evaluate(() =>
    pc.localDescription.sdp.match(/a=ice-ufrag:([^\r\n]+)/)?.[1])));
  for (const page of pages) await page.evaluate(() => {iceTransitions.length = 0;});
  await negotiate(true);
  console.log('ICE restart SDP exchanged.');
  const newCredentials = await Promise.all(pages.map(page => page.evaluate(() =>
    pc.localDescription.sdp.match(/a=ice-ufrag:([^\r\n]+)/)?.[1])));
  require(newCredentials.every((value, index) => value && value !== oldCredentials[index]),
    'ICE restart did not replace both ICE credentials');
  report.restartCredentialsChanged = true;
  await verify('ice-restart');
  for (const page of pages) {
    const closed = await page.evaluate(() => {
      pc.close(); stream.getTracks().forEach(track => track.stop());
      return pc.connectionState === 'closed' && stream.getTracks().every(track => track.readyState === 'ended');
    });
    require(closed, 'Peer connection or capture tracks were not closed');
  }
  report.passed = true;
} catch (error) {
  // Store only test diagnostics, never SDP, ICE credentials, or the TURN password.
  report.error = error.message;
  console.error(report.error);
  report.lastStates = await Promise.all(pages.map(page => snapshot(page).catch(() => ({unavailable: true}))));
  console.error(JSON.stringify(report.lastStates));
  process.exitCode = 1;
} finally {
  await Promise.race([Promise.allSettled(servers.map(server => server.close())),
    new Promise(resolve => setTimeout(resolve, 5000))]);
  for (const server of servers) {
    const child = server.process();
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
  }
  http.close();
  if (turn && turn.exitCode === null && turn.signalCode === null) {
    turn.kill('SIGTERM');
    const timer = setTimeout(() => turn.kill('SIGKILL'), 3000);
    await turnExited; clearTimeout(timer);
  }
  await mkdir(dirname(reportPath), {recursive: true});
  await writeFile(reportPath, JSON.stringify(report, null, 2) + '\n');
  console.log(`${report.passed ? 'PASS' : 'FAIL'} TURN P2P: ${reportPath}`);
  if (report.error) console.error(report.error);
  process.exit(process.exitCode || 0);
}
