FROM debian:bookworm AS builder

ARG DROGON_VERSION=v1.9.13

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libbrotli-dev \
    libcrypt-dev \
    libjsoncpp-dev \
    libpq-dev \
    libssl-dev \
    uuid-dev \
    zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN git clone --branch "${DROGON_VERSION}" --depth 1 --recurse-submodules \
      --shallow-submodules https://github.com/drogonframework/drogon.git \
  && cmake -S drogon -B drogon/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_CTL=OFF \
      -DBUILD_EXAMPLES=OFF \
      -DBUILD_ORM=ON \
      -DBUILD_TESTING=OFF \
  && cmake --build drogon/build --parallel 2 \
  && cmake --install drogon/build

WORKDIR /src
COPY . .

# GitHub web upload can flatten folders. Rebuild the canonical runtime layout
# when src/include/public/db/tests directories are absent.
RUN set -eux; \
    if [ ! -f src/main.cpp ]; then \
      mkdir -p src include/trpg tests db public/icons; \
      cp main.cpp Config.cpp Security.cpp CoreService.cpp LegacyCompat.cpp RoomSocket.cpp src/; \
      cp Config.h CoreService.h LegacyCompat.h LegacyRouteManifest.h RoomSocket.h Security.h include/trpg/; \
      cp security_test.cpp tests/; \
      cp legacy_v39_migrations.sql db/; \
      cp index.html native-socket.js sw.js manifest.webmanifest public/; \
      cp favicon-32.png apple-touch-icon.png icon-192.png icon-512.png public/icons/; \
    fi

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  && cmake --build build --parallel 2 \
  && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libbrotli1 \
    libcrypt1 \
    libjsoncpp25 \
    libpq5 \
    libssl3 \
    libuuid1 \
    zlib1g \
  && rm -rf /var/lib/apt/lists/* \
  && useradd --create-home --uid 10001 trpg

COPY --from=builder /usr/local /usr/local
COPY --from=builder /src/build/trpg_cpp /app/trpg_cpp
COPY --from=builder /src/public /app/public
COPY --from=builder /src/db /app/db
RUN ldconfig && chown -R trpg:trpg /app

USER trpg
WORKDIR /app
ENV PORT=10000 \
    APP_ENV=production \
    DOCUMENT_ROOT=/app/public
EXPOSE 10000

CMD ["./trpg_cpp"]
