#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${XR_DEPS_DIR:-$ROOT/build/deps}"
mkdir -p "$DEPS"
checkout() {
    local name="$1" url="$2" revision="$3"
    if [[ ! -d "$DEPS/$name/.git" ]]; then
        git init "$DEPS/$name"
        git -C "$DEPS/$name" remote add origin "$url"
    fi
    git -C "$DEPS/$name" fetch --depth 1 origin "$revision"
    git -C "$DEPS/$name" checkout --detach "$revision"
}
checkout Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git 73814d006638aa00bd553215b054f0f02e457e22
checkout libusb https://github.com/libusb/libusb.git 6a1aff0b39c724657fd92b2d2bf82eb532344570
ARCHIVE="$DEPS/XRGyroControls_OpenXR_v0.7.zip"
if [[ ! -f "$ARCHIVE" ]]; then
    curl --fail --location --output "$ARCHIVE" https://github.com/shinyquagsire23/XRGyroControls_OpenXR/releases/download/v0.7/XRGyroControls_OpenXR_v0.7.zip
fi
echo "0237d1c78105f9ff30886eaa60ff543ac75c9aa24e162622592468358691ff21  $ARCHIVE" | shasum -a 256 --check
mkdir -p "$DEPS/runtime"
unzip -oq "$ARCHIVE" -d "$DEPS/runtime"
echo "Build inputs prepared in $DEPS. The v0.7 transport is a legacy binary dependency."
