import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {createHmac, randomBytes} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {resolve, dirname} from 'node:path';

const directory = dirname(fileURLToPath(import.meta.url));
const root = resolve(directory, '../..');
const port = name => {
  const number = Number(process.env[name]);
  if (!Number.isInteger(number) || number < 1 || number > 65535) throw new Error(`Invalid ${name}`);
  return number;
};
const httpPort = process.env.PACKETIA_DEMO_HTTP_PORT ? port('PACKETIA_DEMO_HTTP_PORT') : 18000;
const wsPort = process.env.PACKETIA_DEMO_WS_PORT ? port('PACKETIA_DEMO_WS_PORT') : 18080;
const mediaPort = process.env.PACKETIA_DEMO_MEDIA_PORT ? port('PACKETIA_DEMO_MEDIA_PORT') : 19000;
const turnPort = process.env.PACKETIA_DEMO_TURN_PORT ? port('PACKETIA_DEMO_TURN_PORT') : 13478;
if (process.env.PACKETIA_WEBRTC_TOKEN !== undefined)
  throw new Error('PACKETIA_WEBRTC_TOKEN was removed; unset it before running the JWT demo');
// Only this loopback development issuer and the child service see the secret.
const apiKey = `demo-${randomBytes(8).toString('hex')}`;
const apiSecret = randomBytes(32).toString('hex');
const username = 'room-demo';
const credential = randomBytes(24).toString('hex');
const config = {url: `ws://127.0.0.1:${wsPort}`,
  iceServer: {urls: `turn:127.0.0.1:${turnPort}?transport=udp`, username, credential}};
const binary = process.env.PACKETIA_ROOM_DEMO_BINARY || resolve(root, 'build/webrtc-integration/server/PacketiaRoomDemo');
const child = spawn(binary, [String(wsPort), String(mediaPort), String(turnPort)], {
  cwd: '/tmp', env: {...process.env, PACKETIA_WEBRTC_API_KEY: apiKey, PACKETIA_WEBRTC_API_SECRET: apiSecret,
    PACKETIA_TURN_USER: username, PACKETIA_TURN_PASSWORD: credential}, stdio: ['ignore', 'pipe', 'pipe']
});
let server;
let stopping = false;
const stop = () => {
  if (stopping) return;
  stopping = true;
  server?.close();
  child.kill('SIGTERM');
  const timer = setTimeout(() => child.kill('SIGKILL'), 5000);
  timer.unref();
};
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
child.stderr.on('data', () => {});
child.on('error', error => {console.error(error.message); process.exitCode = 1; stop();});
child.on('exit', code => {
  server?.close();
  if (!stopping) {console.error(`Room service exited (${code})`); process.exitCode = 1;}
});
await new Promise((resolveReady, reject) => {
  let output = '';
  const timeout = setTimeout(() => {stop(); reject(new Error('Room service startup timed out'));}, 10000);
  child.stdout.on('data', chunk => {
    output = (output + chunk.toString()).slice(-2048);
    if (output.includes('ROOM_DEMO_READY')) {clearTimeout(timeout); resolveReady();}
  });
  child.once('error', error => {clearTimeout(timeout); reject(error);});
  child.once('exit', () => {clearTimeout(timeout); reject(new Error('Room service startup failed; check ports and build'));});
});
const files = new Map([['/', ['index.html', 'text/html']], ['/app.js', ['app.js', 'text/javascript']],
  ['/style.css', ['style.css', 'text/css']]]);
server = createServer(async (request, response) => {
  try {
    response.setHeader('Cache-Control', 'no-store');
    const path = new URL(request.url, 'http://localhost').pathname;
    if (path === '/token') {
      // Demo-only admission endpoint. A real issuer must verify login and room access.
      if (request.method !== 'POST') {response.writeHead(405).end(); return;}
      if (request.headers.host !== `127.0.0.1:${httpPort}` ||
          request.headers.origin !== `http://127.0.0.1:${httpPort}` ||
          request.headers['content-type'] !== 'application/json') {response.writeHead(403).end(); return;}
      let body = '';
      for await (const chunk of request) {
        body += chunk.toString();
        if (body.length > 2048) {response.writeHead(413).end(); return;}
      }
      let input;
      try {input = JSON.parse(body);} catch {response.writeHead(400).end(); return;}
      if (!input || typeof input.room !== 'string' || typeof input.identity !== 'string' ||
          !/^[A-Za-z0-9_.-]{1,64}$/.test(input.room) ||
          !/^demo-[a-f0-9-]{36}$/.test(input.identity) || !['publisher', 'viewer'].includes(input.role)) {
        response.writeHead(400).end(); return;
      }
      const now = Math.floor(Date.now() / 1000);
      const claims = {iss: apiKey, sub: input.identity, aud: 'packetia-webrtc', iat: now, nbf: now, exp: now + 3600,
        video: {room: input.room, roomJoin: true, canPublish: input.role === 'publisher', canSubscribe: input.role === 'viewer'}};
      const encode = value => Buffer.from(JSON.stringify(value)).toString('base64url');
      const signing = `${encode({alg: 'HS256', typ: 'JWT'})}.${encode(claims)}`;
      const token = `${signing}.${createHmac('sha256', apiSecret).update(signing).digest('base64url')}`;
      response.setHeader('Content-Type', 'application/json'); response.end(JSON.stringify({token})); return;
    }
    if (request.method !== 'GET') {response.writeHead(405).end(); return;}
    if (path === '/config') {response.setHeader('Content-Type', 'application/json'); response.end(JSON.stringify(config)); return;}
    const file = files.get(path);
    if (!file) {response.writeHead(404).end(); return;}
    response.setHeader('Content-Type', `${file[1]}; charset=utf-8`);
    response.end(await readFile(resolve(directory, file[0])));
  } catch {response.writeHead(500).end();}
});
server.on('error', error => {console.error(error.message); process.exitCode = 1; stop();});
server.listen(httpPort, '127.0.0.1', () => console.log(`Room demo: http://127.0.0.1:${httpPort}`));
