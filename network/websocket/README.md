# WebSocket server

The server uses libwebsockets on one of Packetia's existing `TaskScheduler`
threads. It does not create a separate network thread or event loop. Start the
`EventLoop` before `WsServer`, and normally stop the server before the loop.

## Concurrency and backpressure

- Workers may call `SendText`, `CloseConnection`, and `GetStats` concurrently.
  Only the I/O owner calls libwebsockets. Connection lookup uses a shared lock.
- An empty-to-nonempty send queue schedules one notification. A deduplicated
  ready list arms writable callbacks in bounded batches, without scanning idle
  sessions. libwebsockets still uses its own poll backend, serviced every 5 ms.
- Each writable callback writes at most one 16 KiB fragment. All connections
  share an owner-thread scratch buffer. The library handles socket short writes.
- A send reservation covers both queued and currently fragmented messages.
  Reservations are returned on completion or close, including failure paths.
- `SendText == true` means the bounded application queue accepted the message;
  it does not guarantee delivery. On `false`, producers must back off, drop an
  optional notification, or close the slow connection. Do not spin retry on the
  I/O thread. An automatic reply that cannot be queued closes that connection.
- Empty strings sent with `SendText` are real empty text frames. An empty return
  from the message callback means no automatic reply.

The default limits are configurable through `WsServerOptions`:

| Option | Default |
| --- | --- |
| `limits.max_connections` | 4096 |
| `max_pending_handshakes` | 128 |
| `handshake_timeout_seconds` | 10 seconds |
| `limits.max_message_bytes` | 1 MiB per received or sent message |
| `limits.max_queued_bytes` | 4 MiB per connection, including in-flight messages |
| `limits.max_queued_messages` | 256 per connection |
| `limits.max_total_queued_bytes` | 64 MiB across all sending connections |
| `limits.max_total_queued_messages` | 16384 across all sending connections |
| `max_writable_batch` | 256 connections per notification batch |
| `service_interval_ms` | 5 ms |

Server options must be positive. The connection count is an admission limit,
not a measured capacity guarantee; the process file descriptor limit must also
allow the configured sockets. Send budgets count payload bytes, not object
overhead, receive buffers, libwebsockets buffers, or kernel socket memory. Each
connection can additionally hold one incomplete received message up to its
message limit; choose the connection and message limits together.

`GetStats()` reports active connections, outstanding send payload/messages,
accepted/rejected connections, completed received/sent messages, and rejected
message admissions. Counters accumulate across server restarts. Concurrent
reads are diagnostic snapshots, not a transaction across every counter.

## Callbacks and lifecycle

Open/message/close callbacks run on the I/O owner, outside session and registry
locks. Keep them short. For parsing or business work that can block, post to
the project's `WorkerService` with a stable connection affinity key, return an
empty reply, and call `SendText` from the worker when finished. Callbacks must
not wait for another thread that is waiting for `Start` or `Stop` to finish.

Exceptions from callbacks close the affected connection and never cross the
library's C callback boundary. A callback may call `Stop`; destruction waits
until libwebsockets has unwound. Multiple stopping callers are also serialized
after the EventLoop exits. In that case final close callbacks run on the
controlling cleanup thread, so callback state must outlive `Stop`.

An EventLoop-based server can be stopped and restarted after its loop is
restarted. It cleans up its old context before rebinding to the new scheduler;
stale scheduled notifications cannot operate on a new context. The constructor
that accepts a scheduler explicitly remains bound to that scheduler. The
EventLoop object must outlive an EventLoop-based server.

This module provides text WebSocket transport. The default application callback
in `server/main.cpp` returns an acknowledgement; room and WebRTC signaling must
be connected by the application.

## Build and validation

On Linux, install a C++17 compiler, CMake, pkg-config, and libwebsockets development
headers/libraries. If libwebsockets was built with TLS enabled, its public headers
also require the OpenSSL development headers, even for a plain `ws://` listener.
Build this module independently of FFmpeg and libhv:

```sh
cmake -S network/websocket/Test -B build/websocket -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/websocket -j
ctest --test-dir build/websocket --output-on-failure
```

Discovery prefers `PACKETIA_LIBWEBSOCKETS_ROOT` (or `LIBWEBSOCKETS_ROOT`), then
pkg-config/system libraries, then the bundled Linux library. A custom static
build can set `PACKETIA_LIBWEBSOCKETS_EXTRA_LIBRARIES` for its dependencies.
Headers and binaries must match the host architecture and C library.

The tests exercise real HTTP upgrade and masked WebSocket frames, fragmentation,
empty messages, admission limits, concurrent producers, callback failures,
disconnect cleanup, and shutdown/restart. A separate load mode verifies messages
and prints the measured workload; it is not part of the default CTest run:

```sh
./build/websocket/test_ws_server --load 256 32
```

To exercise a running Packetia server with concurrent connection establishment
and measure request/reply latency, use the Python standard-library client:

```sh
python3 network/websocket/Test/websocket_stress.py --host 127.0.0.1 --port 8080 --clients 128 --messages 100
```

The Python tool accepts the application's acknowledgement replies; add
`--verify-echo` only when the server callback actually echoes the payload.

Measure on the intended deployment host before raising limits. Virtual-machine
test throughput is useful for regression checks, not production sizing.
