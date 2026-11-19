// Browser worker for scripts/test_meeting_control.py --media-node.
// Admissions arrive over stdin; only counters and session identifiers leave stdout.
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright';

const directory = dirname(fileURLToPath(import.meta.url));
const input = createInterface({input: process.stdin, crlfDelay: Infinity});
const servers = [], pages = [], errors = [];
let http, baseline;
const options = {headless: true, channel: 'chrome', args: ['--use-fake-device-for-media-stream',
  '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required']};
if (process.env.PACKETIA_CHROME_EXECUTABLE) {
  delete options.channel; options.executablePath = process.env.PACKETIA_CHROME_EXECUTABLE;
}
const snapshot = async () => Promise.all(pages.map(page => page.evaluate(async () => {
  const state = await window.demo.stats();
  return {participant: state.participant, recoveryCount: state.recoveryCount,
    publish: state.publish && {sessionId: state.publish.sessionId, state: state.publish.state,
      sentBytes: state.publish.sentBytes},
    subscribe: state.subscribe && {sessionId: state.subscribe.sessionId, state: state.subscribe.state,
      frames: state.subscribe.frames, audioBytes: state.subscribe.audioBytes}};
})));

async function until(operation, timeout = 30000) {
  const deadline = Date.now() + timeout;
  do {
    const value = await operation();
    if (value) return value;
    await new Promise(resolveWait => setTimeout(resolveWait, 250));
  } while (Date.now() < deadline);
  throw new Error('Cluster media condition timed out');
}

async function start({publisher, viewer}) {
  assert.equal(publisher.room_id, viewer.room_id);
  assert.equal(publisher.signaling_url, viewer.signaling_url);
  const files = new Map([['/', ['index.html', 'text/html']], ['/app.js', ['app.js', 'text/javascript']],
    ['/style.css', ['style.css', 'text/css']], ['/quality-stats.js', ['quality-stats.js', 'text/javascript']],
    ['/quality-panel.js', ['quality-panel.js', 'text/javascript']]]);
  http = createServer(async (request, response) => {
    try {
      response.setHeader('Cache-Control', 'no-store');
      const path = new URL(request.url, 'http://localhost').pathname;
      const origin = `http://127.0.0.1:${http.address().port}`;
      if (path === '/token' && request.method === 'POST') {
        if (request.headers.host !== new URL(origin).host || request.headers.origin !== origin ||
            request.headers['content-type'] !== 'application/json') {response.writeHead(403).end(); return;}
        let body = '';
        for await (const chunk of request) {
          body += chunk.toString();
          if (body.length > 2048) {response.writeHead(413).end(); return;}
        }
        const value = JSON.parse(body);
        const admission = value.role === 'publisher' ? publisher : value.role === 'viewer' ? viewer : null;
        if (!admission || value.room !== admission.room_id) {response.writeHead(403).end(); return;}
        // Serve a real control-plane admission; this worker never signs JWTs.
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify({token: admission.token})); return;
      }
      if (request.method !== 'GET') {response.writeHead(405).end(); return;}
      if (path === '/config') {
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify({url: publisher.signaling_url})); return;
      }
      const file = files.get(path);
      if (!file) {response.writeHead(404).end(); return;}
      response.setHeader('Content-Type', file[1]);
      response.end(await readFile(resolve(directory, file[0])));
    } catch {response.writeHead(400).end();}
  });
  await new Promise((resolveReady, reject) => {
    http.once('error', reject); http.listen(0, '127.0.0.1', resolveReady);
  });
  for (const role of ['publisher', 'viewer']) {
    const server = await chromium.launchServer(options); servers.push(server);
    const browser = await chromium.connect(server.wsEndpoint());
    const context = await browser.newContext({permissions: ['camera', 'microphone']});
    const page = await context.newPage(); pages.push(page);
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${http.address().port}`);
    await page.waitForFunction(() => Boolean(window.demo));
    await page.locator('#room').fill(publisher.room_id);
    await page.locator('#role').selectOption(role);
    await page.locator('#join').click();
    await page.waitForFunction(() => document.getElementById('connection').textContent === 'Joined');
  }
  await pages[0].locator('#publish').click();
  await pages[0].waitForFunction(() => document.getElementById('local-status').textContent === 'Publishing');
  await pages[1].waitForFunction(() => !document.getElementById('watch').disabled);
  await pages[1].locator('#watch').click();
  return until(async () => {
    const states = await snapshot();
    return states[0].publish?.state === 'connected' && states[0].publish.sentBytes > 0 &&
      states[1].subscribe?.state === 'connected' && states[1].subscribe.frames >= 30 &&
      states[1].subscribe.audioBytes > 0 && states;
  });
}

async function progress() {
  const before = await snapshot();
  assert.equal(before.length, 2);
  const after = await until(async () => {
    const states = await snapshot();
    return states[0].publish?.sentBytes > before[0].publish?.sentBytes &&
      states[1].subscribe?.frames >= before[1].subscribe?.frames + 15 &&
      states[1].subscribe?.audioBytes > before[1].subscribe?.audioBytes && states;
  }, 10000);
  for (const [index, state] of after.entries()) {
    assert.equal(state.participant, baseline[index].participant, 'Participant was recreated');
    assert.equal(state.recoveryCount, 0, 'Unexpected signaling recovery');
    const role = index === 0 ? 'publish' : 'subscribe';
    assert.equal(state[role].sessionId, baseline[index][role].sessionId, 'Media session was recreated');
    assert.equal(state[role].state, 'connected');
  }
  assert.deepEqual(errors, []);
  for (const page of pages) assert.equal(await page.locator('#error').textContent(), '');
  return {before, after};
}

try {
  for await (const line of input) {
    const command = JSON.parse(line);
    if (command.type === 'stop') break;
    const result = command.type === 'start' ? await start(command) :
      command.type === 'progress' ? await progress() : assert.fail('Unknown browser command');
    if (command.type === 'start') baseline = result;
    process.stdout.write(JSON.stringify({result}) + '\n');
  }
} catch (error) {
  process.stdout.write(JSON.stringify({error: error.message}) + '\n');
  process.exitCode = 1;
} finally {
  input.close();
  http?.close(); http?.closeAllConnections();
  await Promise.race([Promise.allSettled(servers.map(server => server.close())),
    new Promise(resolveWait => setTimeout(resolveWait, 5000))]);
  for (const server of servers) {
    const child = server.process();
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
  }
  process.exit(process.exitCode || 0);
}
