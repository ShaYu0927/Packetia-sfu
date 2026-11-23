import {readFile} from 'node:fs/promises';
import {startClusterFrontend} from './cluster-server.mjs';

try {
  const directory = process.env.PACKETIA_BROWSER_CONFIG_DIR || '/etc/packetia-browser-test';
  const config = JSON.parse(await readFile(`${directory}/config.json`, 'utf8'));
  const bridge = await startClusterFrontend({httpPort: Number(new URL(config.origin).port || 443),
    publicAccess: {...config, tls: {
      cert: await readFile(`${directory}/server.crt`), key: await readFile(`${directory}/server.key`)
    }}});
  console.log(`Public cluster browser test: ${bridge.origin}\nRoom: ${bridge.room}`);
  console.log('Log in with the test password, then use Publisher and Viewer in two windows.');
  console.log('Stopping this service ends its test room and revokes its account sessions.');
  const shutdown = async () => {
    for (const error of await bridge.stop()) console.error(`Cleanup failed: ${error}`);
  };
  process.once('SIGINT', shutdown); process.once('SIGTERM', shutdown);
} catch (error) {
  console.error(`Public browser test failed: ${error.message}`);
  process.exitCode = 1;
}
