#!/bin/bash
# Fast compositor-only rebuild; use build.sh after changing C++ or input code.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ -n "${XCODE_BETA_PATH:-}" && -z "${DEVELOPER_DIR:-}" ]]; then
    export DEVELOPER_DIR="$XCODE_BETA_PATH/Contents/Developer"
fi
SDK="$(xcrun --sdk xrsimulator --show-sdk-path)"
TARGET="arm64-apple-xros$(xcrun --sdk xrsimulator --show-sdk-version)-simulator"
OUT="$ROOT/build/modern"
xcrun clang -c visionos_stereo_screenshots.m -o "$OUT/bridge.o" \
    -fobjc-arc -fmodules -fmodules-cache-path="$ROOT/build/module-cache" \
    -target "$TARGET" -isysroot "$SDK" -Wno-deprecated-declarations
xcrun clang++ -dynamiclib "$OUT"/*.o -o "$OUT/bundle/libSim2OpenXR.next.dylib" \
    -target "$TARGET" -isysroot "$SDK" -L "$OUT/bundle" \
    -lopenxr_loader -lopenxr_monado -lMoltenVK -lusb-1.0.0 \
    -framework Foundation -framework Metal -framework CompositorServices \
    -Wl,-install_name,@rpath/libSim2OpenXR.dylib
for name in libopenxr_loader.dylib libopenxr_monado.dylib libMoltenVK.dylib libusb-1.0.0.dylib; do
    xcrun install_name_tool -change "@rpath/$name" "@loader_path/$name" "$OUT/bundle/libSim2OpenXR.next.dylib"
done
codesign -s - --force "$OUT/bundle/libSim2OpenXR.next.dylib"
mv "$OUT/bundle/libSim2OpenXR.next.dylib" "$OUT/bundle/libSim2OpenXR.dylib"
