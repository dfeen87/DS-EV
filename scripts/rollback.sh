#!/usr/bin/env bash
# Verified software snapshots; no atomic/power-loss guarantee.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${SCRIPT_DIR}/backup_restore.py" "$@"
