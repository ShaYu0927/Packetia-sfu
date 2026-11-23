import {after, before, test} from 'node:test';
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {request as httpsRequest} from 'node:https';
import {createHash} from 'node:crypto';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {mkdtemp, readFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {startClusterFrontend} from './cluster-server.mjs';

let directory, tls;
const password = 'public-fixture-password-long-enough';
before(async () => {
  directory = await mkdtemp(join(tmpdir(), 'packetia-public-test-'));
  await promisify(execFile)('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
    '-keyout', join(directory, 'key.pem'), '-out', join(directory, 'cert.pem'),
    '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1']);
  tls = {key: await readFile(join(directory, 'key.pem')), cert: await readFile(join(directory, 'cert.pem'))};
});
after(async () => {if (directory) await rm(directory, {recursive: true, force: true});});

const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
async function fixture(t) {
  let origin;
  const calls = [], peers = new Set();
  const sfu = createServer();
  sfu.on('upgrade', (request, socket, head) => {
    peers.add(socket); socket.on('close', () => peers.delete(socket)); socket.on('error', () => {});
    assert.equal(request.url, '/webrtc');
    assert.equal(request.headers.cookie, undefined);
    assert.equal(request.headers['sec-websocket-protocol'], 'packetia');
    const accept = createHash('sha1').update(request.headers['sec-websocket-key'] +
      '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: packetia\r\n\r\n`);
    if (head.length) socket.write(head);
    socket.on('data', data => socket.write(data));
  });
  await listen(sfu);
  const control = createServer(async (request, response) => {
    let text = ''; for await (const chunk of request) text += chunk.toString();
    calls.push({path: request.url, auth: request.headers.authorization});
    let value = {};
    if (request.url.endsWith('/sessions')) value = {session_token: 'fixture-session', identity: {user_id: 'fixture-user'}};
    if (request.url.endsWith('/rooms')) value = {room_id: 'public-test-room'};
    if (request.url.endsWith('/room-admissions')) value = {room_id: 'public-test-room',
      signaling_url: origin.replace('https:', 'wss:') + '/webrtc', token: 'fixture-admission'};
    response.writeHead(request.method === 'DELETE' ? 204 : 200, {'Content-Type': 'application/json'});
    response.end(request.method === 'DELETE' ? undefined : JSON.stringify(value));
  });
  await listen(control);
  // Reserve a free HTTPS port before constructing the public origin.
  const reservation = createServer(); await listen(reservation);
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  origin = `https://127.0.0.1:${port}`;
  const bridge = await startClusterFrontend({httpPort: port,
    controlUrl: `http://127.0.0.1:${control.address().port}`, signalingPort: sfu.address().port,
    publicAccess: {origin, password, tls}});
  t.after(async () => {
    await bridge.stop();
    for (const peer of peers) peer.destroy();
    await Promise.all([new Promise(resolve => control.close(resolve)), new Promise(resolve => sfu.close(resolve))]);
  });
  function request(path, {method = 'GET', body, headers = {}} = {}) {
    return new Promise((resolve, reject) => {
      const req = httpsRequest(`${origin}${path}`, {method, ca: tls.cert, headers}, response => {
        let text = ''; response.on('data', chunk => {text += chunk;});
        response.on('end', () => resolve({status: response.statusCode, headers: response.headers, text}));
      });
      req.on('error', reject); req.end(body);
    });
  }
  async function login(value = password, from = origin) {
    return request('/login', {method: 'POST', headers: {Origin: from,
      'Content-Type': 'application/x-www-form-urlencoded'}, body: new URLSearchParams({password: value}).toString()});
  }
  return {request, login, bridge, calls, origin};
}

test('HTTPS public page requires login and protects admissions with cookies and Origin', async t => {
  const f = await fixture(t);
  assert.equal((await f.request('/healthz')).status, 200);
  assert.equal((await f.request('/')).headers.location, '/login');
  assert.equal((await f.request('/config')).status, 401);
  assert.equal((await f.request('/token', {method: 'POST'})).status, 401);
  assert.equal((await f.login('wrong-password')).status, 403);
  assert.equal((await f.login(password, 'https://other.example')).status, 403);
  const login = await f.login();
  assert.equal(login.status, 303);
  const cookie = login.headers['set-cookie'][0];
  assert.match(cookie, /Secure; HttpOnly; SameSite=Strict/);
  const headers = {Cookie: cookie.split(';')[0]};
  const config = await f.request('/config', {headers});
  assert.equal(JSON.parse(config.text).url, f.origin.replace('https:', 'wss:') + '/webrtc');
  assert.doesNotMatch(config.text, /password|session|fixture-admission/);
  assert.match((await f.request('/', {headers})).text, /id="join-form"/);
  const tokenRequest = {method: 'POST', body: JSON.stringify({room: 'public-test-room', role: 'viewer'}),
    headers: {...headers, 'Content-Type': 'application/json', Origin: 'https://other.example'}};
  const before = f.calls.length;
  assert.equal((await f.request('/token', tokenRequest)).status, 403);
  assert.equal(f.calls.length, before);
  tokenRequest.headers.Origin = f.origin;
  assert.deepEqual(JSON.parse((await f.request('/token', tokenRequest)).text), {token: 'fixture-admission'});
});

test('authenticated WSS upgrade preserves subprotocol and proxies bytes in both directions', async t => {
  const f = await fixture(t);
  const cookie = (await f.login()).headers['set-cookie'][0].split(';')[0];
  function upgrade(headers = {}) {
    return new Promise((resolve, reject) => {
      const request = httpsRequest(`${f.origin}/webrtc`, {ca: tls.cert, headers: {
        Connection: 'Upgrade', Upgrade: 'websocket', Origin: f.origin,
        'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==', 'Sec-WebSocket-Version': '13',
        'Sec-WebSocket-Protocol': 'packetia', ...headers}});
      request.on('upgrade', (response, socket) => resolve({response, socket}));
      request.on('response', response => {response.resume(); resolve({response});});
      request.on('error', reject); request.end();
    });
  }
  assert.equal((await upgrade()).response.statusCode, 403);
  assert.equal((await upgrade({Cookie: cookie, Origin: 'https://other.example'})).response.statusCode, 403);
  const {response, socket} = await upgrade({Cookie: cookie});
  assert.equal(response.statusCode, 101);
  assert.equal(response.headers['sec-websocket-protocol'], 'packetia');
  // A real masked text frame; the fixture echoes bytes to verify both proxy directions.
  const frame = Buffer.from([0x81, 0x82, 1, 2, 3, 4, 'h'.charCodeAt(0) ^ 1, 'i'.charCodeAt(0) ^ 2]);
  const echoed = new Promise((resolve, reject) => {
    socket.once('data', resolve); socket.once('error', reject);
  });
  socket.write(frame);
  assert.deepEqual(await echoed, frame);
  const closed = new Promise(resolve => socket.once('close', resolve));
  await f.bridge.stop();
  await closed;
});
