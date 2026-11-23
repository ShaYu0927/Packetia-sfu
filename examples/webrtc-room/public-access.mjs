// HTTPS access gate and WebSocket proxy for the public IP test page.
import {createHash, randomBytes, timingSafeEqual} from 'node:crypto';
import {request as httpRequest} from 'node:http';
import {isIP} from 'node:net';

export function createPublicAccess({origin, password}) {
  const url = new URL(origin);
  if (url.protocol !== 'https:' || isIP(url.hostname) !== 4 || url.pathname !== '/' ||
      url.username || url.password || url.search || url.hash)
    throw new Error('Public browser test requires an HTTPS IPv4 origin');
  if (typeof password !== 'string' || password.length < 20)
    throw new Error('Public browser test password must contain at least 20 characters');
  const cookie = `packetia_browser_test=${randomBytes(32).toString('hex')}`;
  const passwordHash = createHash('sha256').update(password).digest();
  const authenticated = request => (request.headers.cookie || '').split(';').some(value => value.trim() === cookie);
  async function allow(request, response) {
    const path = new URL(request.url, url).pathname;
    if (path === '/login') {
      if (request.method === 'GET') {
        response.setHeader('Content-Type', 'text/html; charset=utf-8');
        response.end('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>Packetia 公网测试</title>' +
          '<h1>Packetia 公网传输测试</h1><form method="post" action="/login">' +
          '<label>测试访问密码 <input name="password" type="password" required autocomplete="current-password"></label>' +
          '<button>进入测试页面</button></form></html>');
        return false;
      }
      if (request.method !== 'POST') {response.writeHead(405).end(); return false;}
      if (request.headers.origin !== url.origin ||
          request.headers['content-type']?.split(';')[0] !== 'application/x-www-form-urlencoded') {
        response.writeHead(403).end(); return false;
      }
      const chunks = []; let size = 0;
      for await (const chunk of request) {
        size += chunk.length;
        if (size > 2048) {response.writeHead(413).end(); return false;}
        chunks.push(chunk);
      }
      const submitted = new URLSearchParams(Buffer.concat(chunks).toString()).get('password') || '';
      if (!timingSafeEqual(passwordHash, createHash('sha256').update(submitted).digest())) {
        response.writeHead(403).end('Invalid test password'); return false;
      }
      response.setHeader('Set-Cookie', `${cookie}; Path=/; Secure; HttpOnly; SameSite=Strict`);
      response.writeHead(303, {Location: '/'}).end(); return false;
    }
    if (authenticated(request)) return true;
    if (path === '/' && request.method === 'GET') response.writeHead(303, {Location: '/login'}).end();
    else response.writeHead(401).end('Test login required');
    return false;
  }
  return {origin: url.origin, signalingUrl: `${url.origin.replace('https:', 'wss:')}/webrtc`,
    authenticated, allow};
}

export function attachSignalingProxy(server, access, port) {
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Invalid local SFU signaling port');
  const sockets = new Set();
  const track = socket => {sockets.add(socket); socket.once('close', () => sockets.delete(socket));};
  server.on('upgrade', (request, socket, head) => {
    if (request.url !== '/webrtc' || request.headers.host !== new URL(access.origin).host ||
        request.headers.origin !== access.origin || !access.authenticated(request)) {
      socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n'); return;
    }
    track(socket);
    const headers = {...request.headers, host: `127.0.0.1:${port}`};
    delete headers.cookie; delete headers.authorization;
    const upstream = httpRequest({hostname: '127.0.0.1', port, path: '/webrtc', headers, timeout: 5000});
    const fail = () => socket.destroy();
    socket.on('error', () => upstream.destroy());
    socket.once('close', () => upstream.destroy());
    upstream.on('timeout', () => upstream.destroy()); upstream.on('error', fail);
    upstream.on('response', response => {response.resume(); fail();});
    upstream.on('upgrade', (response, target, upstreamHead) => {
      track(target); target.setTimeout(0);
      target.on('error', fail); target.once('close', fail);
      socket.once('close', () => target.destroy());
      let reply = `HTTP/1.1 ${response.statusCode} ${response.statusMessage}\r\n`;
      for (let i = 0; i < response.rawHeaders.length; i += 2)
        reply += `${response.rawHeaders[i]}: ${response.rawHeaders[i + 1]}\r\n`;
      socket.write(`${reply}\r\n`);
      if (upstreamHead.length) socket.write(upstreamHead);
      if (head.length) target.write(head);
      socket.pipe(target).pipe(socket);
    });
    upstream.end();
  });
  return {close: () => {for (const socket of sockets) socket.destroy();}};
}
