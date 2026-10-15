#!/usr/bin/env python3
"""Real control + two SFU processes. Uses Python's standard library only."""
import argparse
import base64
import concurrent.futures
import contextlib
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def eventually(check, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.1)
    raise AssertionError('condition did not become true')


class WebSocket:
    def __init__(self, url):
        from urllib.parse import urlsplit
        parsed = urlsplit(url)
        self.sock = socket.create_connection((parsed.hostname, parsed.port), timeout=3)
        self.sock.settimeout(3)
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        self.sock.sendall((f'GET {parsed.path or "/"} HTTP/1.1\r\nHost: {parsed.netloc}\r\n'
                           'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                           f'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n').encode())
        header = bytearray()
        while not header.endswith(b'\r\n\r\n'):
            header.extend(self.read(1))
            assert len(header) < 16384
        assert b' 101 ' in header.split(b'\r\n')[0]
        expected = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert expected.lower() in header.lower()

    def read(self, size):
        data = bytearray()
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise AssertionError('WebSocket disconnected')
            data.extend(chunk)
        return bytes(data)

    def request(self, body):
        payload = json.dumps(body).encode()
        mask = secrets.token_bytes(4)
        length = len(payload)
        prefix = bytes([0x81, 0x80 | length]) if length < 126 else b'\x81\xfe' + struct.pack('!H', length)
        self.sock.sendall(prefix + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))
        while True:
            first, second = self.read(2)
            size = second & 127
            if size == 126:
                size = struct.unpack('!H', self.read(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', self.read(8))[0]
            assert size <= 1024 * 1024 and not second & 128
            data = self.read(size)
            opcode = first & 15
            if opcode == 9:
                self.sock.sendall(bytes([0x8a, 0x80 | size]) + b'\0\0\0\0' + data)
                continue
            assert opcode == 1 and first & 128, 'unexpected WebSocket frame'
            reply = json.loads(data)
            # Room notifications can precede the response.
            if reply.get('type') in ('participant_joined', 'participant_left', 'tracks_updated'):
                continue
            return reply

    def close(self):
        self.sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--control', required=True, type=Path)
    parser.add_argument('--sfu', required=True, type=Path)
    args = parser.parse_args()
    control_binary, sfu_binary = args.control.resolve(), args.sfu.resolve()
    processes = []
    logs = []
    connections = []
    with tempfile.TemporaryDirectory(prefix='packetia-cluster-test-') as directory:
        root = Path(directory)
        public_port, internal_port = port(), port()
        base = f'http://127.0.0.1:{public_port}'
        internal = f'http://127.0.0.1:{internal_port}'
        node_secrets = {name: secrets.token_hex(32) for name in ('sfu-a', 'sfu-b')}
        env = dict(os.environ, PACKETIA_JWT_ISSUER='integration', PACKETIA_JWT_SECRET=secrets.token_hex(32),
                   PACKETIA_NODE_SECRETS=json.dumps(node_secrets))
        control_config = root / 'control.json'
        control_config.write_text(json.dumps({'listen_ip': '127.0.0.1', 'http_port': public_port,
            'internal_listen_ip': '127.0.0.1', 'internal_http_port': internal_port,
            'account_database': str(root / 'accounts.db'), 'cluster_database': str(root / 'cluster.db'),
            'registration_enabled': True}))
        nodes = {}
        for name in node_secrets:
            ws_port = port()
            config = {'node_id': name, 'control_url': internal, 'signaling_url': f'ws://127.0.0.1:{ws_port}/webrtc',
                'max_rooms': 100, 'listen_ip': '127.0.0.1', 'public_ip': '127.0.0.1',
                'websocket_port': ws_port, 'udp_port': port(socket.SOCK_DGRAM), 'max_participants': 32}
            config_path = root / f'{name}.json'
            config_path.write_text(json.dumps(config))
            nodes[name] = (config, config_path)

        def launch(binary, config_path, extra_env=None):
            index = len(processes)
            cwd = root / f'process-{index}'
            cwd.mkdir()
            log_path = cwd / 'console.log'
            log = log_path.open('wb')
            logs.append(log)
            process = subprocess.Popen([str(binary), str(config_path)], cwd=cwd,
                env=dict(env, **(extra_env or {})), stdout=log, stderr=subprocess.STDOUT)
            processes.append(process)
            def ready():
                assert process.poll() is None, f'{binary.name} exited before readiness'
                return 'PACKETIA_READY' in log_path.read_text(errors='replace')
            eventually(ready, timeout=20)
            return process

        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        def http(path, body=None, token='', target=base, expected=200):
            headers = {'Content-Type': 'application/json'}
            if token:
                headers['Authorization'] = 'Bearer ' + token
            request = urllib.request.Request(target + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
            try:
                with opener.open(request, timeout=3) as response:
                    status, payload = response.status, json.load(response)
            except urllib.error.HTTPError as error:
                status, payload = error.code, json.load(error)
            assert status == expected, f'{path}: expected {expected}, got {status}, code={payload.get("error")}'
            return payload

        def connect(url):
            client = WebSocket(url)
            connections.append(client)
            return client

        def stop(process):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=6)
            assert process.returncode == 0, 'unclean process shutdown'

        try:
            control = launch(control_binary, control_config)
            http('/healthz')
            sessions, users = {}, {}
            for name in ('alice', 'bob', 'eve'):
                registered = http('/api/v1/accounts', {'login': name, 'password': 'integration-password-123', 'display_name': name}, expected=201)
                users[name] = registered['user']['id']
                sessions[name] = http('/api/v1/sessions', {'login': name, 'password': 'integration-password-123'})['session_token']
            alice, bob, eve = (sessions[name] for name in ('alice', 'bob', 'eve'))
            http('/api/v1/rooms', {}, expected=401)
            room = http('/api/v1/rooms', {}, alice, expected=201)['room_id']
            http('/api/v1/room-admissions', {'room_id': room}, alice, expected=503)
            node_processes = {}
            for name, (config, config_path) in nodes.items():
                node_processes[name] = launch(sfu_binary, config_path, {'PACKETIA_NODE_SECRET': node_secrets[name]})
            print('PASS: independent control and two SFUs started', flush=True)
            registration = dict(node_id='sfu-a', instance_id='rogue', signaling_url=nodes['sfu-a'][0]['signaling_url'], max_rooms=100, participants=0)
            http('/internal/v1/nodes/register', registration, node_secrets['sfu-a'], expected=404)
            http('/internal/v1/nodes/register', registration, alice, target=internal, expected=401)
            http('/api/v1/room-admissions', {'room_id': room}, eve, expected=403)
            http('/api/v1/room-members', {'room_id': room, 'user_id': users['eve'], 'role': 'speaker'}, eve, expected=403)
            http('/api/v1/room-members', {'room_id': room, 'user_id': users['bob'], 'role': 'listener'}, alice)
            http('/api/v1/room-admissions', {'room_id': room, 'user_id': users['eve']}, alice, expected=400)
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
                assignments = list(executor.map(lambda _: http('/api/v1/room-admissions', {'room_id': room}, alice), range(24)))
            admission = assignments[0]
            assert len({(item['node_id'], item['instance_id']) for item in assignments}) == 1
            second_room = http('/api/v1/rooms', {}, alice, expected=201)['room_id']
            second = http('/api/v1/room-admissions', {'room_id': second_room}, alice)
            assert admission['node_id'] != second['node_id']
            print('PASS: authenticated membership, concurrent room affinity and room distribution', flush=True)
            join = {'type': 'join', 'room_id': room, 'token': admission['token']}
            client = connect(admission['signaling_url'])
            assert client.request(dict(join, token='invalid')).get('code') == 'unauthenticated'
            assert client.request(dict(join, room_id=second_room)).get('code') == 'forbidden'
            assert client.request(join)['type'] == 'joined'
            other = connect(second['signaling_url'])
            assert other.request(join).get('code') == 'forbidden'
            listener_admission = http('/api/v1/room-admissions', {'room_id': room}, bob)
            listener = connect(listener_admission['signaling_url'])
            assert listener.request(dict(join, token=listener_admission['token']))['type'] == 'joined'
            assert listener.request({'type': 'publish', 'sdp': 'invalid'}).get('code') == 'forbidden'
            http('/api/v1/room-members', {'room_id': room, 'user_id': users['eve'], 'role': 'listener'}, alice)
            recovery_admission = http('/api/v1/room-admissions', {'room_id': room}, eve)
            recovery_join = dict(join, token=recovery_admission['token'])
            print('PASS: real WebSocket admission, wrong-node rejection and listener permissions', flush=True)
            stop(control)
            time.sleep(8.5)
            rejected = connect(admission['signaling_url'])
            assert rejected.request(recovery_join).get('code') == 'forbidden', 'SFU admitted after its control lease expired'
            assert client.request({'type': 'tracks'})['type'] == 'tracks', 'existing participant lost authorization'
            control = launch(control_binary, control_config)
            eventually(lambda: connect(admission['signaling_url']).request(recovery_join).get('type') == 'joined', timeout=6)
            restored = http('/api/v1/room-admissions', {'room_id': room}, alice)
            assert (restored['node_id'], restored['instance_id']) == (admission['node_id'], admission['instance_id'])
            http('/api/v1/room-admissions', {'room_id': room}, bob)
            print('PASS: expired control lease blocks joins; control restart preserves sessions, members and bindings', flush=True)
            assigned_id = admission['node_id']
            stop(node_processes[assigned_id])
            def unavailable():
                try:
                    http('/api/v1/room-admissions', {'room_id': room}, alice, expected=503)
                    return True
                except AssertionError:
                    return False
            eventually(unavailable, timeout=12)
            replacement = launch(sfu_binary, nodes[assigned_id][1], {'PACKETIA_NODE_SECRET': node_secrets[assigned_id]})
            node_processes[assigned_id] = replacement
            http('/api/v1/room-admissions', {'room_id': room}, alice, expected=503)
            old = connect(admission['signaling_url'])
            # Renew the token so this check proves instance fencing independently of expiry.
            parts = admission['token'].split('.')
            claims = json.loads(base64.urlsafe_b64decode(parts[1] + '=='))
            claims['iat'], claims['exp'] = int(time.time()), int(time.time()) + 60
            parts[1] = base64.urlsafe_b64encode(json.dumps(claims).encode()).rstrip(b'=').decode()
            import hmac
            parts[2] = base64.urlsafe_b64encode(hmac.new(env['PACKETIA_JWT_SECRET'].encode(), '.'.join(parts[:2]).encode(), hashlib.sha256).digest()).rstrip(b'=').decode()
            assert old.request(dict(join, token='.'.join(parts))).get('code') == 'forbidden'
            heartbeat = {'node_id': assigned_id, 'instance_id': admission['instance_id'], 'participants': 0}
            http('/internal/v1/nodes/heartbeat', heartbeat, node_secrets[assigned_id], target=internal, expected=409)
            new_room = http('/api/v1/rooms', {}, alice, expected=201)['room_id']
            new_admission = http('/api/v1/room-admissions', {'room_id': new_room}, alice)
            new_client = connect(new_admission['signaling_url'])
            assert new_client.request({'type': 'join', 'room_id': new_room, 'token': new_admission['token']})['type'] == 'joined'
            print('PASS: SFU restart fences old instance; lost rooms stay unavailable; new rooms remain usable', flush=True)
        finally:
            for connection in connections:
                with contextlib.suppress(OSError):
                    connection.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
            for log in logs:
                log.close()


if __name__ == '__main__':
    main()
