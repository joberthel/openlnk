#!/usr/bin/env bash
# tools/ci/install_hooks.sh
# Activates version-controlled clean-room isolation hooks in this git clone.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
chmod +x "${REPO_ROOT}/.githooks/pre-commit"
git -C "${REPO_ROOT}" config core.hooksPath .githooks
echo "[install_hooks] Configured git core.hooksPath = .githooks"
