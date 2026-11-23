#!/usr/bin/env python3
"""Check a deployed public Control API and WSS using isolated test accounts/room.

Creates two accounts (there is no account deletion API), ends the test room and
revokes sessions on exit. Does not restart services or modify existing rooms.
Only Python's standard library is required. Credentials stay in memory.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import http.client
import json
from pathlib import Path
import secrets
import socket
import ssl
import time
from urllib.parse import urlsplit

from test_meeting_control import WebSocket


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def redact(value):
    private = {'password', 'token', 'session_token', 'resume_token', 'authorization'}
    if isinstance(value, dict):
        return {key: '[redacted]' if key.lower() in private else redact(item)
                for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


class SecureWebSocket(WebSocket):
    def __init__(self, url, context, origin):
        parsed = urlsplit(url)
        require(parsed.scheme == 'wss' and parsed.netloc == urlsplit(origin).netloc,
                'admission must return WSS on the tested public origin')
        raw = socket.create_connection((parsed.hostname, parsed.port or 443), timeout=6)
        try:
            self.sock = context.wrap_socket(raw, server_hostname=parsed.hostname)
        except BaseException:
            raw.close()
            raise
        try:
            key = base64.b64encode(secrets.token_bytes(16)).decode()
            path = parsed.path + ('?' + parsed.query if parsed.query else '')
            self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: {parsed.netloc}\r\n'
                f'Origin: {origin}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: packetia\r\n'
                f'Sec-WebSocket-Key: {key}\r\n\r\n').encode())
            response = b''
            while not response.endswith(b'\r\n\r\n'):
                response += self.read(1)
                require(len(response) < 16384, 'oversize upgrade response')
            lines = response.decode().split('\r\n')
            require(' 101 ' in lines[0], 'WebSocket upgrade failed: ' + lines[0])
            headers = {key.lower(): value.strip() for key, value in
                       (line.split(':', 1) for line in lines[1:] if ':' in line)}
            accept = base64.b64encode(hashlib.sha1(
                (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            require(headers.get('sec-websocket-accept') == accept,
                    'invalid WebSocket accept header')
            self.notifications = []
        except BaseException:
            self.sock.close()
            raise


class Probe:
    def __init__(self, origin, context, verified):
        self.origin = origin.rstrip('/')
        self.parsed = urlsplit(self.origin)
        require(self.parsed.scheme == 'https' and not self.parsed.path.strip('/'),
                '--base-url must be an HTTPS origin')
        self.context = context
        self.report = {'started_at': datetime.now(timezone.utc).isoformat(),
            'base_url': self.origin, 'tls_certificate_verified': verified,
            'cases': [], 'accounts_created': [], 'rooms_created': [],
            'limitations': ['未测试真实音视频、SDP/ICE/DTLS/SRTP、弱网或并发容量。',
                '仅部署 sfu-a；未启动的 sfu-b 不在测试范围。',
                '内部节点注册/心跳/回收不发送合法管理命令，只验证公网不可访问。',
                '无删除账号接口；随机测试账号记录保留，密码不落盘，会话在结束时撤销。']}
        self.sessions, self.rooms, self.sockets = [], [], []

    def record(self, name, target, elapsed, actual, expected, body=None, detail=''):
        passed = actual == expected
        self.report['cases'].append({'name': name, 'target': target,
            'latency_ms': round(elapsed * 1000, 2), 'actual': actual,
            'expected': expected, 'passed': passed, 'response': redact(body), 'detail': detail})
        print(f'{"PASS" if passed else "FAIL"} {name}: {actual}', flush=True)

    def http(self, name, method, path, expected=200, body=None, token='', raw=None,
             content_type='application/json'):
        started = time.monotonic()
        connection = http.client.HTTPSConnection(self.parsed.hostname,
            self.parsed.port or 443, context=self.context, timeout=8)
        headers = {'Accept': 'application/json'}
        payload = raw if raw is not None else json.dumps(body).encode() if body is not None else None
        if payload is not None:
            headers['Content-Type'] = content_type
        if token:
            headers['Authorization'] = 'Bearer ' + token
        try:
            connection.request(method, path, body=payload, headers=headers)
            response = connection.getresponse()
            data = response.read(2 * 1024 * 1024)
            try:
                value = json.loads(data) if data else None
            except ValueError:
                value = data.decode(errors='replace')[:500]
            self.record(name, method + ' ' + path, time.monotonic() - started,
                        response.status, expected, value)
            return value
        except Exception as error:
            self.record(name, method + ' ' + path, time.monotonic() - started,
                        type(error).__name__, expected, detail=str(error))
            raise
        finally:
            connection.close()

    def assertion(self, name, condition, detail=''):
        self.record(name, 'response semantics', 0, bool(condition), True, detail=detail)

    def connect(self, admission):
        client = SecureWebSocket(admission['signaling_url'], self.context, self.origin)
        self.sockets.append(client)
        return client

    def signal(self, name, client, body, expected):
        started = time.monotonic()
        try:
            value = client.request(body)
            self.record(name, 'WSS ' + body['type'], time.monotonic() - started,
                        value.get('type'), expected, value)
            return value
        except Exception as error:
            self.record(name, 'WSS ' + body['type'], time.monotonic() - started,
                        type(error).__name__, expected, detail=str(error))
            raise

    def run(self):
        self.http('Control health', 'GET', '/healthz')
        self.http('Anonymous metrics (currently public)', 'GET', '/api/v1/metrics')
        for path in ('/', '/readyz', '/metrics', '/api/v1/no-such-route'):
            self.http('Unknown/unpublished route ' + path, 'GET', path, 404)
        for path in ('/internal/v1/nodes/register', '/internal/v1/nodes/heartbeat',
                     '/internal/v1/rooms/release', '/internal/v1/rooms/command'):
            self.http('Internal route isolated ' + path, 'POST', path, 404, body={})
        post_routes = ('/api/v1/accounts', '/api/v1/sessions', '/api/v1/sessions/renew',
            '/api/v1/rooms', '/api/v1/room-members', '/api/v1/room-admissions',
            '/api/v1/rooms/participants', '/api/v1/rooms/participant',
            '/api/v1/rooms/participants/update', '/api/v1/rooms/participants/remove',
            '/api/v1/rooms/end')
        for path in post_routes:
            self.http('Reject wrong method ' + path, 'GET', path, 405)
        self.http('Me wrong method', 'POST', '/api/v1/me', 405)
        self.http('Logout wrong method', 'GET', '/api/v1/sessions/current', 405)
        for method, path in [('GET', '/api/v1/me'), ('POST', '/api/v1/sessions/renew'),
                             ('DELETE', '/api/v1/sessions/current')]:
            self.http('Missing session ' + path, method, path, 401)
            self.http('Invalid session ' + path, method, path, 401, token='invalid-token')
        for path in post_routes[3:]:
            self.http('Anonymous room operation ' + path, 'POST', path, 401, body={})
        for name, raw, kind, expected in [('Malformed JSON', b'{', 'application/json', 400),
                ('Array body', b'[]', 'application/json', 400),
                ('Duplicate JSON fields', b'{"login":"a","login":"b","password":"test"}', 'application/json', 400),
                ('Wrong content type', b'{}', 'text/plain', 415)]:
            self.http(name, 'POST', '/api/v1/sessions', expected, raw=raw, content_type=kind)
        self.http('Unknown account field', 'POST', '/api/v1/accounts', 400,
                  body={'login': 'unused', 'password': 'x' * 24, 'admin': True})
        self.http('Short password', 'POST', '/api/v1/accounts', 400,
                  body={'login': 'unused', 'password': 'short'})
        self.http('Renew must have empty body', 'POST', '/api/v1/sessions/renew', 400, body={})

        run_id = datetime.now(timezone.utc).strftime('%Y%m%d%H%M%S') + secrets.token_hex(3)
        identities = []
        for role in ('owner', 'member'):
            account = {'login': f'api-{run_id}-{role}', 'password': secrets.token_urlsafe(24),
                       'display_name': f'API test {role}'}
            registered = self.http('Register ' + role, 'POST', '/api/v1/accounts', 201, body=account)
            require(isinstance(registered, dict) and 'user' in registered,
                    'Cannot continue without independent test accounts; registration may be disabled')
            self.report['accounts_created'].append(registered['user'])
            credentials = {key: account[key] for key in ('login', 'password')}
            self.http('Duplicate registration ' + role, 'POST', '/api/v1/accounts', 409, body=account)
            self.http('Invalid password ' + role, 'POST', '/api/v1/sessions', 401,
                      body=dict(credentials, password=secrets.token_urlsafe(24)))
            login = self.http('Login ' + role, 'POST', '/api/v1/sessions', body=credentials)
            token = login['session_token']
            self.sessions.append(token)
            me = self.http('Current user ' + role, 'GET', '/api/v1/me', token=token)
            self.assertion('Identity agrees ' + role, me['user_id'] == registered['user']['id'])
            renewed = self.http('Rotate session ' + role, 'POST', '/api/v1/sessions/renew', token=token)
            self.sessions[-1] = renewed['session_token']
            self.http('Old token revoked ' + role, 'GET', '/api/v1/me', 401, token=token)
            token = renewed['session_token']
            self.http('Renewed token works ' + role, 'GET', '/api/v1/me', token=token)
            identities.append((registered['user']['id'], token))
        (owner_id, owner), (member_id, member) = identities
        room = self.http('Create room', 'POST', '/api/v1/rooms', 201, body={}, token=owner)['room_id']
        self.rooms.append((room, owner))
        self.report['rooms_created'].append(room)

        def manage(name, suffix, extra=None, expected=200, token=owner):
            return self.http(name, 'POST', '/api/v1/rooms/' + suffix, expected,
                             body=dict({'room_id': room}, **(extra or {})), token=token)

        def admit(token=owner, expected=200):
            return self.http('Room admission', 'POST', '/api/v1/room-admissions', expected,
                             body={'room_id': room}, token=token)

        def role(value, token=owner, expected=200):
            return self.http('Member role ' + value, 'POST', '/api/v1/room-members', expected,
                body={'room_id': room, 'user_id': member_id, 'role': value}, token=token)

        self.http('Reject room creation unknown field', 'POST', '/api/v1/rooms', 400,
                  body={'owner': owner_id}, token=owner)
        admit(member, 403)
        role('administrator', expected=400)
        role('speaker', token=member, expected=403)
        roster = manage('Empty room roster', 'participants')
        self.assertion('Room initially empty', roster.get('total') == 0)
        role('speaker')
        owner_admission, member_admission = admit(), admit(member)
        self.assertion('Public route assignment', owner_admission['node_id'] == 'sfu-a' and
                       owner_admission['assignment_id'] == member_admission['assignment_id'],
                       owner_admission['signaling_url'])
        unauth = self.connect(owner_admission)
        self.signal('WSS requires join', unauth, {'type': 'tracks'}, 'error')
        self.signal('WSS rejects invalid admission JWT', unauth,
                    {'type': 'join', 'room_id': room, 'token': 'invalid'}, 'error')
        unauth.close()
        owner_ws = self.connect(owner_admission)
        joined_owner = self.signal('WSS owner joins', owner_ws,
            {'type': 'join', 'room_id': room, 'token': owner_admission['token']}, 'joined')
        member_ws = self.connect(member_admission)
        joined = self.signal('WSS speaker joins', member_ws,
            {'type': 'join', 'room_id': room, 'token': member_admission['token']}, 'joined')
        participant = joined['participant_id']
        roster = manage('List active participants', 'participants')
        self.assertion('Two active participants', roster.get('total') == 2)
        page = manage('Paginated roster page 1', 'participants', {'limit': 1})
        continuation = manage('Paginated roster page 2', 'participants',
                              {'limit': 1, 'cursor': page['next_cursor'], 'revision': page['revision']})
        self.assertion('Pagination has distinct participants', len(page['participants']) == 1 and
            len(continuation['participants']) == 1 and
            page['participants'][0]['participant_id'] != continuation['participants'][0]['participant_id'])
        manage('Reject invalid page limit', 'participants', {'limit': 0}, 400)
        manage('Reject stale page revision', 'participants', {'revision': page['revision'] + 100}, 400)
        manage('Missing participant', 'participant', {'participant_id': 'missing'}, 404)
        for suffix, extra in [('participants', {}), ('participant', {'participant_id': participant}),
                ('participants/update', {'participant_id': participant, 'name': 'unauthorized'}),
                ('participants/remove', {'participant_id': joined_owner['participant_id']}), ('end', {})]:
            manage('Non-owner denied ' + suffix, suffix, extra, 403, member)
        self.signal('WSS participant roster', owner_ws, {'type': 'participants'}, 'participants')
        self.signal('WSS participant detail', owner_ws,
                    {'type': 'participant', 'participant_id': participant}, 'participant')
        self.signal('WSS track list', owner_ws, {'type': 'tracks'}, 'tracks')
        self.signal('WSS member cannot kick owner', member_ws,
                    {'type': 'remove_participant', 'participant_id': joined_owner['participant_id']}, 'error')
        manage('Update profile and permissions', 'participants/update',
            {'participant_id': participant, 'name': 'API Updated', 'metadata': 'isolated API test',
             'attributes': {'purpose': 'api-test'}, 'permissions': {'can_subscribe': False}})
        detail = manage('Read back participant update', 'participant', {'participant_id': participant})
        self.assertion('Profile and permissions persisted', detail['participant']['name'] == 'API Updated' and
                       not detail['participant']['permissions']['can_subscribe'])
        role('listener')
        member_ws.close()
        deadline = time.monotonic() + 6
        while True:
            detail = manage('Observe disconnected participant', 'participant', {'participant_id': participant})
            if detail['participant']['state'] == 'reconnecting' or time.monotonic() >= deadline:
                break
            time.sleep(.2)
        fresh = admit(member)
        recovered = self.connect(fresh)
        resumed = self.signal('WSS resume after disconnect', recovered,
            {'type': 'resume', 'room_id': room, 'token': fresh['token'],
             'participant_id': participant, 'resume_token': joined['resume_token']}, 'resumed')
        self.assertion('Resume preserves identity/runtime permissions', resumed.get('participant_id') == participant and
            resumed['participant']['permissions']['can_publish'] and
            not resumed['participant']['permissions']['can_subscribe'])
        manage('Kick participant', 'participants/remove', {'participant_id': participant})
        notice = recovered.reply()
        self.assertion('Kick notifies participant', notice.get('type') == 'removed')
        rejected = self.connect(fresh)
        self.signal('Kicked participant cannot resume', rejected,
            {'type': 'resume', 'room_id': room, 'token': fresh['token'],
             'participant_id': participant, 'resume_token': joined['resume_token']}, 'error')
        rejected.close()
        fresh = admit(member)
        listener = self.connect(fresh)
        new_join = self.signal('WSS listener fresh join', listener,
            {'type': 'join', 'room_id': room, 'token': fresh['token']}, 'joined')
        self.assertion('Stored role applies on next join', not new_join['participant']['permissions']['can_publish'])
        denial = self.signal('Listener cannot publish', listener, {'type': 'publish', 'sdp': 'invalid'}, 'error')
        self.assertion('Publish denied by permission', denial.get('code') == 'forbidden')
        self.signal('WSS leave', listener, {'type': 'leave'}, 'closed')
        manage('End room', 'end')
        self.assertion('Room end notifies owner', owner_ws.reply().get('type') == 'removed')
        manage('End room idempotent', 'end')
        admit(owner, 410)
        admit(member, 410)
        self.http('Control healthy after test', 'GET', '/healthz')

    def cleanup(self):
        for room, owner in self.rooms:
            try:
                self.http('Cleanup end test room', 'POST', '/api/v1/rooms/end',
                          body={'room_id': room}, token=owner)
            except Exception:
                pass  # The failed request is already recorded.
        for client in self.sockets:
            client.close()
        for token in self.sessions:
            try:
                self.http('Cleanup revoke session', 'DELETE', '/api/v1/sessions/current', 204, token=token)
                self.http('Revoked session rejected', 'GET', '/api/v1/me', 401, token=token)
            except Exception:
                pass

    def write(self, directory):
        self.report.setdefault('finished_at', datetime.now(timezone.utc).isoformat())
        cases = self.report['cases']
        self.report['counts'] = {'passed': sum(case['passed'] for case in cases),
                                 'failed': sum(not case['passed'] for case in cases)}
        http_cases = [case for case in cases if case['target'].startswith(('GET ', 'POST ', 'DELETE '))]
        ws_cases = [case for case in cases if case['target'].startswith('WSS ')]
        covered = sorted({case['target'] for case in http_cases if isinstance(case['actual'], int)
                          and 200 <= case['actual'] < 300 and case['passed']})
        self.report['public_http_success_routes'] = covered
        self.report['request_counts'] = {'http': len(http_cases), 'wss_messages': len(ws_cases),
                                        'semantic_assertions': len(cases) - len(http_cases) - len(ws_cases)}
        latencies = sorted(case['latency_ms'] for case in http_cases)
        if latencies:
            self.report['http_request_latency_ms'] = {
                'minimum': latencies[0], 'median': latencies[len(latencies) // 2],
                'p95': latencies[int((len(latencies) - 1) * .95)], 'maximum': latencies[-1]}
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'results.json').write_text(json.dumps(self.report, indent=2, ensure_ascii=False) + '\n')
        counts = self.report['counts']
        lines = ['# 公网 API 实测报告', '', f'- 地址：{self.origin}',
            f'- 开始：{self.report["started_at"]}', f'- 结束：{self.report["finished_at"]}',
            f'- 通过：{counts["passed"]}；失败：{counts["failed"]}',
            f'- HTTP 请求：{len(http_cases)}；WSS 请求：{len(ws_cases)}；响应语义检查：{len(cases) - len(http_cases) - len(ws_cases)}',
            f'- TLS 证书校验：{"已启用" if self.report["tls_certificate_verified"] else "未启用（等同 curl -k）；不代表证书受信任"}', '',
            '## 结论', '',
            '逐项结果覆盖账号/会话、房间创建与成员授权、入房许可、参与者查询/修改/移除和结束房间。',
            'WSS 检查覆盖入会、列表、权限拒绝、断线恢复、踢出后的恢复拒绝和离开。',
            '清理结果见末尾用例：仅结束本次创建的房间，并撤销本次会话、验证旧会话失效。', '',
            '当前注册接口开放，`/api/v1/metrics` 也允许匿名读取；受保护业务接口会拒绝匿名操作。', '',
            f'成功调用的公开 HTTP 方法与路径：{len(covered)} 个。', '']
        lines.extend('- `' + route + '`' for route in covered)
        if latencies:
            latency = self.report['http_request_latency_ms']
            lines.extend(['', f'HTTP 请求中位数 {latency["median"]} ms，P95 {latency["p95"]} ms。',
                '每个 HTTP 请求单独建立连接，耗时包含 TCP/TLS 握手、跨网传输及业务处理；不是媒体 RTT，也不是服务端处理时延。'])
        lines.extend(['', '## 范围与限制', ''])
        lines.extend('- ' + limitation for limitation in self.report['limitations'])
        lines.extend(['', '## 测试资源', '', '已创建的随机账号（不保存密码）：'])
        lines.extend(f'- `{user["login"]}` / `{user["id"]}`' for user in self.report['accounts_created'])
        lines.extend(['', '测试房间：' + ', '.join(self.report['rooms_created']), '',
                      '## 逐项结果', '', '| 用例 | 请求 | 实际 / 预期 | 耗时 ms | 结果 |',
                      '|---|---|---|---:|---|'])
        for case in cases:
            lines.append(f'| {case["name"]} | {case["target"]} | {case["actual"]} / {case["expected"]} | '
                         f'{case["latency_ms"]} | {"PASS" if case["passed"] else "FAIL"} |')
        if self.report.get('error'):
            lines.extend(['', '## 中断原因', '', self.report['error']])
        (directory / 'REPORT.md').write_text('\n'.join(lines) + '\n')
        print(json.dumps({'report': str(directory / 'REPORT.md'), **counts}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', required=True)
    parser.add_argument('--ca', help='CA certificate file; otherwise TLS verification is disabled')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    directory = args.output or Path(__file__).resolve().parents[1] / 'examples/webrtc-room/artifacts' / (
        'public-api-' + datetime.now(timezone.utc).strftime('%Y-%m-%dT%H-%M-%SZ'))
    context = ssl.create_default_context(cafile=args.ca) if args.ca else ssl._create_unverified_context()
    probe = Probe(args.base_url, context, bool(args.ca))
    try:
        probe.run()
    except Exception as error:
        probe.report['error'] = type(error).__name__ + ': ' + str(error)
        print(probe.report['error'], flush=True)
    finally:
        probe.cleanup()
        probe.write(directory)
    return 1 if probe.report.get('error') or probe.report['counts']['failed'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
