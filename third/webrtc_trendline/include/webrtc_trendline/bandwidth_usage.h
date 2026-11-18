/*
 *  Copyright (c) 2024 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PACKETIA_WEBRTC_TRENDLINE_BANDWIDTH_USAGE_H_
#define PACKETIA_WEBRTC_TRENDLINE_BANDWIDTH_USAGE_H_

namespace packetia::webrtc_port {

enum class BandwidthUsage {
  kBwNormal = 0,
  kBwUnderusing = 1,
  kBwOverusing = 2,
  kLast
};

}  // namespace packetia::webrtc_port

#endif  // PACKETIA_WEBRTC_TRENDLINE_BANDWIDTH_USAGE_H_
