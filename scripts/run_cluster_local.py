#!/usr/bin/env python3
"""Start one control and a configurable number of local SFUs."""
import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import threading
import time


CONSOLE_LOCK = threading.Lock()


def status(message):
    with CONSOLE_LOCK:
        print(message, flush=True)


class LogRelay:
    """Drain a child's output to its persistent log and the shared terminal."""
    def __init__(self, name, path, console=None, max_bytes=16 * 1024 * 1024, backups=3):
        if max_bytes < 0 or not 1 <= backups <= 64:
            raise ValueError('invalid log rotation limits')
        self.name = name
        self.path = path
        self.console = console
        self.output = path.open('ab', buffering=0)
        self.bytes = path.stat().st_size
        self.max_bytes = max_bytes
        self.backups = backups
        self.ready = threading.Event()
        self.error = None
        self.thread = None

    def start(self, stream):
        self.thread = threading.Thread(target=self._read, args=(stream,),
                                       name=f'logs-{self.name}', daemon=True)
        try:
            self.thread.start()
        except RuntimeError:
            self.thread = None
            stream.close()
            self.output.close()
            raise

    def _rotate(self):
        self.output.close()
        for index in range(self.backups, 0, -1):
            destination = Path(str(self.path) + f'.{index}')
            source = self.path if index == 1 else Path(str(self.path) + f'.{index - 1}')
            if destination.exists():
                if not destination.is_file():
                    raise OSError(f'log backup is not a regular file: {destination}')
                destination.unlink()
            if source.exists():
                if not source.is_file():
                    raise OSError(f'log source is not a regular file: {source}')
                source.rename(destination)
        self.output = self.path.open('ab', buffering=0)
        self.bytes = 0

    def _read(self, stream):
        try:
            with stream:
                # Bound a single read even if a child emits an unusually long line.
                for line in iter(lambda: stream.readline(65536), b''):
                    if self.max_bytes and self.bytes and self.bytes + len(line) > self.max_bytes:
                        self._rotate()
                    if self.output.write(line) != len(line):
                        raise OSError('incomplete log file write')
                    self.bytes += len(line)
                    if line.rstrip(b'\r\n') == b'PACKETIA_READY':
                        self.ready.set()
                    if self.console is not None:
                        text = line.decode('utf-8', errors='replace').replace('\r\n', '\n')
                        try:
                            with CONSOLE_LOCK:
                                print(f'[{self.name}] {text}', end='' if text.endswith('\n') else '\n',
                                      file=self.console, flush=True)
                        except (OSError, ValueError):
                            # A closed terminal must not prevent draining or saving child logs.
                            self.console = None
        except (OSError, ValueError) as error:
            self.error = error
        finally:
            self.output.close()

    def close(self):
        if self.thread is not None:
            self.thread.join()
        else:
            self.output.close()


def find_build(root, requested=None):
    candidates = [requested.resolve()] if requested else [
        root / 'build', root / 'build/webrtc-integration', root / 'build/macos', root / 'build-system']
    for build in candidates:
        if all((build / 'server' / name).is_file() for name in ('PacketiaControl', 'PacketiaSfu')):
            return build
    raise RuntimeError('PacketiaControl and PacketiaSfu were not found. Build them first:\n'
                       '  cmake -S . -B build\n'
                       '  cmake --build build --target PacketiaControl PacketiaSfu --parallel 6\n'
                       'Or pass --build-dir PATH for an existing build.')


def node_names(count):
    if not 1 <= count <= 64:
        raise ValueError('--nodes must be between 1 and 64')
    return [f'sfu-{chr(ord("a") + i)}' if i < 26 else f'sfu-{i + 1}' for i in range(count)]


def load_credentials(path, names):
    credentials = json.loads(path.read_text()) if path.exists() else {
        'jwt': secrets.token_hex(32), 'nodes': {}}
    if (not isinstance(credentials, dict) or not isinstance(credentials.get('jwt'), str) or
            not 32 <= len(credentials['jwt']) <= 4096 or not isinstance(credentials.get('nodes'), dict) or
            any(not isinstance(value, str) or not 32 <= len(value) <= 4096
                for value in credentials['nodes'].values())):
        raise RuntimeError(f'invalid credentials file: {path}; preserve existing keys when repairing it')
    missing = [name for name in names if name not in credentials['nodes']]
    for name in missing:
        credentials['nodes'][name] = secrets.token_hex(32)
    if missing or not path.exists():
        # The caller holds the launcher lock. Publish a complete private file atomically.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as output:
                temporary = Path(output.name)
                json.dump(credentials, output)
            os.chmod(temporary, 0o600)
            os.replace(temporary, path)
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
    return credentials


def runtime_configs(root, data, names):
    control = json.loads((root / 'config/packetia.control-local.json').read_text())
    first = json.loads((root / 'config/packetia.sfu-a-local.json').read_text())
    configs = [('control', control)]
    for index, name in enumerate(names):
        template = root / 'config' / f'packetia.{name}-local.json'
        if template.is_file():
            node = json.loads(template.read_text())
        else:
            node = dict(first, node_id=name,
                        websocket_port=first['websocket_port'] + index,
                        udp_port=first['udp_port'] + index)
            if 'metrics_http_port' in first:
                node['metrics_http_port'] = first['metrics_http_port'] + index
            host = node['public_ip']
            if ':' in host:
                host = f'[{host}]'
            node['signaling_url'] = f'ws://{host}:{node["websocket_port"]}/webrtc'
        if node['node_id'] != name:
            raise RuntimeError(f'{template}: node_id must be {name}')
        configs.append((name, node))
    # Catch local template conflicts before starting any processes.
    tcp = {(control['listen_ip'], control['http_port']),
           (control['internal_listen_ip'], control['internal_http_port'])}
    udp = set()
    if len(tcp) != 2:
        raise RuntimeError('control HTTP ports conflict')
    for name, node in configs[1:]:
        stream = (node['listen_ip'], node['websocket_port'])
        datagram = (node['listen_ip'], node['udp_port'])
        if (not 1 <= node['websocket_port'] <= 65535 or not 1 <= node['udp_port'] <= 65535 or
                stream in tcp or datagram in udp):
            raise RuntimeError(f'{name}: invalid or conflicting local ports')
        tcp.add(stream)
        udp.add(datagram)
        if 'metrics_http_port' in node:
            metrics = (node.get('metrics_listen_ip', '127.0.0.1'), node['metrics_http_port'])
            if not 1 <= metrics[1] <= 65535 or metrics in tcp:
                raise RuntimeError(f'{name}: invalid or conflicting metrics HTTP port')
            tcp.add(metrics)
    result = []
    for name, config in configs:
        directory = data / name
        directory.mkdir(exist_ok=True)
        path = directory / 'config.json'
        path.write_text(json.dumps(config, indent=2) + '\n')
        result.append((name, path, config))
    return result


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, help='build containing server/PacketiaControl and server/PacketiaSfu')
    parser.add_argument('--data-dir', type=Path, default=root / 'build/cluster-local')
    parser.add_argument('--nodes', type=int, default=2, help='number of local SFU processes (1..64, default: 2)')
    parser.add_argument('--quiet', action='store_true', help='save child logs to files without showing them in the terminal')
    parser.add_argument('--log-max-mb', type=int, default=16, help='log file size in MiB before rotation (0 disables, default: 16)')
    parser.add_argument('--log-backups', type=int, default=3, help='rotated log files to retain per process (1..64, default: 3)')
    args = parser.parse_args()
    try:
        names = node_names(args.nodes)
        if not 0 <= args.log_max_mb <= 1024 or not 1 <= args.log_backups <= 64:
            raise ValueError('--log-max-mb must be 0..1024 and --log-backups must be 1..64')
        build = find_build(root, args.build_dir)
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))
    data = args.data_dir.resolve()
    control, sfu = build / 'server/PacketiaControl', build / 'server/PacketiaSfu'
    data.mkdir(parents=True, exist_ok=True)
    # Exclude two local launchers sharing databases/keys. Lock releases even after a crash.
    import fcntl
    lock = (data / 'launcher.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('another local launcher is using this data directory')
    credentials = load_credentials(data / 'credentials.json', names)
    configs = runtime_configs(root, data, names)
    env = dict(os.environ, PACKETIA_JWT_ISSUER='packetia-local', PACKETIA_JWT_SECRET=credentials['jwt'],
               PACKETIA_NODE_SECRETS=json.dumps(credentials['nodes']))
    # The launcher owns file logs; keep C++ output on stdout to avoid duplicate app.log files.
    env.setdefault('PACKETIA_LOG_MODE', 'console')
    processes, logs = [], []
    stopping = False

    def stop_signal(_signum, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop_signal)
    signal.signal(signal.SIGTERM, stop_signal)

    def launch(name, binary, config_path, extra=None):
        cwd = data / name
        cwd.mkdir(exist_ok=True)
        log_path = cwd / 'console.log'
        relay = LogRelay(name, log_path, console=None if args.quiet else sys.stdout,
                         max_bytes=args.log_max_mb * 1024 * 1024, backups=args.log_backups)
        logs.append(relay)
        status(f'Starting {name}...')
        process = subprocess.Popen([str(binary), str(config_path)], cwd=cwd,
            env=dict(env, **(extra or {})), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        processes.append((name, process, log_path))
        relay.start(process.stdout)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not stopping:
            if relay.error is not None:
                raise RuntimeError(f'{name}: cannot save logs to {log_path}: {relay.error}')
            if process.poll() is not None:
                raise RuntimeError(f'{name} exited with code {process.returncode}; inspect {log_path}')
            if relay.ready.wait(0.1):
                return
        if stopping:
            raise InterruptedError()
        raise RuntimeError(f'{name} did not become ready; inspect {log_path}')

    try:
        launch('control', control, configs[0][1])
        for name, config_path, _ in configs[1:]:
            if stopping:
                raise InterruptedError()
            launch(name, sfu, config_path, {'PACKETIA_NODE_SECRET': credentials['nodes'][name]})
        http = configs[0][2]
        host = http['listen_ip']
        if ':' in host:
            host = f'[{host}]'
        status(f'Control API: http://{host}:{http["http_port"]}')
        for name, _, node in configs[1:]:
            status(f'{name}: {node["signaling_url"]} / UDP {node["udp_port"]}')
        status(f'Data and logs: {data}\nPress Ctrl+C to stop all {len(configs)} processes.')
        while not stopping:
            for relay in logs:
                if relay.error is not None:
                    raise RuntimeError(f'{relay.name}: cannot save logs to {relay.path}: {relay.error}')
            for name, process, log_path in processes:
                if process.poll() is not None:
                    raise RuntimeError(f'{name} exited with code {process.returncode}; inspect {log_path}')
            time.sleep(0.2)
    except InterruptedError:
        pass
    finally:
        # Stop SFUs before control; each process drains its own I/O and workers.
        for _, process, _ in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for relay in logs:
            relay.close()
        lock.close()


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from None
