#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
./build.sh
./inject.sh
exec python3 tools/xrctl.py input
