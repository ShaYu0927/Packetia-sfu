# WebRTC TrendlineEstimator source import

This directory vendors WebRTC's trendline delay detector, pinned to commit
`afef16afa42fb248d85c717bfbd88d612a37722a`. It imports one subalgorithm, not the
complete libwebrtc congestion controller or its network stack.

## Source and layout

- Repository: https://webrtc.googlesource.com/src
- [Pinned implementation](https://webrtc.googlesource.com/src/+/afef16afa42fb248d85c717bfbd88d612a37722a/modules/congestion_controller/goog_cc/trendline_estimator.cc)
- `UPSTREAM.json`: revision, original paths and SHA-256 hashes.
- `upstream/`: byte-for-byte source, unit tests, enum and license snapshots.
- `include/webrtc_trendline/` and `port/`: the production port.
- `tests/compat/`: default-profile dependency shims used only to compile the
  untouched reference and its unit tests. These headers are not on the
  production target's include path.
- `tests/test_port_equivalence.cpp`: compares the port against the reference
  after every input, including stable delivery, growing/draining queues,
  jitter, spikes and inputs with no calculated group deltas.

The WebRTC files retain their original copyright notices. `LICENSE`, `PATENTS`
and `AUTHORS` are copied from the pinned upstream revision. Packetia adaptations
are identified in the production headers and source.

## Local adaptations

The production namespace is `packetia::webrtc_port`. The port uses the upstream
default settings: a 20-sample window, smoothing coefficient 0.9, threshold gain
4.0, sorting disabled and slope capping disabled. The original sorting and cap
calculations remain in the source, but non-default configurations are not exposed.

The `FieldTrialsView` string parser, optional `NetworkStatePredictor`, dependency
on `DelayIncreaseDetectorInterface`, and constructor logging are removed.
`RTC_DCHECK` becomes `assert`; the threshold's `SafeClamp` becomes `std::clamp`
with double bounds. Read-only diagnostics expose window readiness, smoothed
delay, slope and threshold. Initialization, regression, overuse state transitions
and threshold adaptation retain the upstream calculation order.

The reference tests use empty field trials and a null predictor. Their shims
provide only those dependencies: logging is discarded, default settings are
left unchanged, and the parser rejects nonempty experiment strings. This does
not validate other field trials or the complete WebRTC build.

## Build and compare

No network fetch is performed by CMake. With CMake and GoogleTest installed:

```sh
cmake -S third/webrtc_trendline -B build/webrtc-trendline -DCMAKE_BUILD_TYPE=Debug
cmake --build build/webrtc-trendline --parallel
ctest --test-dir build/webrtc-trendline --output-on-failure
```

The same `webrtc_trendline` test is registered by the root project when
`BUILD_TESTING` is enabled. It runs all four original upstream unit tests and
the 2400-input default-profile equivalence trace. The production library target
is `packetia_webrtc_trendline`; the reference implementation is linked only into
the test executable.

Compare the adaptations directly:

```sh
diff -u third/webrtc_trendline/upstream/modules/congestion_controller/goog_cc/trendline_estimator.cc \
  third/webrtc_trendline/port/trendline_estimator.cc
```

A `diff` exit status of 1 is expected because adaptations exist.
For the integration and reading order, see [Packetia's study guide](../../docs/webrtc-trendline.md).

## Updating

Choose a new upstream commit explicitly. Refresh the original files, license
documents and manifest hashes together, then review the port against that
snapshot and run both equivalence and Packetia integration tests. Do not refresh
only the original reference or only the adapted implementation.
