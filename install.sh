#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
if [[ "${1:-}" == "--legacy-plugin" ]]; then
    shift
    exec python3 "$ROOT/tools/install_plugin.py" install "$@"
fi
echo "Standalone input needs no global plugin installation. Checking prerequisites:"
exec python3 "$ROOT/tools/xrctl.py" doctor "$@"
