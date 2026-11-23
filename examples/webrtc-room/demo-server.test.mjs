import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {createServer} from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';

async function freePort() {
  const server = createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}

test('remote demo serves localhost signaling, omits loopback TURN and retains token origin checks', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'packetia-demo-test-'));
  const fixture = join(directory, 'room-fixture');
  const addressFile = join(directory, 'addresses.json');
  // Substitute only the native process, so HTTP handling and credential wiring
  // execute through the actual launcher without requiring a media build.
  await writeFile(fixture, `#!${process.execPath}
import('node:fs').then(({writeFileSync}) => {
  writeFileSync(process.env.PACKETIA_FIXTURE_OUTPUT, JSON.stringify({
    publicIp: process.env.PACKETIA_DEMO_PUBLIC_IP,
    listenIp: process.env.PACKETIA_DEMO_LISTEN_IP,
    ports: process.argv.slice(2)
  }));
  console.log('ROOM_DEMO_READY');
  setInterval(() => {}, 1000);
});
`, {mode: 0o700});
  const httpPort = await freePort();
  const wsPort = await freePort();
  const child = spawn(process.execPath, ['dev-server.mjs'], {
    cwd: new URL('.', import.meta.url),
    env: {...process.env, PACKETIA_WEBRTC_TOKEN: undefined,
      PACKETIA_ROOM_DEMO_BINARY: fixture, PACKETIA_FIXTURE_OUTPUT: addressFile,
      PACKETIA_DEMO_PUBLIC_IP: '203.0.113.10', PACKETIA_DEMO_LISTEN_IP: '127.0.0.1',
      PACKETIA_DEMO_HTTP_PORT: String(httpPort), PACKETIA_DEMO_WS_PORT: String(wsPort),
      PACKETIA_DEMO_MEDIA_PORT: '19000', PACKETIA_DEMO_TURN_PORT: '13478'},
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', bytes => { output += bytes; });
  child.stderr.on('data', bytes => { output += bytes; });
  const exited = once(child, 'exit');
  try {
    const deadline = Date.now() + 10000;
    while (!output.includes('Room demo:')) {
      if (child.exitCode !== null || Date.now() >= deadline) throw new Error(`Demo failed: ${output}`);
      await delay(20);
    }
    const origin = `http://127.0.0.1:${httpPort}`;
    const config = await (await fetch(`${origin}/config`)).json();
    assert.deepEqual(config, {url: `ws://127.0.0.1:${wsPort}`});
    assert.deepEqual(JSON.parse(await readFile(addressFile, 'utf8')), {
      publicIp: '203.0.113.10', listenIp: '127.0.0.1',
      ports: [String(wsPort), '19000', '13478']
    });
    const body = JSON.stringify({identity: 'demo-00000000-0000-0000-0000-000000000000',
      room: 'public-test', role: 'publisher'});
    const request = allowedOrigin => fetch(`${origin}/token`, {method: 'POST', body,
      headers: {'Content-Type': 'application/json', Origin: allowedOrigin}});
    assert.equal((await request('http://203.0.113.10')).status, 403);
    const allowed = await request(origin);
    assert.equal(allowed.status, 200);
    const {token} = await allowed.json();
    const claims = JSON.parse(Buffer.from(token.split('.')[1], 'base64url'));
    assert.equal(claims.video.room, 'public-test');
    assert.equal(claims.video.canPublish, true);
  } finally {
    child.kill('SIGTERM');
    await exited;
    await rm(directory, {recursive: true, force: true});
  }
});
