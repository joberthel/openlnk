#!/usr/bin/env bash
# analysis/start_ghidra_mcp.sh
# Launch the Ghidra MCP server standalone (stdio or local HTTP on 127.0.0.1:8080).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_PATH="${SCRIPT_DIR}/ghidra_project"
TRANSPORT="${1:-http}"
HOST="${GHIDRA_MCP_HOST:-127.0.0.1}"
PORT="${GHIDRA_MCP_PORT:-8080}"

if [[ "$TRANSPORT" == "stdio" ]]; then
    exec python3 -m ghidra_mcp.server --project-path "$PROJECT_PATH"
elif [[ "$TRANSPORT" == "http" ]]; then
    exec python3 -m ghidra_mcp.server \
        --project-path "$PROJECT_PATH" \
        --transport http \
        --host "$HOST" \
        --port "$PORT"
else
    echo "Usage: $(basename "$0") [http|stdio]" >&2
    exit 2
fi
