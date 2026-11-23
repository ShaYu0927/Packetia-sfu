#!/usr/bin/env python3
"""Configure and operate a single-host Packetia Control + two-SFU Docker stack."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile


DEPLOY_DIR = Path(__file__).resolve().parent
SERVICES = ('control', 'sfu-a', 'sfu-b', 'edge')
SECRET_KEYS = ('PACKETIA_JWT_SECRET', 'PACKETIA_NODE_A_SECRET', 'PACKETIA_NODE_B_SECRET')


def read_environment(path):
    result = {}
    if not path.exists():
        return result
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        if not separator or not re.fullmatch(r'[A-Z][A-Z0-9_]*', key):
            raise ValueError('invalid environment file; expected KEY=value lines')
        if value.startswith("'") and value.endswith("'"):
            value = value[1:-1].replace("\\'", "'")
        result[key] = value
    return result


def write_file(path, text, mode):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            os.fchmod(output.fileno(), mode)
            output.write(text)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def public_host(values):
    if values.get('PACKETIA_DOMAIN'):
        return values['PACKETIA_DOMAIN']
    address = values.get('PACKETIA_PUBLIC_IP', '127.0.0.1')
    return '' if ipaddress.IPv4Address(address).is_loopback else address


def public_origin(values):
    host = public_host(values)
    if not host:
        return ''
    port = int(values.get('PACKETIA_HTTPS_PORT', '443'))
    return f'https://{host}' + (f':{port}' if port != 443 else '')


def initialize(runtime, public_ip=None, domain=None, registration=None, tls_mode=None, https_port=None):
    runtime = runtime.resolve()
    values = read_environment(runtime / '.env')
    public_ip = public_ip if public_ip is not None else values.get('PACKETIA_PUBLIC_IP', '127.0.0.1')
    address = ipaddress.IPv4Address(public_ip)
    if address.is_unspecified or address.is_multicast or address.is_link_local:
        raise ValueError('--public-ip must be a reachable IPv4 address')
    previous_domain = values.get('PACKETIA_DOMAIN', '')
    domain = (domain if domain is not None else previous_domain).lower()
    if domain:
        labels = domain.split('.')
        if (len(domain) > 253 or len(labels) < 2 or
                any(not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label) for label in labels) or
                all(label.isdigit() for label in labels)):
            raise ValueError('--domain must be a DNS hostname, without a scheme, port or path')
        if address.is_loopback:
            raise ValueError('public HTTPS deployment requires the server IPv4 address, not loopback')
    if tls_mode is None:
        tls_mode = values.get('PACKETIA_TLS_MODE') if domain == previous_domain else None
        tls_mode = tls_mode or ('auto' if domain else 'internal')
    if tls_mode not in ('auto', 'internal'):
        raise ValueError('--tls-mode must be auto or internal')
    if tls_mode == 'auto' and not domain and not address.is_loopback:
        raise ValueError('automatic public certificates require --domain; use --tls-mode internal for IP testing')
    https_port = int(https_port if https_port is not None else values.get('PACKETIA_HTTPS_PORT', '443'))
    if not 1 <= https_port <= 65535 or https_port == 80:
        raise ValueError('--https-port must be 1..65535 and distinct from the HTTP redirect port 80')
    for key in SECRET_KEYS:
        if key not in values:
            values[key] = secrets.token_hex(32)
        if not re.fullmatch(r'[A-Za-z0-9_-]{32,4096}', values[key]):
            raise ValueError(f'{key} must contain 32..4096 letters, digits, underscores or hyphens')
    if len({values[key] for key in SECRET_KEYS}) != len(SECRET_KEYS):
        raise ValueError('JWT and per-node secrets must be distinct')
    values.setdefault('PACKETIA_JWT_ISSUER', 'packetia')
    values.setdefault('PACKETIA_PROJECT_NAME', 'packetia')
    values.setdefault('PACKETIA_IMAGE', 'packetia-cluster:local')
    values.setdefault('PACKETIA_BUILD_JOBS', '2')
    values.setdefault('PACKETIA_LOG_FORMAT', 'json')
    values.setdefault('PACKETIA_LOG_LEVEL', 'info')
    values.setdefault('PACKETIA_QOS_LOG', '0')
    values['PACKETIA_DOMAIN'] = domain
    values['PACKETIA_PUBLIC_IP'] = str(address)
    values['PACKETIA_PUBLIC_HOST'] = public_host(values)
    values['PACKETIA_HTTPS_PORT'] = str(https_port)
    values['PACKETIA_TLS_MODE'] = tls_mode
    values['PACKETIA_TLS_CONFIG'] = 'tls internal' if tls_mode == 'internal' else ''
    values['PACKETIA_CONFIG_DIR'] = str(runtime)

    control = json.loads((DEPLOY_DIR / 'control.json').read_text())
    if registration is None:
        current = runtime / 'control.json'
        registration = json.loads(current.read_text()).get('registration_enabled', False) if current.exists() else False
    control['registration_enabled'] = registration
    configs = {'control': control}
    for index, name in enumerate(('sfu-a', 'sfu-b')):
        node = json.loads((DEPLOY_DIR / f'{name}.json').read_text())
        node['public_ip'] = str(address)
        node['signaling_url'] = (f'{public_origin(values).replace("https:", "wss:")}/{name}/webrtc' if public_host(values) else
                                 f'ws://{address}:{node["websocket_port"]}/webrtc')
        node.update(management_url=f'http://{name}:{19080 + index}',
                    internal_listen_ip='0.0.0.0', internal_http_port=19080 + index,
                    metrics_listen_ip='0.0.0.0', metrics_http_port=18084 + index)
        configs[name] = node
    if any('\n' in value or '\r' in value for value in values.values()):
        raise ValueError('environment values cannot contain newlines')
    runtime.mkdir(parents=True, exist_ok=True)
    for name, config in configs.items():
        write_file(runtime / f'{name}.json', json.dumps(config, indent=2) + '\n', 0o644)
    lines = ['# Generated by deploy.py init. Preserve this file across updates.']
    for key, value in sorted(values.items()):
        escaped = value.replace("'", "\\'")
        lines.append(f"{key}='{escaped}'")
    write_file(runtime / '.env', '\n'.join(lines) + '\n', 0o600)
    return values


def compose_command(runtime, values, docker='docker'):
    command = [docker, 'compose', '--project-name', values.get('PACKETIA_PROJECT_NAME', 'packetia'),
               '--env-file', str(runtime / '.env'), '-f', str(DEPLOY_DIR / 'compose.yaml')]
    if public_host(values):
        command += ['-f', str(DEPLOY_DIR / 'compose.public.yaml')]
    return command


def operate(runtime, args):
    if not (runtime / '.env').is_file():
        raise ValueError('run deploy.py init before operating the stack')
    for name in ('control', 'sfu-a', 'sfu-b'):
        if not (runtime / f'{name}.json').is_file():
            raise ValueError(f'missing {name}.json; run deploy.py init')
    docker = shutil.which('docker')
    if not docker:
        raise ValueError('install Docker Engine and the Docker Compose V2 plugin first')
    values = read_environment(runtime / '.env')
    if args.action == 'up' and public_host(values) and not values.get('PACKETIA_PUBLIC_HOST'):
        raise ValueError('run deploy.py init again to configure the public reverse proxy')
    command = compose_command(runtime, values, docker)
    env = dict(os.environ, **values)

    def run(arguments):
        subprocess.run(command + arguments, cwd=DEPLOY_DIR, env=env, check=True)

    if args.action == 'up':
        run(['config', '--quiet'])
        if args.build:
            run(['build', 'control'])
        options = ['up', '-d', '--no-build', '--wait', '--wait-timeout', str(args.timeout)]
        if args.recreate:
            options.append('--force-recreate')
        if args.single_sfu:
            options += ['control', 'sfu-a']
            if public_host(values):
                options.append('edge')
        run(options)
        run(['ps'])
    elif args.action == 'build':
        run(['build', 'control'])
    elif args.action == 'check':
        run(['config', '--quiet'])
        backends = [('control', 18082), ('sfu-a', 19080)]
        if not args.single_sfu:
            backends.append(('sfu-b', 19081))
        for name, port in backends:
            run(['exec', '-T', name, 'curl', '-fsS', '--max-time', '3', f'http://127.0.0.1:{port}/readyz'])
        print('\nSelected backends are ready. Public TLS and UDP reachability must be checked from a client.')
    elif args.action == 'logs':
        if any(name not in SERVICES for name in args.services):
            raise ValueError('unknown service; choose control, sfu-a, sfu-b or edge')
        options = ['logs', '--tail', str(args.tail)]
        if args.follow:
            options.append('--follow')
        run(options + args.services)
    else:
        run([args.action])


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('must be positive')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-dir', type=Path, default=DEPLOY_DIR / 'runtime')
    actions = parser.add_subparsers(dest='action', required=True)
    init = actions.add_parser('init', help='generate configs and persistent credentials without starting services')
    init.add_argument('--public-ip', help='server IPv4 address advertised to media clients')
    init.add_argument('--domain', help='DNS hostname for automatic HTTPS/WSS; omit for public-IP testing or local development')
    init.add_argument('--tls-mode', choices=('auto', 'internal'),
                      help='auto: public domain certificate; internal: Caddy test CA (default for IP testing)')
    init.add_argument('--https-port', type=int, help='public HTTPS/WSS port (default: 443; use 8443 if occupied)')
    init.add_argument('--registration', action='store_true', default=None, help='enable account registration')
    init.add_argument('--no-registration', action='store_false', dest='registration', help='disable account registration')
    init.set_defaults(registration=None)
    up = actions.add_parser('up', help='start services and wait for health checks')
    up.add_argument('--build', action='store_true', help='build the shared Control/SFU image first')
    up.add_argument('--recreate', action='store_true', help='recreate containers to reload changed config files')
    up.add_argument('--single-sfu', action='store_true', help='start only Control, SFU A and the public proxy')
    up.add_argument('--timeout', type=positive, default=120, help='health-check wait timeout in seconds')
    for name in ('build', 'down', 'ps'):
        actions.add_parser(name)
    check = actions.add_parser('check')
    check.add_argument('--single-sfu', action='store_true', help='check Control and SFU A only')
    logs = actions.add_parser('logs')
    logs.add_argument('services', nargs='*')
    logs.add_argument('--tail', type=positive, default=200)
    logs.add_argument('--follow', '-f', action='store_true')
    args = parser.parse_args()
    try:
        if args.action == 'init':
            values = initialize(args.runtime_dir, args.public_ip, args.domain, args.registration, args.tls_mode, args.https_port)
            print(f'Configuration: {args.runtime_dir.resolve()}')
            endpoint = public_origin(values) or 'http://127.0.0.1:18082'
            print(f'Control API: {endpoint}/api/v1/')
            if public_host(values) and values['PACKETIA_TLS_MODE'] == 'internal':
                print('HTTPS uses the Caddy test CA. Export its root certificate after startup and trust it on test clients; '
                      'see deploy/cluster/README.md.')
            print(f'Next: python3 {Path(__file__).resolve()} up '
                  '(add --single-sfu for one SFU; add --build only if the backend image needs building)')
        else:
            operate(args.runtime_dir.resolve(), args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Deployment failed: {error}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == '__main__':
    sys.exit(main())
