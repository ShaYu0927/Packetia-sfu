import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {randomBytes} from 'node:crypto';
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
const token = randomBytes(24).toString('hex');
const username = 'room-demo';
const credential = randomBytes(24).toString('hex');
const config = {url: `ws://127.0.0.1:${wsPort}`, token,
  iceServer: {urls: `turn:127.0.0.1:${turnPort}?transport=udp`, username, credential}};
const binary = process.env.PACKETIA_ROOM_DEMO_BINARY || resolve(root, 'build/webrtc-integration/server/PacketiaRoomDemo');
const child = spawn(binary, [String(wsPort), String(mediaPort), String(turnPort)], {
  cwd: '/tmp', env: {...process.env, PACKETIA_WEBRTC_TOKEN: token,
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
    if (request.method !== 'GET') {response.writeHead(405).end(); return;}
    const path = new URL(request.url, 'http://localhost').pathname;
    if (path === '/config') {response.setHeader('Content-Type', 'application/json'); response.end(JSON.stringify(config)); return;}
    const file = files.get(path);
    if (!file) {response.writeHead(404).end(); return;}
    response.setHeader('Content-Type', `${file[1]}; charset=utf-8`);
    response.end(await readFile(resolve(directory, file[0])));
  } catch {response.writeHead(500).end();}
});
server.on('error', error => {console.error(error.message); process.exitCode = 1; stop();});
server.listen(httpPort, '127.0.0.1', () => console.log(`Room demo: http://127.0.0.1:${httpPort}`));
