"""Real RFC 6455 socket tests; Python standard library only."""
import base64
import hashlib
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time


class Client:
    def __init__(self, port, path="/signal"):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.file = self.sock.makefile("rb")
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        assert b"101" in self.file.readline()
        headers = {}
        while True:
            line = self.file.readline()
            if line == b"\r\n":
                break
            assert line
            name, value = line.decode().split(":", 1)
            headers[name.lower()] = value.strip()
        expected = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest()).decode()
        assert headers["sec-websocket-accept"] == expected

    def send(self, payload, opcode=1, final=True):
        if isinstance(payload, str):
            payload = payload.encode()
        header = bytes([(0x80 if final else 0) | opcode])
        length = len(payload)
        if length < 126:
            header += bytes([0x80 | length])
        elif length <= 65535:
            header += bytes([0x80 | 126]) + struct.pack("!H", length)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", length)
        mask = os.urandom(4)
        masked = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def exact(self, count):
        data = self.file.read(count)
        assert len(data) == count, "unexpected socket close"
        return data

    def frame(self):
        first, second = self.exact(2)
        assert not second & 0x80, "server frames must not be masked"
        length = second & 127
        if length == 126:
            length = struct.unpack("!H", self.exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.exact(8))[0]
        return bool(first & 128), first & 15, self.exact(length)

    def message(self):
        parts = []
        while True:
            final, opcode, payload = self.frame()
            if opcode == 9:
                self.send(payload, 10)
                continue
            assert opcode == (1 if not parts else 0), (opcode, payload)
            parts.append(payload)
            if final:
                return b"".join(parts)

    def closed(self, expected=None):
        _, opcode, payload = self.frame()
        assert opcode == 8, (opcode, payload)
        if expected is not None:
            assert len(payload) >= 2
            assert struct.unpack("!H", payload[:2])[0] == expected
        self.send(payload, 8)
        self.dispose()

    def dispose(self):
        self.file.close()
        self.sock.close()


def main():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [sys.argv[1], str(port)], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, text=True, bufsize=1,
    )
    events = queue.Queue()
    threading.Thread(target=lambda: [events.put(line.strip()) for line in process.stdout],
                     daemon=True).start()
    pending = []
    clients = []

    def event(prefix):
        deadline = time.monotonic() + 8
        while True:
            for i, value in enumerate(pending):
                if value == prefix or value.startswith(prefix + " "):
                    return pending.pop(i)
            remaining = deadline - time.monotonic()
            assert remaining > 0, (prefix, pending, process.poll())
            pending.append(events.get(timeout=remaining))

    def command(value, result):
        process.stdin.write(value + "\n")
        process.stdin.flush()
        return event(result)

    def connect():
        client = Client(port)
        clients.append(client)
        opened = event("OPEN").split()
        assert opened[2] == "/signal" and opened[3]
        return client, opened[1]

    try:
        event("READY")
        # A second server must report bind failure, not start a worker silently.
        conflict = subprocess.run([sys.argv[1], str(port)], input="QUIT\n",
                                  text=True, capture_output=True, timeout=10)
        assert conflict.returncode == 2, conflict.stderr

        client, cid = connect()
        for value in ["hello", "???WebSocket", "x" * 100000]:
            client.send(value)
            assert client.message() == value.encode()
        client.send("__empty__")
        assert client.message() == b""
        # Split a UTF-8 character between continuation frames; interleave ping.
        payload = "fragment:??".encode()
        client.send(payload[:10], final=False)
        client.send(b"ping", opcode=9)
        _, opcode, pong = client.frame()
        assert opcode == 10 and pong == b"ping"
        client.send(payload[10:], opcode=0)
        assert client.message() == payload
        for i in range(10):
            client.send(str(i))
        for i in range(10):
            assert client.message() == str(i).encode()
        command("PUSH " + cid + " worker-message", "PUSHED")
        assert client.message() == b"worker-message"
        command("CLOSE " + cid, "CLOSING")
        client.closed(1000)
        event("CLOSED " + cid)
        command("PUSH " + cid + " late-message", "REJECTED")

        client, cid = connect()
        client.send(b"binary", opcode=2)
        client.closed(1003)
        event("CLOSED " + cid)

        client, cid = connect()
        client.send(b"x" * (1024 * 1024), final=False)
        client.send(b"x", opcode=0)
        client.closed(1009)
        event("CLOSED " + cid)

        client, cid = connect()
        client.send("__close__")
        client.closed(1000)
        event("CLOSED " + cid)

        client, cid = connect()
        client.send("__throw__")
        # Exception containment may close without a close status.
        try:
            client.frame()
        except (AssertionError, OSError):
            pass
        client.dispose()
        event("CLOSED " + cid)

        client, cid = connect()
        client.send(struct.pack("!H", 1000), opcode=8)
        _, opcode, _ = client.frame()
        assert opcode == 8
        client.dispose()
        event("CLOSED " + cid)

        command("STOP", "STOPPED")  # must wake an idle service loop
        command("START", "READY")
        client, cid = connect()
        command("STOP", "STOPPED")
        event("CLOSED " + cid)
        client.dispose()
        command("START", "READY")
        client, cid = connect()
        client.send("__stop__")  # callback shutdown must not self-join
        event("CLOSED " + cid)
        command("STOP", "STOPPED")
        client.dispose()
        command("START", "READY")
        client, cid = connect()
        client.send("after restart")
        assert client.message() == b"after restart"
        command("STOP", "STOPPED")
        event("CLOSED " + cid)
        process.stdin.write("QUIT\n")
        process.stdin.flush()
        assert process.wait(timeout=5) == 0
        print("PASS: sessions, handshake, UTF-8, fragmentation, queues, send/close, limits, restart")
    finally:
        for client in clients:
            client.dispose()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
