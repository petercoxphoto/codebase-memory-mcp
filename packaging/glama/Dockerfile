# Glama MCP directory (glama.ai) check image — NOT required to run the tool.
#
# codebase-memory-mcp ships as a single, fully-static Linux binary (gcc -static)
# and needs NO Docker to run. This image exists only so Glama can build a
# sandbox, launch the stdio MCP server, and run its introspection checks, which
# power the directory's score badge. The same image is exercised locally and in
# CI by packaging/glama/verify.sh to guard against drift.
#
# The binary is static, so the runtime base is arbitrary. We fetch the latest
# released binary for the build platform's architecture (TARGETARCH is amd64 /
# arm64, matching the release asset names).

FROM debian:bookworm-slim AS fetch
ARG TARGETARCH
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL "https://github.com/DeusData/codebase-memory-mcp/releases/latest/download/codebase-memory-mcp-linux-${TARGETARCH}.tar.gz" \
      | tar -xz -C /tmp codebase-memory-mcp \
 && chmod +x /tmp/codebase-memory-mcp \
 && rm -rf /var/lib/apt/lists/*

FROM debian:bookworm-slim
COPY --from=fetch /tmp/codebase-memory-mcp /usr/local/bin/codebase-memory-mcp
ENV CBM_CACHE_DIR=/tmp/cbm
ENTRYPOINT ["codebase-memory-mcp"]
