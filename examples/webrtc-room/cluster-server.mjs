// Local browser bridge for an existing Control/SFU deployment through SSH.
import {createServer} from 'node:http';
import {createServer as createSecureServer} from 'node:https';
import {randomBytes} from 'node:crypto';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import {createPublicAccess, attachSignalingProxy} from './public-access.mjs';

const files = new Map([['/', ['index.html', 'text/html']], ['/app.js', ['app.js', 'text/javascript']],
  ['/style.css', ['style.css', 'text/css']], ['/quality-stats.js', ['quality-stats.js', 'text/javascript']],
  ['/quality-panel.js', ['quality-panel.js', 'text/javascript']]]);

function loopbackUrl(value, protocols) {
  const url = new URL(value);
  if (!protocols.includes(url.protocol) || !['127.0.0.1', 'localhost'].includes(url.hostname) ||
      url.username || url.password || url.search || url.hash)
    throw new Error('This SSH test launcher requires loopback HTTP/WS URLs');
  return url;
}

export async function startClusterFrontend({controlUrl = 'http://127.0.0.1:18082', httpPort = 18100,
  publicAccess, signalingPort = 18180} = {}) {
  const control = loopbackUrl(controlUrl, ['http:']);
  if (control.pathname !== '/') throw new Error('Control URL must not include a path');
  if (!Number.isInteger(httpPort) || httpPort < 0 || httpPort > 65535)
    throw new Error('HTTP port must be an integer from 0 to 65535');
  const access = publicAccess && createPublicAccess(publicAccess);
  if (access && Number(new URL(access.origin).port || 443) !== httpPort)
    throw new Error('Public HTTPS origin port must match the listening port');
  const sessions = {};
  let room, config, origin, closing;
  async function api(path, body, session, method = 'POST') {
    const response = await fetch(new URL(`/api/v1/${path}`, control), {
      method, signal: AbortSignal.timeout(5000), redirect: 'error',
      headers: {'Content-Type': 'application/json', ...(session && {Authorization: `Bearer ${session}`})},
      ...(body !== undefined && {body: JSON.stringify(body)})
    });
    if (!response.ok) {
      const hint = path === 'accounts' && response.status === 403 ?
        '; enable registration_enabled in runtime/control.json and recreate Control first' : '';
      throw new Error(`Control ${path}: HTTP ${response.status}${hint}`);
    }
    return response.status === 204 ? undefined : response.json();
  }
  async function admission(role) {
    const value = await api('room-admissions', {room_id: room}, sessions[role]?.session_token);
    if (access) {
      if (value.signaling_url !== access.signalingUrl)
        throw new Error('SFU signaling_url must match the public browser test WSS URL');
    } else loopbackUrl(value.signaling_url, ['ws:']);
    if (value.room_id !== room || typeof value.token !== 'string' || !value.token ||
        (config && value.signaling_url !== config.url))
      throw new Error('Room admission changed route; restart the cluster test launcher');
    return value;
  }
  const handle = async (request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    try {
      if (request.headers.host !== new URL(origin).host) {response.writeHead(403).end(); return;}
      if (!config || closing) {response.writeHead(503).end(); return;}
      const path = new URL(request.url, origin).pathname;
      if (access && path === '/healthz' && request.method === 'GET') {
        response.setHeader('Content-Type', 'application/json');
        response.end('{"status":"ok"}'); return;
      }
      if (access && !await access.allow(request, response)) return;
      if (path === '/token') {
        if (request.method !== 'POST') {response.writeHead(405).end(); return;}
        if (request.headers.origin !== origin || request.headers['content-type'] !== 'application/json') {
          response.writeHead(403).end(); return;
        }
        let size = 0, body = '';
        for await (const chunk of request) {
          size += chunk.length;
          if (size > 2048) {response.writeHead(413).end(); return;}
          body += chunk.toString();
        }
        let input;
        try {input = JSON.parse(body);} catch {response.writeHead(400).end(); return;}
        if (input?.room !== room || !['publisher', 'viewer'].includes(input?.role)) {
          response.writeHead(403).end(); return;
        }
        // Never sign JWTs locally. Each join/resume receives a fresh Control admission.
        const value = await admission(input.role);
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify({token: value.token})); return;
      }
      if (request.method !== 'GET') {response.writeHead(405).end(); return;}
      if (path === '/config') {
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify(config)); return;
      }
      const file = files.get(path);
      if (!file) {response.writeHead(404).end(); return;}
      response.setHeader('Content-Type', `${file[1]}; charset=utf-8`);
      response.end(await readFile(new URL(file[0], import.meta.url)));
    } catch {
      response.writeHead(502).end('Control admission unavailable; check Control connectivity and SFU readiness');
    }
  };
  const server = access ? createSecureServer(publicAccess.tls, handle) : createServer(handle);
  const proxy = access && attachSignalingProxy(server, access, signalingPort);
  async function stop() {
    if (closing) return closing;
    closing = (async () => {
      const closed = new Promise(resolveClosed => server.close(resolveClosed));
      proxy?.close();
      server.closeAllConnections();
      await closed;
      const cleanupErrors = [];
      if (room) {
        try {await api('rooms/end', {room_id: room}, sessions.publisher.session_token);}
        catch (error) {cleanupErrors.push(error.message);}
      }
      const revoked = await Promise.allSettled(Object.values(sessions).map(session =>
        api('sessions/current', undefined, session.session_token, 'DELETE')));
      for (const result of revoked)
        if (result.status === 'rejected') cleanupErrors.push(result.reason.message);
      return cleanupErrors;
    })();
    return closing;
  }
  try {
    await new Promise((resolveReady, reject) => {
      server.once('error', reject); server.listen(httpPort, access ? '0.0.0.0' : '127.0.0.1', resolveReady);
    });
    origin = access?.origin || `http://127.0.0.1:${server.address().port}`;
    for (const role of ['publisher', 'viewer']) {
      const login = `browser-${role}-${randomBytes(8).toString('hex')}`;
      const password = randomBytes(24).toString('base64url');
      await api('accounts', {login, password, display_name: `Browser test ${role}`});
      sessions[role] = await api('sessions', {login, password});
    }
    room = (await api('rooms', {}, sessions.publisher.session_token)).room_id;
    await api('room-members', {room_id: room, user_id: sessions.viewer.identity.user_id, role: 'listener'},
      sessions.publisher.session_token);
    const first = await admission('publisher');
    config = {url: first.signaling_url, defaultRoom: room, testRoles: ['publisher', 'viewer']};
    return {origin, room, stop};
  } catch (error) {
    await stop();
    throw error;
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const bridge = await startClusterFrontend({
      controlUrl: process.env.PACKETIA_CONTROL_URL,
      httpPort: Number(process.env.PACKETIA_CLUSTER_HTTP_PORT ?? 18100)
    });
    console.log(`Cluster browser test: ${bridge.origin}\nRoom: ${bridge.room}`);
    console.log('Use Publisher in the first window and Viewer in the second. Keep the SSH tunnel running.');
    console.log('Ctrl+C ends this test room and revokes its sessions. Test accounts remain in the Control database.');
    const shutdown = async () => {
      for (const error of await bridge.stop()) console.error(`Cleanup failed: ${error}`);
    };
    process.once('SIGINT', shutdown); process.once('SIGTERM', shutdown);
  } catch (error) {
    console.error(`Cluster browser test failed: ${error.message}`);
    process.exitCode = 1;
  }
}
