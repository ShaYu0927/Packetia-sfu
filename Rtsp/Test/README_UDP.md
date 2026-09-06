# RTSP UDP transport

SETUP with `RTP/AVP;unicast;client_port=P-Q` reserves an exclusive even/odd
server port pair for that track. Its response advertises both `client_port`
and `server_port`. The RTP socket's callback is bound to RTP and the RTCP
socket's callback to RTCP; neither guesses its role from the second byte.
Packet structure and source address are still validated.

`UdpMediaTransport` has separate-port and single-transport constructors. The
latter explicitly enables RTP/RTCP mux and performs packet classification.
WebRTC continues using `WebRtcTransport` for STUN/DTLS/SRTP/SRTCP demultiplexing.
The generic UDP server and datagram transport remain protocol-neutral.

Each dedicated socket learns its first valid source port from the IP of the
RTSP control connection, supporting NAT port translation, then pins it.
Outbound RTP and RTCP use their respective socket and selected peer. TEARDOWN
or control-connection closure releases both sockets and the endpoint binding.

This adds UDP transport to the existing RTSP publishing/receiving path; it
does not implement RTSP PLAY. Multicast and RTSP `rtcp-mux` negotiation return
461. Invalid transport parameters and allocation failures receive an explicit
error response without registering a track endpoint.

## Verification

```sh
cmake --build build --target test_udp_binding test_datagram_transport
ctest --test-dir build -R 'test_udp_binding|test_datagram_transport' --output-on-failure
```

The tests cover role-bound and multiplexed delivery, independent socket/peer
selection, NAT source-port learning, exclusive port ownership, malformed
parameters, and live RTSP SETUP through H264 frame delivery and RTCP feedback
to TEARDOWN. Existing TCP interleaved negotiation is also exercised.

Design references (independently implemented):

- [ZLMediaKit RTSP UDP SETUP](https://github.com/ZLMediaKit/ZLMediaKit/blob/master/src/Rtsp/RtspSession.cpp)
- [ZLMediaKit WebRTC demultiplexing](https://github.com/ZLMediaKit/ZLMediaKit/blob/master/webrtc/WebRtcTransport.cpp)
