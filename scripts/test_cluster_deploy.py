#!/usr/bin/env python3
"""Test deployment configuration and command construction without Docker."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import stat
import subprocess
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('packetia_deploy', ROOT / 'deploy/cluster/deploy.py')
deploy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deploy)
LAUNCHER_SPEC = importlib.util.spec_from_file_location('packetia_launcher', ROOT / 'scripts/run_cluster_local.py')
launcher = importlib.util.module_from_spec(LAUNCHER_SPEC)
LAUNCHER_SPEC.loader.exec_module(launcher)


class ClusterDeploymentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='packetia-deploy-test-')
        self.addCleanup(self.temporary.cleanup)
        self.runtime = Path(self.temporary.name) / 'runtime'

    def config(self, name):
        return json.loads((self.runtime / f'{name}.json').read_text())

    def test_public_configuration_matches_proxy_routes_and_private_management(self):
        values = deploy.initialize(self.runtime, '203.0.113.10', 'Rtc.Example.com')
        self.assertEqual(values['PACKETIA_DOMAIN'], 'rtc.example.com')
        self.assertEqual(values['PACKETIA_LOG_FORMAT'], 'json')
        self.assertEqual(values['PACKETIA_LOG_LEVEL'], 'info')
        self.assertFalse(self.config('control')['registration_enabled'])
        for index, name in enumerate(('sfu-a', 'sfu-b')):
            node = self.config(name)
            self.assertEqual(node['signaling_url'], f'wss://rtc.example.com/{name}/webrtc')
            self.assertEqual(node['public_ip'], '203.0.113.10')
            self.assertEqual(node['management_url'], f'http://{name}:{19080 + index}')
            self.assertEqual(node['internal_http_port'], 19080 + index)
            self.assertEqual(node['udp_port'], 19100 + index)
        self.assertEqual(len({values[key] for key in deploy.SECRET_KEYS}), 3)
        self.assertEqual(stat.S_IMODE((self.runtime / '.env').stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((self.runtime / 'sfu-a.json').stat().st_mode), 0o644)

    def test_reinitialization_preserves_credentials_domain_registration_and_custom_build_jobs(self):
        original = deploy.initialize(self.runtime, registration=True)
        with (self.runtime / '.env').open('a') as output:
            output.write("PACKETIA_BUILD_JOBS='4'\n")
            output.write("PACKETIA_LOG_LEVEL='error'\n")
        public = deploy.initialize(self.runtime, '203.0.113.10', 'rtc.example.com')
        again = deploy.initialize(self.runtime)
        for key in deploy.SECRET_KEYS:
            self.assertEqual(original[key], public[key])
            self.assertEqual(original[key], again[key])
        self.assertEqual(again['PACKETIA_DOMAIN'], 'rtc.example.com')
        self.assertEqual(again['PACKETIA_BUILD_JOBS'], '4')
        self.assertEqual(again['PACKETIA_LOG_LEVEL'], 'error')
        self.assertTrue(self.config('control')['registration_enabled'])
        deploy.initialize(self.runtime, registration=False)
        self.assertFalse(self.config('control')['registration_enabled'])

    def test_local_configuration_uses_direct_loopback_signaling(self):
        values = deploy.initialize(self.runtime)
        self.assertEqual(values['PACKETIA_PUBLIC_IP'], '127.0.0.1')
        self.assertEqual(self.config('sfu-a')['signaling_url'], 'ws://127.0.0.1:18180/webrtc')
        self.assertEqual(deploy.compose_command(self.runtime, values).count('-f'), 1)
        public = deploy.initialize(self.runtime, '203.0.113.10', 'rtc.example.com')
        self.assertEqual(deploy.compose_command(self.runtime, public).count('-f'), 2)

    def test_public_ip_uses_https_proxy_and_test_ca(self):
        values = deploy.initialize(self.runtime, '203.0.113.10', '')
        self.assertEqual(values['PACKETIA_PUBLIC_HOST'], '203.0.113.10')
        self.assertEqual(values['PACKETIA_TLS_MODE'], 'internal')
        self.assertEqual(values['PACKETIA_TLS_CONFIG'], 'tls internal')
        self.assertEqual(deploy.compose_command(self.runtime, values).count('-f'), 2)
        for name in ('sfu-a', 'sfu-b'):
            self.assertEqual(self.config(name)['signaling_url'], f'wss://203.0.113.10/{name}/webrtc')

    def test_switching_ip_and_domain_updates_tls_without_rotating_credentials(self):
        original = deploy.initialize(self.runtime, '203.0.113.10', '')
        public = deploy.initialize(self.runtime, domain='rtc.example.com')
        self.assertEqual(public['PACKETIA_TLS_MODE'], 'auto')
        self.assertEqual(public['PACKETIA_TLS_CONFIG'], '')
        internal = deploy.initialize(self.runtime, tls_mode='internal')
        self.assertEqual(deploy.initialize(self.runtime)['PACKETIA_TLS_MODE'], 'internal')
        self.assertEqual(internal['PACKETIA_PUBLIC_HOST'], 'rtc.example.com')
        again = deploy.initialize(self.runtime, domain='')
        self.assertEqual(again['PACKETIA_TLS_MODE'], 'internal')
        for key in deploy.SECRET_KEYS:
            self.assertEqual(original[key], again[key])

    def test_invalid_certificate_modes_do_not_create_configuration(self):
        for mode in ('auto', 'tls internal\nrespond broken'):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                deploy.initialize(self.runtime, '203.0.113.10', '', tls_mode=mode)
        self.assertFalse(self.runtime.exists())

    def test_custom_https_port_updates_both_signaling_routes_and_survives_reinitialization(self):
        original = deploy.initialize(self.runtime, '203.0.113.10', '')
        values = deploy.initialize(self.runtime, https_port=8443)
        self.assertEqual(values['PACKETIA_HTTPS_PORT'], '8443')
        self.assertEqual(deploy.public_origin(values), 'https://203.0.113.10:8443')
        for name in ('sfu-a', 'sfu-b'):
            self.assertEqual(self.config(name)['signaling_url'], f'wss://203.0.113.10:8443/{name}/webrtc')
        again = deploy.initialize(self.runtime)
        self.assertEqual(again['PACKETIA_HTTPS_PORT'], '8443')
        for key in deploy.SECRET_KEYS:
            self.assertEqual(original[key], again[key])
        domain = deploy.initialize(self.runtime, domain='rtc.example.com')
        self.assertEqual(deploy.public_origin(domain), 'https://rtc.example.com:8443')
        default = deploy.initialize(self.runtime, https_port=443)
        self.assertEqual(deploy.public_origin(default), 'https://rtc.example.com')
        self.assertEqual(self.config('sfu-a')['signaling_url'], 'wss://rtc.example.com/sfu-a/webrtc')

    def test_invalid_https_ports_do_not_create_configuration(self):
        for port in (0, -1, 65536, 80, 'not-a-port'):
            with self.subTest(port=port), self.assertRaises(ValueError):
                deploy.initialize(self.runtime, '203.0.113.10', '', https_port=port)
        self.assertFalse(self.runtime.exists())

    def test_invalid_addresses_and_domains_do_not_create_configuration(self):
        for address, domain in [('0.0.0.0', ''), ('224.0.0.1', ''), ('169.254.1.1', ''),
                                ('127.0.0.1', 'rtc.example.com'), ('203.0.113.10', 'https://rtc.example.com'),
                                ('203.0.113.10', 'rtc.example.com/path'), ('203.0.113.10', 'x\n.example.com'),
                                ('203.0.113.10', '203.0.113.10')]:
            with self.subTest(address=address, domain=domain), self.assertRaises(ValueError):
                deploy.initialize(self.runtime, address, domain)
        self.assertFalse(self.runtime.exists())

    def test_existing_invalid_or_shared_credentials_are_rejected(self):
        deploy.initialize(self.runtime)
        path = self.runtime / '.env'
        original = path.read_text()
        for suffix in ["PACKETIA_JWT_SECRET='short'\n",
                       "PACKETIA_JWT_SECRET='" + 'x' * 64 + "'\nPACKETIA_NODE_A_SECRET='" + 'x' * 64 + "'\n"]:
            path.write_text(original + suffix)
            with self.assertRaises(ValueError):
                deploy.initialize(self.runtime)

    def test_runtime_path_with_spaces_and_quotes_round_trips(self):
        self.runtime = Path(self.temporary.name) / "runtime's $literal path"
        values = deploy.initialize(self.runtime)
        self.assertEqual(deploy.read_environment(self.runtime / '.env'), values)

    def test_launcher_understands_structured_and_legacy_readiness(self):
        for line in (b'PACKETIA_READY\n', b'[INFO] [event=process_ready] PACKETIA_READY\n',
                     b'{"event":"process_ready","service":"sfu","message":"PACKETIA_READY"}\n'):
            self.assertTrue(launcher.is_ready_line(line))
        for line in (b'not PACKETIA_READY\n', b'{bad json}', b'{}', b'[]',
                     b'{"message":"PACKETIA_READY"}', b'{"event":"process_failed"}'):
            self.assertFalse(launcher.is_ready_line(line))

    @mock.patch.object(deploy.shutil, 'which', return_value='/mock/docker')
    @mock.patch.object(deploy.subprocess, 'run')
    def test_up_builds_shared_image_before_starting_and_uses_generated_credentials(self, run, _which):
        values = deploy.initialize(self.runtime, '203.0.113.10', 'rtc.example.com')
        with mock.patch.dict(os.environ, {'PACKETIA_JWT_SECRET': 'unrelated-shell-key'}):
            deploy.operate(self.runtime, argparse.Namespace(action='up', build=True, recreate=True, timeout=90,
                                                          single_sfu=False))
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0][-2:], ['config', '--quiet'])
        self.assertEqual(commands[1][-2:], ['build', 'control'])
        self.assertEqual(commands[2][-7:], ['up', '-d', '--no-build', '--wait', '--wait-timeout', '90', '--force-recreate'])
        self.assertEqual(commands[3][-1], 'ps')
        for call in run.call_args_list:
            self.assertFalse(call.kwargs.get('shell', False))
            self.assertEqual(call.kwargs['env']['PACKETIA_JWT_SECRET'], values['PACKETIA_JWT_SECRET'])

    @mock.patch.object(deploy.shutil, 'which', return_value='/mock/docker')
    @mock.patch.object(deploy.subprocess, 'run')
    def test_single_sfu_public_start_reuses_image_and_does_not_start_sfu_b(self, run, _which):
        deploy.initialize(self.runtime, '203.0.113.10', '')
        deploy.operate(self.runtime, argparse.Namespace(action='up', build=False, recreate=True, timeout=120,
                                                      single_sfu=True))
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(len(commands), 3)
        self.assertIn('compose.public.yaml', ' '.join(commands[1]))
        self.assertEqual(commands[1][-3:], ['control', 'sfu-a', 'edge'])
        self.assertIn('--no-build', commands[1])
        self.assertNotIn('sfu-b', commands[1])

    @mock.patch.object(deploy.shutil, 'which', return_value='/mock/docker')
    @mock.patch.object(deploy.subprocess, 'run')
    def test_single_sfu_check_does_not_require_sfu_b(self, run, _which):
        deploy.initialize(self.runtime, '203.0.113.10', '')
        deploy.operate(self.runtime, argparse.Namespace(action='check', single_sfu=True))
        self.assertEqual(len(run.call_args_list), 3)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertTrue(any('http://127.0.0.1:19080/readyz' in command for command in commands))
        self.assertFalse(any('sfu-b' in command for command in commands))

    @mock.patch.object(deploy.shutil, 'which', return_value='/mock/docker')
    @mock.patch.object(deploy.subprocess, 'run')
    def test_down_preserves_volumes_and_does_not_require_public_readiness(self, run, _which):
        deploy.initialize(self.runtime, '203.0.113.10')
        deploy.operate(self.runtime, argparse.Namespace(action='down'))
        command = run.call_args.args[0]
        self.assertEqual(command[-1], 'down')
        self.assertNotIn('--volumes', command)
        self.assertNotIn('-v', command)


CADDY_BINARY = os.environ.get('PACKETIA_CADDY_BINARY') or shutil.which('caddy')


@unittest.skipUnless(CADDY_BINARY, 'set PACKETIA_CADDY_BINARY or install caddy for the IP TLS regression')
class PublicIpTlsTest(unittest.TestCase):
    def test_no_sni_client_behind_port_mapping_receives_the_public_ip_certificate(self):
        # The public address differs from the listening address, as with Docker
        # NAT. Testing a 127.0.0.1 certificate on loopback hides this failure.
        public_ip = '203.0.113.10'
        with tempfile.TemporaryDirectory(prefix='packetia-caddy-ip-test-') as temporary:
            directory = Path(temporary)
            with socket.socket() as tls_listener, socket.socket() as http_listener:
                tls_listener.bind(('127.0.0.1', 0))
                http_listener.bind(('127.0.0.1', 0))
                tls_port = tls_listener.getsockname()[1]
                http_port = http_listener.getsockname()[1]
            source = (ROOT / 'deploy/cluster/Caddyfile').read_text()
            source = source.replace('skip_install_trust',
                f'skip_install_trust\n\tadmin off\n\thttp_port {http_port}\n\thttps_port {tls_port}'
                '\n\tdefault_bind 127.0.0.1')
            config = directory / 'Caddyfile'
            config.write_text(source)
            environment = dict(os.environ, PACKETIA_PUBLIC_HOST=public_ip, PACKETIA_HTTPS_PORT='8443',
                PACKETIA_TLS_CONFIG='tls internal', XDG_DATA_HOME=str(directory / 'data'),
                XDG_CONFIG_HOME=str(directory / 'config'))
            with (directory / 'caddy.log').open('w') as log:
                process = subprocess.Popen([CADDY_BINARY, 'run', '--config', str(config), '--adapter', 'caddyfile'],
                    env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
                try:
                    ca = directory / 'data/caddy/pki/authorities/local/root.crt'

                    def request():
                        context = ssl.create_default_context(cafile=str(ca))
                        context.check_hostname = False
                        with socket.create_connection(('127.0.0.1', tls_port), timeout=1) as connection:
                            with context.wrap_socket(connection, server_hostname=None) as tls:
                                certificate = tls.getpeercert()
                                tls.sendall(f'GET / HTTP/1.1\r\nHost: {public_ip}:8443\r\nConnection: close\r\n\r\n'.encode())
                                response = tls.recv(4096)
                        return certificate, response

                    deadline = time.monotonic() + 15
                    while True:
                        if process.poll() is not None:
                            self.fail((directory / 'caddy.log').read_text())
                        # Wait for the CA and managed leaf certificate to exist
                        # before testing certificate selection without SNI.
                        certificates = directory / 'data/caddy/certificates'
                        if ca.is_file() and any(certificates.rglob(f'{public_ip}.crt')):
                            break
                        if time.monotonic() >= deadline:
                            self.fail((directory / 'caddy.log').read_text())
                        time.sleep(0.05)
                    certificate, response = request()
                    self.assertIn(('IP Address', public_ip), certificate['subjectAltName'])
                    self.assertTrue(response.startswith(b'HTTP/1.1 404'), response)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    unittest.main()
