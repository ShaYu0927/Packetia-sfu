#!/usr/bin/env python3
"""Test deployment configuration and command construction without Docker."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('packetia_deploy', ROOT / 'deploy/cluster/deploy.py')
deploy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deploy)


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
        public = deploy.initialize(self.runtime, '203.0.113.10', 'rtc.example.com')
        again = deploy.initialize(self.runtime)
        for key in deploy.SECRET_KEYS:
            self.assertEqual(original[key], public[key])
            self.assertEqual(original[key], again[key])
        self.assertEqual(again['PACKETIA_DOMAIN'], 'rtc.example.com')
        self.assertEqual(again['PACKETIA_BUILD_JOBS'], '4')
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

    @mock.patch.object(deploy.shutil, 'which', return_value='/mock/docker')
    @mock.patch.object(deploy.subprocess, 'run')
    def test_up_builds_shared_image_before_starting_and_uses_generated_credentials(self, run, _which):
        values = deploy.initialize(self.runtime, '203.0.113.10', 'rtc.example.com')
        with mock.patch.dict(os.environ, {'PACKETIA_JWT_SECRET': 'unrelated-shell-key'}):
            deploy.operate(self.runtime, argparse.Namespace(action='up', build=True, recreate=True, timeout=90))
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0][-2:], ['config', '--quiet'])
        self.assertEqual(commands[1][-2:], ['build', 'control'])
        self.assertEqual(commands[2][-6:], ['up', '-d', '--wait', '--wait-timeout', '90', '--force-recreate'])
        self.assertEqual(commands[3][-1], 'ps')
        for call in run.call_args_list:
            self.assertFalse(call.kwargs.get('shell', False))
            self.assertEqual(call.kwargs['env']['PACKETIA_JWT_SECRET'], values['PACKETIA_JWT_SECRET'])

    @mock.patch.object(deploy.shutil, 'which', return_value='/mock/docker')
    @mock.patch.object(deploy.subprocess, 'run')
    def test_down_preserves_volumes_and_does_not_require_public_readiness(self, run, _which):
        deploy.initialize(self.runtime, '203.0.113.10')
        deploy.operate(self.runtime, argparse.Namespace(action='down'))
        command = run.call_args.args[0]
        self.assertEqual(command[-1], 'down')
        self.assertNotIn('--volumes', command)
        self.assertNotIn('-v', command)


if __name__ == '__main__':
    unittest.main()
