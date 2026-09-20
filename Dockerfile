# Multi-stage Dockerfile for MarketPulse
# Target: linux/arm64 (aarch64)
# Final image: < 100 MB
#
# Build: docker build --platform linux/arm64 -t marketpulse .
# Run:   docker run -p 9001:9001 marketpulse --symbol btcusdt

# ============================================================
# Stage 1: Builder
# ============================================================
FROM ubuntu:24.04 AS builder

ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    gcc-13 \
    g++-13 \
    git \
    libssl-dev \
    zlib1g-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Use GCC 13 explicitly
ENV CC=gcc-13
ENV CXX=g++-13

WORKDIR /build/src

COPY . .

# Configure and build with Release optimisations
RUN cmake -B /build/out \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
        -GNinja \
    && cmake --build /build/out --parallel $(nproc)

# Run tests in the builder (sanity check before producing image)
RUN ctest --test-dir /build/out --output-on-failure -R "test_sequencer|test_spsc|test_signals"

# ============================================================
# Stage 2: Runtime
# ============================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl3 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Non-root user
RUN useradd -r -s /bin/false marketpulse

WORKDIR /app

COPY --from=builder /build/out/marketpulse ./marketpulse
COPY --from=builder /build/src/tests/fixtures/ ./tests/fixtures/

RUN chown -R marketpulse:marketpulse /app

USER marketpulse

EXPOSE 9001

ENTRYPOINT ["/app/marketpulse"]
CMD ["--symbol", "btcusdt"]
