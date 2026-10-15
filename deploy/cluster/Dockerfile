FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config ca-certificates \
    libssl-dev libsqlite3-dev libcurl4-openssl-dev libwebsockets-dev libsrtp2-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /tmp/packetia-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DPACKETIA_WITH_LIBWEBSOCKETS=ON -DPACKETIA_WITH_SRTP=ON \
    && cmake --build /tmp/packetia-build --target PacketiaControl PacketiaSfu --parallel 2

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 libsqlite3-0 libcurl4 libwebsockets17 libsrtp2-1 \
    libavcodec59 libavformat59 libavutil57 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir /data && chown 10001:10001 /data
COPY --from=build /tmp/packetia-build/server/PacketiaControl /usr/local/bin/PacketiaControl
COPY --from=build /tmp/packetia-build/server/PacketiaSfu /usr/local/bin/PacketiaSfu
USER 10001:10001
WORKDIR /data
CMD ["PacketiaControl", "/etc/packetia/control.json"]
