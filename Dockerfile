# ------------------------------------------------------------------------------
# Stage 1: Build Environment
# ------------------------------------------------------------------------------
FROM gcc:12-bookworm AS builder

WORKDIR /usr/src/byo-redis

# Copy source files, headers, and Makefile
COPY Makefile ./
COPY src/ ./src/
COPY tests/ ./tests/

# Build core server and run data-structure unit test suite
RUN make server test && make test

# ------------------------------------------------------------------------------
# Stage 2: Minimal Production Runtime
# ------------------------------------------------------------------------------
FROM debian:bookworm-slim

# Create dedicated unprivileged user for security
RUN groupadd -r redisgroup && useradd -r -g redisgroup -d /data -s /sbin/nologin redisuser

# Create persistent storage directory for AOF files
RUN mkdir -p /data && chown -R redisuser:redisgroup /data

# Copy compiled binary from builder stage
COPY --from=builder /usr/src/byo-redis/server /usr/local/bin/server

# Switch to unprivileged runtime user
USER redisuser
WORKDIR /data

# Expose standard server port
EXPOSE 1234

# Health check using bash TCP probe
HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
  CMD (exec 3<>/dev/tcp/127.0.0.1/1234 && echo -e "*1\r\n\$4\r\nPING\r\n" >&3 && head -n 1 <&3 | grep -q "PONG") || exit 1

# Launch server with AOF persistence enabled
ENTRYPOINT ["/usr/local/bin/server"]
CMD ["--aof", "/data/appendonly.aof", "-p", "1234"]

