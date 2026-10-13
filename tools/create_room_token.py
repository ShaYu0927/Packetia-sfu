#!/usr/bin/env python3
"""Issue a Packetia room JWT on a trusted backend; print only the signed token."""

import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import time


def encode(value):
    raw = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return base64.urlsafe_b64encode(raw).rstrip(b"=")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", required=True, help="Authenticated user identity (JWT sub)")
    parser.add_argument("--room", required=True, help="Room this user is allowed to join")
    parser.add_argument("--ttl", type=int, default=3600, help="Admission lifetime in seconds (default: 3600)")
    parser.add_argument("--can-publish", action="store_true", help="Permit publishing audio/video")
    parser.add_argument("--can-subscribe", action="store_true", help="Permit subscribing to room tracks")
    parser.add_argument("--name", help="Participant display name")
    args = parser.parse_args()
    key = os.environ.get("PACKETIA_WEBRTC_API_KEY", "")
    secret = os.environ.get("PACKETIA_WEBRTC_API_SECRET", "").encode("utf-8")
    if not re.fullmatch(r"[!-~]{1,128}", key) or not 32 <= len(secret) <= 4096:
        parser.error("set PACKETIA_WEBRTC_API_KEY (1..128 printable ASCII bytes) and PACKETIA_WEBRTC_API_SECRET (32..4096 bytes)")
    if not args.identity or len(args.identity.encode("utf-8")) > 128 or any(ord(ch) < 32 or ord(ch) == 127 for ch in args.identity):
        parser.error("--identity must contain 1..128 UTF-8 bytes without control characters")
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", args.room):
        parser.error("--room must contain 1..64 letters, digits, underscores, dots or hyphens")
    if args.ttl < 1 or args.ttl > 86400:
        parser.error("--ttl must be between 1 and 86400 seconds")
    if args.name is not None and (len(args.name.encode("utf-8")) > 128 or any(ord(ch) < 32 or ord(ch) == 127 for ch in args.name)):
        parser.error("--name must contain at most 128 UTF-8 bytes without control characters")
    now = int(time.time())
    claims = {
        "iss": key, "sub": args.identity, "aud": "packetia-webrtc",
        "iat": now, "nbf": now, "exp": now + args.ttl,
        "video": {"room": args.room, "roomJoin": True,
                  "canPublish": args.can_publish, "canSubscribe": args.can_subscribe},
    }
    if args.name is not None:
        claims["name"] = args.name
    signing_input = encode({"alg": "HS256", "typ": "JWT"}) + b"." + encode(claims)
    signature = base64.urlsafe_b64encode(hmac.new(secret, signing_input, hashlib.sha256).digest()).rstrip(b"=")
    print((signing_input + b"." + signature).decode("ascii"))


if __name__ == "__main__":
    main()
