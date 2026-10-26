#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
if [[ "${1:-}" == "--legacy-plugin" ]]; then
    shift
    exec python3 "$ROOT/tools/install_plugin.py" uninstall "$@"
fi
echo "Standalone mode does not modify Xcode. Stop xr-input with Ctrl-C and shut down the selected simulator to unload the bridge."
