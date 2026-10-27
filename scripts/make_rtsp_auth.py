#!/usr/bin/env python3
"""交互生成 RTSP Digest HA1 配置，不输出明文密码。"""

import argparse
import getpass
import hashlib
import json


def safe_name(value):
    return 1 <= len(value) <= 128 and all(
        33 <= ord(ch) <= 126 and ch not in '\\":' for ch in value
    )


def valid_path(value):
    return 1 <= len(value) <= 256 and all(
        ch in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./'
        for ch in value
    ) and all(part not in ('', '.', '..') for part in value.split('/'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--username', required=True)
    parser.add_argument('--realm', default='packetia')
    parser.add_argument('--stream', action='append', required=True, help='精确流路径，可重复指定')
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--play', action='store_true')
    args = parser.parse_args()
    if not safe_name(args.username) or not safe_name(args.realm):
        parser.error('用户名和 realm 必须是 1–128 个可打印 ASCII 字符，不能含冒号、引号或反斜杠')
    if not (args.publish or args.play):
        parser.error('至少指定 --publish 或 --play')
    if len(args.stream) > 128 or len(set(args.stream)) != len(args.stream) or not all(map(valid_path, args.stream)):
        parser.error('流路径无效、重复或数量超过 128')
    password = getpass.getpass('RTSP password: ')
    if not password or password != getpass.getpass('Confirm password: '):
        parser.error('密码不能为空，两次输入必须一致')
    ha1 = hashlib.md5(f'{args.username}:{args.realm}:{password}'.encode('utf-8')).hexdigest()
    config = {'rtsp_auth': {
        'enabled': True,
        'realm': args.realm,
        'nonce_ttl_seconds': 300,
        'allow_legacy_digest': True,
        'users': [{
            'username': args.username,
            'ha1': ha1,
            'streams': [dict(path=path, can_publish=args.publish, can_play=args.play) for path in args.stream],
        }],
    }}
    print(json.dumps(config, indent=2))


if __name__ == '__main__':
    main()
