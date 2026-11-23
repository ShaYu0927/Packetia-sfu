import {isIPv4} from 'node:net';

// Browser-facing HTTP/WS URLs stay on localhost for SSH port forwarding.
// The advertised media address is independent of the process bind address.
export function readDemoSettings(env = process.env) {
  const port = (name, fallback) => {
    const value = env[name] === undefined ? fallback : Number(env[name]);
    if (!Number.isInteger(value) || value < 1 || value > 65535) throw new Error(`Invalid ${name}`);
    return value;
  };
  const publicIp = env.PACKETIA_DEMO_PUBLIC_IP ?? '127.0.0.1';
  const octets = publicIp.split('.').map(Number);
  if (!isIPv4(publicIp) || octets[0] === 0 || octets[0] >= 224 ||
      (octets[0] === 169 && octets[1] === 254))
    throw new Error('PACKETIA_DEMO_PUBLIC_IP must be a reachable IPv4 address');
  const listenIp = env.PACKETIA_DEMO_LISTEN_IP ?? '127.0.0.1';
  if (!['127.0.0.1', '0.0.0.0'].includes(listenIp))
    throw new Error('PACKETIA_DEMO_LISTEN_IP must be 127.0.0.1 or 0.0.0.0');
  return {httpPort: port('PACKETIA_DEMO_HTTP_PORT', 18000),
    wsPort: port('PACKETIA_DEMO_WS_PORT', 18080),
    mediaPort: port('PACKETIA_DEMO_MEDIA_PORT', 19000),
    turnPort: port('PACKETIA_DEMO_TURN_PORT', 13478),
    publicIp, listenIp, localTurnEnabled: publicIp === '127.0.0.1'};
}
