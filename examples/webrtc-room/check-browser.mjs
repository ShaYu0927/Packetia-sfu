import {chromium} from 'playwright';
import {mkdir} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const directory = dirname(fileURLToPath(import.meta.url));
const artifacts = resolve(directory, 'artifacts');
await mkdir(artifacts, {recursive: true});
const url = process.env.PACKETIA_DEMO_URL || 'http://127.0.0.1:18000';
async function until(operation, timeout = 60000) {
  const deadline = Date.now() + timeout;
  do {
    if (await operation()) return;
    await new Promise(resolve => setTimeout(resolve, 250));
  } while (Date.now() < deadline);
  throw new Error('Browser media condition timed out');
}
const options = {headless: true, channel: 'chrome', args: ['--use-fake-device-for-media-stream',
  '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required']};
if (process.env.PACKETIA_CHROME_EXECUTABLE) {
  delete options.channel;
  options.executablePath = process.env.PACKETIA_CHROME_EXECUTABLE;
}
const publisherServer = await chromium.launchServer(options);
const publisherBrowser = await chromium.connect(publisherServer.wsEndpoint());
let viewerServer, viewerBrowser;
let a, b;
try {
  viewerServer = await chromium.launchServer(options);
  viewerBrowser = await chromium.connect(viewerServer.wsEndpoint());
  const contextA = await publisherBrowser.newContext({permissions: ['camera', 'microphone'], viewport: {width: 1280, height: 800}});
  const contextB = await viewerBrowser.newContext({permissions: ['camera', 'microphone'], viewport: {width: 1280, height: 800}});
  a = await contextA.newPage(); b = await contextB.newPage();
  const errors = [];
  for (const page of [a, b]) page.on('pageerror', error => errors.push(error.message));
  await a.goto(url); await b.goto(url);
  await a.waitForFunction(() => Boolean(window.demo)); await b.waitForFunction(() => Boolean(window.demo));
  const room = `check-${Date.now()}`;
  await a.locator('#room').fill(room); await a.locator('#join').click();
  await a.waitForFunction(() => document.getElementById('connection').textContent === 'Joined');
  await a.locator('#publish').click();
  await a.waitForFunction(() => document.getElementById('local-status').textContent === 'Publishing', null, {timeout: 30000});
  await b.locator('#room').fill(room); await b.locator('#relay').check(); await b.locator('#join').click();
  await b.waitForFunction(() => !document.getElementById('watch').disabled);
  await b.locator('#watch').click();
  await until(async () => {
    const stats = await b.evaluate(() => window.demo.stats());
    return stats.frames >= 30 && stats.audioBytes > 0 && stats.candidateType === 'relay' && stats.width > 0;
  });
  const first = await b.evaluate(() => window.demo.stats());
  await until(async () => (await b.evaluate(() => window.demo.stats())).frames >= first.frames + 15, 15000);
  const publisher = await a.evaluate(() => window.demo.stats());
  const viewer = await b.evaluate(() => window.demo.stats());
  console.log(JSON.stringify({publisher, viewer}));
  await b.evaluate(() => Promise.race([document.getElementById('remote').play(),
    new Promise((_, reject) => setTimeout(() => reject(new Error('Remote playback timed out')), 5000))]));
  await b.waitForFunction(() => document.getElementById('remote').currentTime > 0);
  const pixels = await b.evaluate(() => {
    const canvas = document.createElement('canvas'); canvas.width = 64; canvas.height = 36;
    const context = canvas.getContext('2d'); context.drawImage(document.getElementById('remote'), 0, 0, 64, 36);
    const data = context.getImageData(0, 0, 64, 36).data;
    let minimum = 255, maximum = 0;
    for (let i = 0; i < data.length; i += 4)
      for (let channel = 0; channel < 3; ++channel) {
        minimum = Math.min(minimum, data[i + channel]); maximum = Math.max(maximum, data[i + channel]);
      }
    return {minimum, maximum, currentTime: document.getElementById('remote').currentTime};
  });
  if (pixels.maximum - pixels.minimum < 20) throw new Error('Remote video pixels are blank');
  await b.screenshot({path: resolve(artifacts, 'viewer-desktop.png'), fullPage: true});
  await b.setViewportSize({width: 390, height: 844});
  await b.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  await b.screenshot({path: resolve(artifacts, 'viewer-mobile.png')});
  await b.locator('#remote').scrollIntoViewIfNeeded();
  await b.screenshot({path: resolve(artifacts, 'viewer-mobile-video.png')});
  if (await b.evaluate(() => document.documentElement.scrollWidth > innerWidth)) throw new Error('Mobile horizontal overflow');
  if (errors.length) throw new Error(`Browser errors: ${errors.join('; ')}`);
  console.log(JSON.stringify({publisher, viewer, pixels}, null, 2));
  await a.locator('#leave').click();
  await until(async () => (await b.evaluate(() => window.demo.refreshTracks())).length === 0, 10000);
  await b.locator('#leave').click();
  console.log('Browser room/TURN check passed; room tracks removed after publisher leave.');
} catch (error) {
  if (b && !b.isClosed()) {
    console.error(JSON.stringify(await b.evaluate(async () => ({stats: await window.demo.stats(),
      video: {paused: document.getElementById('remote').paused, readyState: document.getElementById('remote').readyState,
        currentTime: document.getElementById('remote').currentTime}, error: document.getElementById('error').textContent})).catch(() => ({}))));
    await b.screenshot({path: resolve(artifacts, 'viewer-failure.png'), fullPage: true}).catch(() => {});
  }
  console.error(error.stack);
  process.exitCode = 1;
} finally {
  const servers = [viewerServer, publisherServer].filter(Boolean);
  for (const server of servers) {
    const child = server.process();
    if (child.exitCode !== null || child.signalCode !== null) continue;
    try {
      if (process.platform === 'win32') child.kill('SIGKILL');
      else process.kill(-child.pid, 'SIGKILL');
    } catch (error) {if (error.code !== 'ESRCH') {console.error(error.message); process.exitCode = 1;}}
  }
  await Promise.race([Promise.allSettled(servers.map(server => server.kill())),
    new Promise(resolve => setTimeout(resolve, 5000))]);
  process.exit(process.exitCode || 0);
}
