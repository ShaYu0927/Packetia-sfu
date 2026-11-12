#!/usr/bin/env python3
"""验证控制进程到两台 SFU 的会议管理、代次隔离与回收链路，仅用标准库。"""
import argparse
import base64
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def port(kind=socket.SOCK_STREAM):
    with socket.socket(type=kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def post(base, path, body, token='', expected=200):
    request = urllib.request.Request(base + path, json.dumps(body).encode(),
        {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token})
    try:
        response = urllib.request.urlopen(request, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        value = json.load(response)
        check(response.status == expected, f'{path}: HTTP {response.status}, expected {expected}: {value}')
        return value


def get(base, path, expected=200):
    try:
        response = urllib.request.urlopen(base + path, timeout=2)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        value = json.load(response)
        if expected is not None:
            check(response.status == expected, f'{path}: HTTP {response.status}, expected {expected}')
        return value


def eventually(call, timeout=8):
    deadline = time.monotonic() + timeout
    while True:
        value = call()
        if value:
            return value
        check(time.monotonic() < deadline, 'condition did not converge')
        time.sleep(.05)


class WebSocket:
    def __init__(self, url):
        parsed = urlsplit(url)
        self.sock = socket.create_connection((parsed.hostname, parsed.port), timeout=4)
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        self.sock.sendall((f'GET {parsed.path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
            'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: packetia\r\n'
            f'Sec-WebSocket-Key: {key}\r\n\r\n').encode())
        response = b''
        while not response.endswith(b'\r\n\r\n'):
            response += self.read(1)
            check(len(response) < 16384, 'oversize upgrade response')
        check(b' 101 ' in response, 'upgrade failed')
        self.notifications = []

    def close(self):
        self.sock.close()

    def read(self, size):
        result = b''
        while len(result) < size:
            chunk = self.sock.recv(size - len(result))
            check(chunk, 'WebSocket closed unexpectedly')
            result += chunk
        return result

    def send(self, body, opcode=1):
        body = json.dumps(body).encode() if opcode == 1 else body
        size = len(body)
        header = bytes([128 | opcode])
        if size < 126:
            header += bytes([128 | size])
        elif size < 65536:
            header += bytes([254]) + struct.pack('!H', size)
        else:
            header += bytes([255]) + struct.pack('!Q', size)
        mask = secrets.token_bytes(4)
        self.sock.sendall(header + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(body)))

    def receive(self):
        message = b''
        while True:
            first, second = self.read(2)
            size = second & 127
            if size == 126:
                size = struct.unpack('!H', self.read(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', self.read(8))[0]
            check(size <= 1024 * 1024 and not second & 128, 'invalid server frame')
            body = self.read(size)
            if first & 15 == 9:
                self.send(body, 10)
                continue
            check(first & 15 in (0, 1), 'unexpected server frame')
            message += body
            if first & 128:
                return json.loads(message)

    def reply(self):
        while True:
            value = self.receive()
            if value.get('type') in ('participants_changed', 'tracks_changed', 'permissions_changed'):
                self.notifications.append(value)
            else:
                return value

    def request(self, body):
        self.send(body)
        return self.reply()


class Process:
    def __init__(self, binary, name, config, directory, env):
        path = directory / (name + '.json')
        path.write_text(json.dumps(config))
        self.log_path = directory / (name + '.log')
        self.log = self.log_path.open('wb')
        self.process = subprocess.Popen([str(Path(binary).resolve()), str(path)], env=env,
            stdout=self.log, stderr=subprocess.STDOUT, cwd=directory)
        try:
            self.wait_ready()
        except BaseException:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.log.close()
            raise

    def wait_ready(self):
        deadline = time.monotonic() + 16
        while b'PACKETIA_READY' not in self.log_path.read_bytes():
            check(self.process.poll() is None and time.monotonic() < deadline,
                'process startup failed: ' + self.log_path.read_text(errors='replace')[-5000:])
            time.sleep(.05)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError('process shutdown timed out')
        self.log.close()
        check(self.process.returncode == 0, f'process exited with {self.process.returncode}')


def run(control, sfu):
    processes, clients = [], []
    with tempfile.TemporaryDirectory(prefix='packetia-meeting-') as temporary:
        directory = Path(temporary)
        secret = secrets.token_hex(32)
        credentials = {name: secrets.token_hex(32) for name in ('sfu-a', 'sfu-b')}
        env = dict(os.environ, PACKETIA_JWT_ISSUER='test', PACKETIA_JWT_SECRET=secret,
            PACKETIA_NODE_SECRETS=json.dumps(credentials))
        public, internal = f'http://127.0.0.1:{port()}', f'http://127.0.0.1:{port()}'
        config = {'listen_ip': '127.0.0.1', 'http_port': urlsplit(public).port,
            'internal_listen_ip': '127.0.0.1', 'internal_http_port': urlsplit(internal).port,
            'account_database': str(directory / 'accounts.db'), 'cluster_database': str(directory / 'rooms.db'),
            'registration_enabled': True}
        try:
            processes.append(Process(control, 'control', config, directory, env))
            check(get(public, '/readyz')['status'] == 'ok', 'control readyz failed')
            with urllib.request.urlopen(public + '/healthz', timeout=2) as response:
                health = json.load(response)
                check(health['status'] == 'ok' and health['meeting_state'] == 3 and health['meeting_error'] == 0,
                    'control readiness did not report Running meeting module')
            nodes = {}
            for name in credentials:
                ws, udp, management = port(), port(socket.SOCK_DGRAM), port()
                node = {'node_id': name, 'listen_ip': '127.0.0.1', 'public_ip': '127.0.0.1',
                    'websocket_port': ws, 'udp_port': udp, 'control_url': internal,
                    'signaling_url': f'ws://127.0.0.1:{ws}/webrtc', 'max_rooms': 1, 'max_participants': 16,
                    'management_url': f'http://127.0.0.1:{management}', 'internal_listen_ip': '127.0.0.1',
                    'internal_http_port': management}
                child = Process(sfu, name, node, directory, dict(env, PACKETIA_NODE_SECRET=credentials[name]))
                processes.append(child)
                nodes[name] = (node, child)
                check(get(node['management_url'], '/healthz')['node_id'] == name, 'SFU liveness failed')
                check(get(node['management_url'], '/readyz')['status'] == 'ok', 'registered SFU not ready')

            def account(name):
                post(public, '/api/v1/accounts', {'login': name, 'password': 'correct-password-123', 'display_name': name}, expected=201)
                return post(public, '/api/v1/sessions', {'login': name, 'password': 'correct-password-123'})

            owner, bob = account('owner'), account('bob')
            owner_token, bob_token = owner['session_token'], bob['session_token']
            rooms = [post(public, '/api/v1/rooms', {}, owner_token, 201)['room_id'] for _ in range(3)]

            def admit(room, token=owner_token):
                return post(public, '/api/v1/room-admissions', {'room_id': room}, token)

            def manage(room, action, body=None, expected=200, token=owner_token):
                return post(public, '/api/v1/rooms/' + action, dict(body or {}, room_id=room), token, expected)

            def connect(admission, kind='join', **extra):
                ws = WebSocket(admission['signaling_url'])
                clients.append(ws)
                result = ws.request(dict(type=kind, room_id=admission['room_id'], token=admission['token'], **extra))
                return ws, result

            a, b = admit(rooms[0]), admit(rooms[1])
            check(a['node_id'] != b['node_id'], 'rooms did not spread across nodes')
            post(public, '/api/v1/room-admissions', {'room_id': rooms[2]}, owner_token, 503)
            post(public, '/api/v1/room-members', {'room_id': rooms[0], 'user_id': bob['identity']['user_id'], 'role': 'speaker'}, owner_token)
            bob_admission = admit(rooms[0], bob_token)
            member, joined = connect(bob_admission)
            check(joined['type'] == 'joined', 'member join failed')
            other, other_joined = connect(b)
            check(other_joined['type'] == 'joined', 'second node join failed')
            roster = manage(rooms[0], 'participants')
            check(roster['total'] == 1 and roster['participants'][0]['subject_id'] == bob['identity']['user_id'], 'owner backend list failed')
            participant = joined['participant_id']
            manage(rooms[0], 'participants', expected=403, token=bob_token)
            manage(rooms[0], 'participants/update', {'participant_id': participant, 'name': 'Changed',
                'metadata': 'meeting data', 'attributes': {'team': 'audio'}, 'permissions': {'can_subscribe': False}})
            check(manage(rooms[0], 'participant', {'participant_id': participant})['participant']['name'] == 'Changed', 'backend profile update missing')
            manage(rooms[0], 'participant', {'participant_id': 'missing'}, 404)
            role = post(public, '/api/v1/room-members', {'room_id': rooms[0], 'user_id': bob['identity']['user_id'], 'role': 'listener'}, owner_token)
            check(role['effective'] == 'next_join', 'persistent role update semantics missing')
            member.close()
            eventually(lambda: manage(rooms[0], 'participant', {'participant_id': participant})['participant']['state'] == 'reconnecting')
            refreshed = admit(rooms[0], bob_token)
            recovered, resumed = connect(refreshed, 'resume', participant_id=participant, resume_token=joined['resume_token'])
            check(resumed['type'] == 'resumed' and resumed['participant']['permissions']['can_publish'] and
                not resumed['participant']['permissions']['can_subscribe'], 'resume overwrote runtime permissions with new role')

            # 内部命令同时检查节点密钥、进程代次与房间代次。
            node_config = nodes[a['node_id']][0]
            command = dict(node_id=a['node_id'], instance_id=a['instance_id'], room_id=rooms[0],
                assignment_id=a['assignment_id'], command={'type': 'participants'})
            post(node_config['management_url'], '/internal/v1/rooms/command', command, 'incorrect', 401)
            post(node_config['management_url'], '/internal/v1/rooms/command', dict(command, instance_id='old-boot'), credentials[a['node_id']], 409)
            post(node_config['management_url'], '/internal/v1/rooms/command', dict(command, assignment_id='old-assignment'), credentials[a['node_id']], 409)

            manage(rooms[0], 'participants/remove', {'participant_id': participant})
            check(recovered.reply()['type'] == 'removed', 'kick notification missing')
            _, denied = connect(refreshed, 'resume', participant_id=participant, resume_token=joined['resume_token'])
            check(denied['type'] == 'error', 'kick left recovery credentials valid')
            # 旧代次等待节点确认回收后才能获得新代次。
            next_route = eventually(lambda: (lambda value: value if value['assignment_id'] != a['assignment_id'] else None)(admit(rooms[0], bob_token)))
            fresh, new_join = connect(next_route)
            check(new_join['type'] == 'joined' and not new_join['participant']['permissions']['can_publish'], 'new join ignored persistent role')
            released = post(internal, '/internal/v1/rooms/release', dict(node_id=a['node_id'], instance_id=a['instance_id'],
                room_id=rooms[0], assignment_id=a['assignment_id']), credentials[a['node_id']])
            check(not released['released'] and admit(rooms[0], bob_token)['assignment_id'] == next_route['assignment_id'], 'old release cleared new binding')
            _, old_join = connect(bob_admission)
            check(old_join.get('code') == 'stale_assignment', 'old JWT joined new epoch')
            fresh.request({'type': 'leave'})

            manage(rooms[1], 'end')
            check(other.reply()['type'] == 'removed', 'end did not notify second node')
            manage(rooms[1], 'end')
            post(public, '/api/v1/room-admissions', {'room_id': rooms[1]}, owner_token, 410)
            c = admit(rooms[2])
            check(c['node_id'] == b['node_id'], 'end did not free node capacity')
            # 控制面重启保留会议、用户会话与代次；SFU 不需要重建参与者。
            processes[0].stop()
            for node, _ in nodes.values():
                eventually(lambda node=node: get(node['management_url'], '/readyz', expected=None)['status'] == 'lease_expired', timeout=15)
                check(get(node['management_url'], '/healthz')['status'] == 'ok', 'lease expiry killed SFU liveness')
            processes[0] = Process(control, 'control-restarted', config, directory, env)
            for node, _ in nodes.values():
                eventually(lambda node=node: get(node['management_url'], '/readyz', expected=None)['status'] == 'ok')
            check(admit(rooms[2])['assignment_id'] == c['assignment_id'], 'control restart lost assignment')
            nodes[c['node_id']][1].stop()
            manage(rooms[2], 'participants', expected=503)
            print('PASS two-node meeting management, readiness, lease expiry/recovery, epochs and control restart')
        except Exception:
            for process in processes:
                print(process.log_path.name + ':\n' + process.log_path.read_text(errors='replace')[-2500:])
            raise
        finally:
            failed = sys.exc_info()[0] is not None
            cleanup_errors = []
            for client in clients:
                client.close()
            for process in reversed(processes):
                try:
                    process.stop()
                except Exception as error:
                    cleanup_errors.append(error)
            if cleanup_errors and not failed:
                raise cleanup_errors[0]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', required=True)
    parser.add_argument('--sfu', required=True)
    args = parser.parse_args()
    run(args.control, args.sfu)
