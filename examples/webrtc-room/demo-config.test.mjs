import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readDemoSettings} from './demo-config.mjs';

test('local demo retains loopback listeners and TURN', () => {
  const settings = readDemoSettings({});
  assert.equal(settings.publicIp, '127.0.0.1');
  assert.equal(settings.listenIp, '127.0.0.1');
  assert.equal(settings.httpPort, 18000);
  assert.equal(settings.wsPort, 18080);
  assert.equal(settings.mediaPort, 19000);
  assert.equal(settings.localTurnEnabled, true);
});

test('SSH demo separates public media from container bind addresses and disables local TURN', () => {
  const settings = readDemoSettings({PACKETIA_DEMO_PUBLIC_IP: '203.0.113.10',
    PACKETIA_DEMO_LISTEN_IP: '0.0.0.0', PACKETIA_DEMO_HTTP_PORT: '18100',
    PACKETIA_DEMO_WS_PORT: '18180', PACKETIA_DEMO_MEDIA_PORT: '19100'});
  assert.equal(settings.publicIp, '203.0.113.10');
  assert.equal(settings.listenIp, '0.0.0.0');
  assert.equal(settings.httpPort, 18100);
  assert.equal(settings.wsPort, 18180);
  assert.equal(settings.mediaPort, 19100);
  assert.equal(settings.localTurnEnabled, false);
});

test('invalid advertised addresses, wildcard media addresses and hostnames are rejected', () => {
  for (const address of ['', '0.0.0.0', '224.0.0.1', '169.254.1.1', '::1',
                         'rtc.example.com', '203.0.113.10:19000'])
    assert.throws(() => readDemoSettings({PACKETIA_DEMO_PUBLIC_IP: address}), /IPv4/);
  assert.throws(() => readDemoSettings({PACKETIA_DEMO_LISTEN_IP: '203.0.113.10'}), /LISTEN_IP/);
});

test('ports reject malformed values before a child process is launched', () => {
  for (const name of ['PACKETIA_DEMO_HTTP_PORT', 'PACKETIA_DEMO_WS_PORT',
                       'PACKETIA_DEMO_MEDIA_PORT', 'PACKETIA_DEMO_TURN_PORT'])
    for (const value of ['0', '-1', '65536', 'abc', '80.5', ''])
      assert.throws(() => readDemoSettings({[name]: value}), /Invalid/);
});
