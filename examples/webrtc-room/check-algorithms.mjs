import {spawn} from 'node:child_process';
import {mkdir, writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const directory = dirname(fileURLToPath(import.meta.url));
const root = resolve(directory, '../..'), artifacts = resolve(directory, 'artifacts');
const build = resolve(process.env.PACKETIA_BUILD_DIR || resolve(root, 'build/webrtc-integration'));
await mkdir(artifacts, {recursive: true});
const commands = [
  ['cmake', ['--build', build, '--target', 'test_weak_network_manual', 'test_rtp_input', 'test_media_pacer', 'test_webrtc_trendline', '-j4']],
  ['ctest', ['--test-dir', build, '--output-on-failure', '--timeout', '120',
    '--output-junit', resolve(artifacts, 'algorithms-junit.xml'),
    '-R', '^(webrtc_trendline|test_weak_network_manual|test_rtp_input|media_pacer)$']]
];
const result = {generated_at: new Date().toISOString(), passed: false, build_directory: build,
  scope: 'Local deterministic C++ tests, not a browser or production network benchmark', commands: []};
let output = '';
try {
  for (const [command, args] of commands) {
    console.log(`Running ${command} ${args.join(' ')}`);
    const code = await new Promise((resolveCode, reject) => {
      const child = spawn(command, args, {cwd: root, env: {...process.env, PACKETIA_LOG_MODE: 'console'}});
      child.stdout.on('data', data => {output += data.toString(); process.stdout.write(data);});
      child.stderr.on('data', data => {output += data.toString(); process.stderr.write(data);});
      child.once('error', reject); child.once('close', resolveCode);
    });
    result.commands.push({command, args, exit_code: code});
    if (code !== 0) throw new Error(`${command} failed (${code})`);
  }
  result.passed = true;
} catch (error) {result.error = error.message; console.error(error.message); process.exitCode = 1;}
await writeFile(resolve(artifacts, 'algorithms.log'), output);
await writeFile(resolve(artifacts, 'algorithms-result.json'), JSON.stringify(result, null, 2));
