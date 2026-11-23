#!/usr/bin/env python3
"""Prepare direct public-IP HTTPS testing for an existing single-SFU deployment."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile


def write_json(path, value, mode=0o600):
    path.write_text(json.dumps(value, indent=2) + '\n')
    path.chmod(mode)


def openssl(*arguments):
    subprocess.run(['openssl', *map(str, arguments)], check=True, capture_output=True)


def prepare(runtime, public_ip, port):
    public_ip = str(ipaddress.IPv4Address(public_ip))
    if not 1 <= port <= 65535:
        raise ValueError('HTTPS port must be from 1 to 65535')
    control_path, sfu_path = runtime / 'control.json', runtime / 'sfu-a.json'
    control = json.loads(control_path.read_text())
    sfu = json.loads(sfu_path.read_text())
    if sfu.get('websocket_port') != 18180:
        raise ValueError('This test entry expects SFU A websocket_port 18180')
    if not shutil.which('openssl'):
        raise ValueError('Install openssl first: apt-get install openssl')
    directory = runtime / 'browser-test'
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory.chmod(0o700)
    ca_key, ca_cert = directory / 'ca.key', directory / 'ca.crt'
    if ca_key.exists() != ca_cert.exists():
        raise ValueError('Both browser-test/ca.key and ca.crt must exist together')
    origin = f'https://{public_ip}:{port}'
    config_path = directory / 'config.json'
    password = json.loads(config_path.read_text())['password'] if config_path.exists() else secrets.token_urlsafe(24)
    if not isinstance(password, str) or len(password) < 20:
        raise ValueError('Existing test password is invalid')
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        temporary = Path(temporary)
        if not ca_key.exists():
            openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3650',
                    '-keyout', temporary / 'ca.key', '-out', temporary / 'ca.crt',
                    '-subj', '/CN=Packetia Browser Test CA',
                    '-addext', 'basicConstraints=critical,CA:TRUE',
                    '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
            for name in ('ca.key', 'ca.crt'):
                os.replace(temporary / name, directory / name)
        extensions = temporary / 'extensions.cnf'
        extensions.write_text(f'subjectAltName=IP:{public_ip}\n'
                              'basicConstraints=critical,CA:FALSE\n'
                              'keyUsage=critical,digitalSignature,keyEncipherment\n'
                              'extendedKeyUsage=serverAuth\n')
        openssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', temporary / 'server.key',
                '-out', temporary / 'server.csr', '-subj', f'/CN={public_ip}')
        openssl('x509', '-req', '-in', temporary / 'server.csr', '-CA', ca_cert, '-CAkey', ca_key,
                '-set_serial', str(secrets.randbits(128) or 1), '-days', '30',
                '-extfile', extensions, '-out', temporary / 'server.crt')
        for name in ('server.key', 'server.crt'):
            os.replace(temporary / name, directory / name)
    for name in ('ca.key', 'server.key', 'server.crt'):
        (directory / name).chmod(0o600)
    ca_cert.chmod(0o644)
    write_json(config_path, {'origin': origin, 'password': password})
    control['registration_enabled'] = True
    sfu['public_ip'] = public_ip
    sfu['signaling_url'] = origin.replace('https:', 'wss:') + '/webrtc'
    # The prebuilt backend runs as UID 10001; these JSON files contain no keys.
    write_json(control_path, control, 0o644)
    write_json(sfu_path, sfu, 0o644)
    return origin, ca_cert


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-dir', type=Path, default=Path(__file__).resolve().parent / 'runtime')
    parser.add_argument('--public-ip')
    parser.add_argument('--port', type=int, default=18443)
    parser.add_argument('--show-password', action='store_true')
    args = parser.parse_args()
    try:
        if args.show_password:
            print(json.loads((args.runtime_dir / 'browser-test/config.json').read_text())['password'])
            return
        if not args.public_ip:
            parser.error('--public-ip is required when preparing the test entry')
        origin, certificate = prepare(args.runtime_dir, args.public_ip, args.port)
        print(f'Public test page: {origin}')
        print(f'Import this CA certificate into the test computer: {certificate}')
        print('Read the test login password with: python3 deploy/cluster/prepare_browser_test.py --show-password')
        print('Configuration updated; recreate control, sfu-a and browser-test with compose.browser-test.yaml.')
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Public browser test setup failed: {error}\n')


if __name__ == '__main__':
    main()
