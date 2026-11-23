import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {startClusterFrontend} from './cluster-server.mjs';

async function controlFixture(t, {registration = true} = {}) {
  const calls = [];
  let users = 0, admissions = 0, signaling = 'ws://127.0.0.1:18180/webrtc';
  const server = createServer(async (request, response) => {
    let text = '';
    for await (const chunk of request) text += chunk.toString();
    const body = text ? JSON.parse(text) : undefined;
    const call = {path: request.url, body, method: request.method, auth: request.headers.authorization};
    calls.push(call);
    let status = 200, reply;
    switch (request.url) {
      case '/api/v1/accounts':
        status = registration ? 201 : 403;
        reply = {user: {user_id: `user-${++users}`}};
        assert.ok(body.password.length >= 12);
        break;
      case '/api/v1/sessions': {
        const role = body.login.includes('publisher') ? 'publisher' : 'viewer';
        const registrationCall = calls.find(item => item.path === '/api/v1/accounts' && item.body.login === body.login);
        assert.equal(body.password, registrationCall.body.password);
        reply = {session_token: `${role}-session`, identity: {user_id: role}};
        break;
      }
      case '/api/v1/rooms': status = 201; reply = {room_id: 'test-room'}; break;
      case '/api/v1/room-members': reply = {status: 'ok'}; break;
      case '/api/v1/room-admissions':
        reply = {room_id: 'test-room', signaling_url: signaling, token: `admission-${++admissions}`}; break;
      case '/api/v1/rooms/end': reply = {status: 'ok'}; break;
      case '/api/v1/sessions/current': status = 204; break;
      default: status = 404;
    }
    response.writeHead(status, {'Content-Type': 'application/json'});
    response.end(reply && JSON.stringify(reply));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  return {calls, url: `http://127.0.0.1:${server.address().port}`,
    setSignaling: value => {signaling = value;}};
}

test('cluster bridge provisions real identities and fetches fresh admissions without leaking sessions', async t => {
  const fixture = await controlFixture(t);
  const bridge = await startClusterFrontend({controlUrl: fixture.url, httpPort: 0});
  t.after(() => bridge.stop());
  const config = await (await fetch(`${bridge.origin}/config`)).json();
  assert.deepEqual(config, {url: 'ws://127.0.0.1:18180/webrtc', defaultRoom: 'test-room',
    testRoles: ['publisher', 'viewer']});
  assert.match(await (await fetch(bridge.origin)).text(), /id="join-form"/);
  assert.equal((await fetch(`${bridge.origin}/cluster-server.mjs`)).status, 404);
  assert.deepEqual(fixture.calls.find(call => call.path.endsWith('/room-members')), {
    path: '/api/v1/room-members', method: 'POST', auth: 'Bearer publisher-session',
    body: {room_id: 'test-room', user_id: 'viewer', role: 'listener'}
  });
  const token = async role => {
    const response = await fetch(`${bridge.origin}/token`, {method: 'POST',
      headers: {'Content-Type': 'application/json', Origin: bridge.origin},
      body: JSON.stringify({room: bridge.room, role, identity: 'ignored-browser-identity'})});
    assert.equal(response.status, 200);
    return response.json();
  };
  const first = await token('publisher'), second = await token('publisher'), viewer = await token('viewer');
  assert.notDeepEqual(first, second);
  assert.deepEqual(viewer, {token: 'admission-4'});
  assert.deepEqual(fixture.calls.filter(call => call.path.endsWith('/room-admissions')).map(call => call.auth),
    ['Bearer publisher-session', 'Bearer publisher-session', 'Bearer publisher-session', 'Bearer viewer-session']);
  assert.deepEqual(await bridge.stop(), []);
  const cleanup = fixture.calls.slice(-3);
  assert.equal(cleanup[0].path, '/api/v1/rooms/end');
  assert.equal(cleanup[0].auth, 'Bearer publisher-session');
  assert.ok(cleanup.slice(1).every(call => call.method === 'DELETE' && call.path.endsWith('/sessions/current')));
});

test('cluster token endpoint rejects foreign origins, rooms, roles, malformed or oversized bodies', async t => {
  const fixture = await controlFixture(t);
  const bridge = await startClusterFrontend({controlUrl: fixture.url, httpPort: 0});
  t.after(() => bridge.stop());
  const before = fixture.calls.length;
  const valid = JSON.stringify({room: bridge.room, role: 'viewer'});
  async function request(body, headers = {}, method = 'POST') {
    return fetch(`${bridge.origin}/token`, {method, ...(method !== 'GET' && {body}),
      headers: {'Content-Type': 'application/json', Origin: bridge.origin, ...headers}});
  }
  assert.equal((await request(valid, {Origin: 'https://other.example'})).status, 403);
  assert.equal((await request(valid, {'Content-Type': 'text/plain'})).status, 403);
  assert.equal((await request(valid, {}, 'GET')).status, 405);
  assert.equal((await request('{')).status, 400);
  assert.equal((await request('x'.repeat(2049))).status, 413);
  assert.equal((await request(JSON.stringify({room: 'other-room', role: 'viewer'}))).status, 403);
  assert.equal((await request(JSON.stringify({room: bridge.room, role: 'participant'}))).status, 403);
  assert.equal(fixture.calls.length, before);
  fixture.setSignaling('ws://127.0.0.1:18181/webrtc');
  const changed = await request(valid);
  assert.equal(changed.status, 502);
  assert.doesNotMatch(await changed.text(), /admission-\d/);
});

test('closed registration gives an actionable error and releases the HTTP listener', async t => {
  const fixture = await controlFixture(t, {registration: false});
  await assert.rejects(startClusterFrontend({controlUrl: fixture.url, httpPort: 0}),
    /enable registration_enabled.*recreate Control/);
  assert.equal(fixture.calls.length, 1);
});

test('failed bootstrap revokes sessions and ends its room', async t => {
  const fixture = await controlFixture(t);
  fixture.setSignaling('ws://203.0.113.10:18180/webrtc');
  await assert.rejects(startClusterFrontend({controlUrl: fixture.url, httpPort: 0}), /loopback/);
  assert.equal(fixture.calls.at(-3).path, '/api/v1/rooms/end');
  assert.equal(fixture.calls.filter(call => call.method === 'DELETE').length, 2);
});

test('remote Control URLs are rejected before sending test credentials', async () => {
  await assert.rejects(startClusterFrontend({controlUrl: 'http://203.0.113.10:18082'}), /loopback/);
});
