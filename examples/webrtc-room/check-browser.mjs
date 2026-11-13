import {chromium} from 'playwright';
import {mkdir, writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const directory = dirname(fileURLToPath(import.meta.url));
const artifacts = resolve(directory, 'artifacts');
await mkdir(artifacts, {recursive: true});
const url = process.env.PACKETIA_DEMO_URL || 'http://127.0.0.1:18000';
async function until(operation, timeout = 30000) {
  const deadline = Date.now() + timeout;
  do {
    if (await operation()) return;
    await new Promise(resolve => setTimeout(resolve, 250));
  } while (Date.now() < deadline);
  throw new Error('Browser media condition timed out');
}
function require(condition, message) {if (!condition) throw new Error(message);}
const options = {headless: true, channel: 'chrome', args: ['--use-fake-device-for-media-stream',
  '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required']};
if (process.env.PACKETIA_CHROME_EXECUTABLE) {
  delete options.channel; options.executablePath = process.env.PACKETIA_CHROME_EXECUTABLE;
}
const servers = [], pages = [], errors = [], logs = [];
const snapshot = page => page.evaluate(() => window.demo.stats());
const watch = page => page.evaluate(() => window.demo.watch('*'));
try {
  // Separate browser processes produce independent ICE transports and identities.
  for (let i = 0; i < 3; ++i) {
    const server = await chromium.launchServer(options); servers.push(server);
    const browser = await chromium.connect(server.wsEndpoint());
    const context = await browser.newContext({permissions: ['camera', 'microphone'], viewport: {width: 1280, height: 900}});
    const page = await context.newPage(); pages.push(page);
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(url); await page.waitForFunction(() => Boolean(window.demo));
  }
  const [a, b, c] = pages, room = `duplex-${Date.now()}`;
  for (const page of pages) {
    await page.locator('#room').fill(room);
    await page.locator('#role').selectOption('participant');
    // Exercise relay on both directions of B; A and C may use direct UDP.
    if (page === b) await page.locator('#relay').check();
    await page.locator('#join').click();
    await page.waitForFunction(() => document.getElementById('connection').textContent === 'Joined');
    await page.locator('#publish').click();
    await page.waitForFunction(() => document.getElementById('local-status').textContent === 'Publishing', null, {timeout: 30000});
  }
  for (const page of pages) {
    await page.waitForFunction(() => !document.getElementById('watch').disabled);
    await page.locator('#watch').click();
  }
  await until(async () => {
    const states = await Promise.all(pages.map(snapshot));
    return states.every(state => state.mode === 'duplex' && state.publish?.state === 'connected' &&
      state.subscribe?.state === 'connected' && state.publish.sentBytes > 0 && state.subscribe.audioBytes > 0 &&
      state.subscribe.frames >= 30 && state.remoteVideos.length === 2 && state.remoteVideos.every(video => video.width > 0)) &&
      states[1].publish.candidateType === 'relay' && states[1].subscribe.candidateType === 'relay';
  }, 60000);
  const baseline = await Promise.all(pages.map(snapshot)); logs.push({phase: 'three-way-media', states: baseline});
  for (const page of pages) {
    const pixels = await page.evaluate(() => [...document.querySelectorAll('#remotes video')].map(video => {
      const canvas = document.createElement('canvas'); canvas.width = 64; canvas.height = 36;
      const context = canvas.getContext('2d'); context.drawImage(video, 0, 0, 64, 36);
      const data = context.getImageData(0, 0, 64, 36).data;
      let minimum = 255, maximum = 0;
      for (let i = 0; i < data.length; i += 4) for (let channel = 0; channel < 3; ++channel) {
        minimum = Math.min(minimum, data[i + channel]); maximum = Math.max(maximum, data[i + channel]);
      }
      return {minimum, maximum, currentTime: video.currentTime};
    }));
    require(pixels.length === 2 && pixels.every(pixel => pixel.maximum - pixel.minimum >= 20 && pixel.currentTime > 0),
      'A remote video is blank or not playing');
  }
  await b.screenshot({path: resolve(artifacts, 'duplex-desktop.png'), fullPage: true});
  await b.setViewportSize({width: 390, height: 844});
  await b.screenshot({path: resolve(artifacts, 'duplex-mobile.png'), fullPage: true});
  require(!await b.evaluate(() => document.documentElement.scrollWidth > innerWidth), 'Mobile horizontal overflow');

  // One signaling recovery must retain both IDs and restart both ICE sessions.
  const beforeRecovery = await snapshot(b);
  await b.evaluate(() => window.demo.dropSignaling());
  await until(async () => {
    const state = await snapshot(b);
    return state.recoveryCount > beforeRecovery.recoveryCount && state.participant === beforeRecovery.participant &&
      state.publish?.sessionId === beforeRecovery.publish.sessionId && state.subscribe?.sessionId === beforeRecovery.subscribe.sessionId &&
      state.publish.restarts > beforeRecovery.publish.restarts && state.subscribe.restarts > beforeRecovery.subscribe.restarts &&
      state.publish.state === 'connected' && state.subscribe.state === 'connected' &&
      state.subscribe.frames >= beforeRecovery.subscribe.frames + 15;
  });
  logs.push({phase: 'dual-session-recovery', state: await snapshot(b)});

  // Removing B's camera leaves its receiver and all remote audio/video alive.
  await b.evaluate(() => {window.cameraTracks = document.getElementById('local').srcObject.getTracks();});
  await b.locator('#stop-publish').click();
  await b.waitForFunction(() => !document.getElementById('publish').disabled);
  require(await b.evaluate(() => window.cameraTracks.every(track => track.readyState === 'ended')), 'Camera not released');
  const receivingOnly = await snapshot(b);
  require(!receivingOnly.publish && receivingOnly.subscribe.sessionId === beforeRecovery.subscribe.sessionId &&
    receivingOnly.participant === beforeRecovery.participant, 'Stop publishing lost receiving session or identity');
  await until(async () => (await snapshot(b)).subscribe.frames >= receivingOnly.subscribe.frames + 15);
  await until(async () => (await a.evaluate(() => window.demo.refreshTracks())).length === 2);
  await b.locator('#publish').click();
  await b.waitForFunction(() => document.getElementById('local-status').textContent === 'Publishing', null, {timeout: 30000});
  const republished = await snapshot(b);
  require(republished.publish.sessionId !== beforeRecovery.publish.sessionId &&
    republished.subscribe.sessionId === beforeRecovery.subscribe.sessionId, 'Republish changed receiving session');
  // Published replacements need explicit resubscription with the current source instances.
  for (const page of [a, c]) {await page.evaluate(() => window.demo.stopMedia('subscribe')); await watch(page);}
  await until(async () => (await snapshot(a)).remoteVideos.length === 2 && (await snapshot(c)).remoteVideos.length === 2);

  // Removing B's subscriber keeps B publishing to the other two browsers.
  const beforeStopWatching = await snapshot(b), aBefore = await snapshot(a);
  await b.locator('#stop-watch').click();
  await b.waitForFunction(() => !document.getElementById('watch').disabled);
  const sendingOnly = await snapshot(b);
  require(!sendingOnly.subscribe && sendingOnly.publish.sessionId === beforeStopWatching.publish.sessionId,
    'Stop watching lost publishing session');
  await until(async () => (await snapshot(a)).subscribe.frames >= aBefore.subscribe.frames + 15 &&
    (await snapshot(b)).publish.sentBytes > sendingOnly.publish.sentBytes);
  await b.locator('#watch').click();
  await until(async () => {
    const state = await snapshot(b);
    return state.mode === 'duplex' && state.subscribe.sessionId !== beforeStopWatching.subscribe.sessionId &&
      state.publish.sessionId === beforeStopWatching.publish.sessionId && state.subscribe.frames >= 15 && state.subscribe.audioBytes > 0;
  });
  logs.push({phase: 'independent-stop-and-recreate', states: await Promise.all(pages.map(snapshot))});
  for (const page of pages) {
    await page.locator('#leave').click();
    await page.waitForFunction(() => document.getElementById('connection').textContent === 'Disconnected');
    require(await page.evaluate(() => document.getElementById('local').srcObject === null &&
      document.querySelectorAll('#remotes video').length === 0), 'Leave retained local or remote media');
  }
  require(!errors.length, `Browser errors: ${errors.join('; ')}`);
  await writeFile(resolve(artifacts, 'duplex-result.json'), JSON.stringify({passed: true, logs}, null, 2));
  console.log('PASS three-browser bidirectional audio/video, TURN, dual ICE recovery, independent stop/recreate and leave');
} catch (error) {
  const states = await Promise.all(pages.map(page => snapshot(page).catch(() => ({}))));
  await writeFile(resolve(artifacts, 'duplex-result.json'), JSON.stringify({passed: false, error: error.stack, errors, states, logs}, null, 2));
  for (const [i, page] of pages.entries()) await page.screenshot({path: resolve(artifacts, `duplex-failure-${i}.png`), fullPage: true}).catch(() => {});
  console.error(error.stack); console.error(JSON.stringify(states)); process.exitCode = 1;
} finally {
  await Promise.race([Promise.allSettled(servers.map(server => server.close())), new Promise(resolve => setTimeout(resolve, 5000))]);
  for (const server of servers) {
    const child = server.process();
    if (child.exitCode !== null || child.signalCode !== null) continue;
    try {child.kill('SIGKILL');} catch {}
  }
  process.exit(process.exitCode || 0);
}
