FROM debian:bookworm-slim AS build
ARG BUILD_JOBS=2
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config ca-certificates \
    libssl-dev libsqlite3-dev libcurl4-openssl-dev libwebsockets-dev libsrtp2-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /tmp/packetia-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DPACKETIA_WITH_LIBWEBSOCKETS=ON -DPACKETIA_WITH_SRTP=ON \
    && cmake --build /tmp/packetia-build --target PacketiaRoomDemo --parallel "${BUILD_JOBS}"

FROM node:22-bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 libsqlite3-0 libcurl4 libwebsockets17 libsrtp2-1 \
    libavcodec59 libavformat59 libavutil57 libswscale6 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /tmp/packetia-build/server/PacketiaRoomDemo /usr/local/bin/PacketiaRoomDemo
WORKDIR /app
COPY --chown=node:node examples/webrtc-room/ ./
ENV PACKETIA_ROOM_DEMO_BINARY=/usr/local/bin/PacketiaRoomDemo \
    PACKETIA_LOG_MODE=console PACKETIA_LOG_FORMAT=json PACKETIA_QOS_LOG=1
USER node
CMD ["node", "dev-server.mjs"]
