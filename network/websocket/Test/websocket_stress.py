#!/usr/bin/env python3
"""Measure concurrent WebSocket request/reply traffic using only Python stdlib.

Example against Packetia's default acknowledgement callback:
  python3 websocket_stress.py --clients 128 --messages 100 --payload-bytes 256

Use --verify-echo for an echo callback. The elapsed measurement includes TCP and
WebSocket connection setup; latency measures each individual request/reply.
"""

import argparse
import asyncio
import base64
from collections import Counter
import hashlib
import json
import os
import struct
import time


def encode_frame(opcode, payload):
    mask = os.urandom(4)
    size = len(payload)
    if size < 126:
        header = bytes((0x80 | opcode, 0x80 | size))
    elif size <= 65535:
        header = bytes((0x80 | opcode, 0xFE)) + struct.pack("!H", size)
    else:
        header = bytes((0x80 | opcode, 0xFF)) + struct.pack("!Q", size)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    return header + mask + masked


async def receive_text(reader, writer):
    message = bytearray()
    started = False
    while True:
        first, second = await reader.readexactly(2)
        if first & 0x70 or second & 0x80:
            raise ValueError("unexpected extension or masked server frame")
        opcode, final = first & 0x0F, bool(first & 0x80)
        size = second & 0x7F
        if size == 126:
            size = struct.unpack("!H", await reader.readexactly(2))[0]
        elif size == 127:
            size = struct.unpack("!Q", await reader.readexactly(8))[0]
        if size > 8 * 1024 * 1024:
            raise ValueError("server frame exceeds 8 MiB benchmark limit")
        payload = await reader.readexactly(size)
        if opcode == 8:
            code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
            raise ConnectionError(f"server closed connection (code={code})")
        if opcode == 9:
            writer.write(encode_frame(10, payload))
            await writer.drain()
            continue
        if opcode == 10:
            continue
        if opcode != (0 if started else 1):
            raise ValueError(f"unexpected message opcode {opcode}")
        started = True
        message.extend(payload)
        if len(message) > 8 * 1024 * 1024:
            raise ValueError("server message exceeds 8 MiB benchmark limit")
        if final:
            return bytes(message)


async def run_client(index, options):
    writer = None
    latencies = []
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(options.host, options.port), options.timeout
        )
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {options.path} HTTP/1.1\r\n"
            f"Host: {options.host}:{options.port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: packetia\r\n"
            f"Sec-WebSocket-Key: {key}\r\n\r\n"
        )
        writer.write(request.encode("ascii"))
        await asyncio.wait_for(writer.drain(), options.timeout)
        response = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), options.timeout)
        lines = response.decode("latin1").split("\r\n")
        if " 101 " not in lines[0]:
            raise ConnectionError(f"upgrade rejected: {lines[0]}")
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.lower()] = value.strip()
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode("ascii")
        if headers.get("sec-websocket-accept") != expected:
            raise ValueError("invalid Sec-WebSocket-Accept")
        for sequence in range(options.messages):
            prefix = f"{index}:{sequence}:".encode()
            payload = (prefix + b"x" * options.payload_bytes)[: options.payload_bytes]
            started = time.perf_counter()
            writer.write(encode_frame(1, payload))
            await asyncio.wait_for(writer.drain(), options.timeout)
            reply = await asyncio.wait_for(receive_text(reader, writer), options.timeout)
            if options.verify_echo and reply != payload:
                raise ValueError("echo payload mismatch")
            latencies.append((time.perf_counter() - started) * 1000)
        writer.write(encode_frame(8, struct.pack("!H", 1000)))
        await asyncio.wait_for(writer.drain(), options.timeout)
        return latencies, None
    except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError,
            asyncio.LimitOverrunError, ValueError) as error:
        return latencies, f"{type(error).__name__}: {error}"
    finally:
        if writer is not None:
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), options.timeout)
            except (OSError, asyncio.TimeoutError):
                pass


async def main(options):
    started = time.perf_counter()
    results = await asyncio.gather(*(run_client(index, options) for index in range(options.clients)))
    elapsed = time.perf_counter() - started
    latencies = sorted(latency for timings, _ in results for latency in timings)
    errors = Counter(error for _, error in results if error)

    def percentile(fraction):
        return round(latencies[min(len(latencies) - 1, int((len(latencies) - 1) * fraction))], 3) if latencies else None

    print(json.dumps({
        "clients": options.clients,
        "completed_clients": sum(error is None for _, error in results),
        "completed_messages": len(latencies),
        "elapsed_seconds_including_setup": round(elapsed, 3),
        "messages_per_second": round(len(latencies) / elapsed, 1),
        "request_reply_latency_ms": {"p50": percentile(0.50), "p95": percentile(0.95), "p99": percentile(0.99)},
        "errors": dict(errors),
    }, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--path", default="/")
    parser.add_argument("--clients", type=int, default=128)
    parser.add_argument("--messages", type=int, default=100)
    parser.add_argument("--payload-bytes", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--verify-echo", action="store_true")
    args = parser.parse_args()
    if args.clients < 1 or args.messages < 1 or not 0 <= args.payload_bytes <= 8 * 1024 * 1024 or args.timeout <= 0:
        parser.error("clients, messages and timeout must be positive; payload-bytes must be between 0 and 8 MiB")
    if not 0 < args.port <= 65535 or not args.path.startswith("/") or "\r" in args.path or "\n" in args.path:
        parser.error("port must be 1..65535 and path must be an absolute HTTP path")
    raise SystemExit(asyncio.run(main(args)))
