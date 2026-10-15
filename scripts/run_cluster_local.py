#!/usr/bin/env python3
"""Start one control and two SFUs with persistent local data and generated secrets."""
import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import time


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, default=root / 'build/webrtc-integration')
    parser.add_argument('--data-dir', type=Path, default=root / 'build/cluster-local')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    data = args.data_dir.resolve()
    control, sfu = build / 'server/PacketiaControl', build / 'server/PacketiaSfu'
    if not control.is_file() or not sfu.is_file():
        parser.error('build PacketiaControl and PacketiaSfu first (see docs/distributed-services.md)')
    data.mkdir(parents=True, exist_ok=True)
    # Exclude two local launchers sharing databases/keys. Lock releases even after a crash.
    import fcntl
    lock = (data / 'launcher.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('another local launcher is using this data directory')
    credentials_path = data / 'credentials.json'
    if not credentials_path.exists():
        credentials = {'jwt': secrets.token_hex(32), 'nodes': {name: secrets.token_hex(32) for name in ('sfu-a', 'sfu-b')}}
        descriptor = os.open(credentials_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as output:
            json.dump(credentials, output)
    else:
        credentials = json.loads(credentials_path.read_text())
    env = dict(os.environ, PACKETIA_JWT_ISSUER='packetia-local', PACKETIA_JWT_SECRET=credentials['jwt'],
               PACKETIA_NODE_SECRETS=json.dumps(credentials['nodes']))
    processes, logs = [], []
    stopping = False

    def stop_signal(_signum, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop_signal)
    signal.signal(signal.SIGTERM, stop_signal)

    def launch(name, binary, config_name, extra=None):
        cwd = data / name
        cwd.mkdir(exist_ok=True)
        log_path = cwd / 'console.log'
        output = log_path.open('wb')
        logs.append(output)
        process = subprocess.Popen([str(binary), str(root / 'config' / config_name)], cwd=cwd,
            env=dict(env, **(extra or {})), stdout=output, stderr=subprocess.STDOUT)
        processes.append(process)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not stopping:
            if process.poll() is not None:
                raise RuntimeError(f'{name} exited; inspect {log_path}')
            if 'PACKETIA_READY' in log_path.read_text(errors='replace'):
                return
            time.sleep(0.1)
        raise RuntimeError(f'{name} did not become ready; inspect {log_path}')

    try:
        launch('control', control, 'packetia.control-local.json')
        for name in ('sfu-a', 'sfu-b'):
            launch(name, sfu, f'packetia.{name}-local.json', {'PACKETIA_NODE_SECRET': credentials['nodes'][name]})
        print('Control API: http://127.0.0.1:18082', flush=True)
        print('SFUs: ws://127.0.0.1:18180/webrtc, ws://127.0.0.1:18181/webrtc', flush=True)
        print(f'Data and logs: {data}\nPress Ctrl+C to stop all three processes.', flush=True)
        while not stopping:
            if any(process.poll() is not None for process in processes):
                raise RuntimeError(f'a process exited; inspect logs in {data}')
            time.sleep(0.2)
    finally:
        # Stop SFUs before control; each process drains its own I/O and workers.
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for output in logs:
            output.close()
        lock.close()


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        raise SystemExit(str(error)) from None
