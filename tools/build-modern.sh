#!/bin/bash
# Build directly for the simulator instead of relabeling macOS object files.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ -n "${XCODE_BETA_PATH:-}" && -z "${DEVELOPER_DIR:-}" ]]; then
    export DEVELOPER_DIR="$XCODE_BETA_PATH/Contents/Developer"
fi
DEV="${DEVELOPER_DIR:-$(xcode-select -p)}"
OUT="$ROOT/build/modern"
SDK="$(xcrun --sdk xrsimulator --show-sdk-path)"
TARGET="arm64-apple-xros$(xcrun --sdk xrsimulator --show-sdk-version)-simulator"
DEPS="${XR_DEPS_DIR:-$ROOT/build/deps}"
VULKAN_HEADERS="${VULKAN_HEADERS:-$DEPS/Vulkan-Headers/include}"
LIBUSB_HEADERS="${LIBUSB_HEADERS:-$DEPS/libusb/libusb}"
RUNTIME_BUNDLE="${XR_RUNTIME_BUNDLE:-$DEPS/runtime}"
mkdir -p "$OUT/include" "$OUT/simui" "$OUT/bundle" "$ROOT/build/module-cache"
ln -sfn "$ROOT/openxr_headers" "$OUT/include/openxr"

build_simui() {
    local simui="$OUT/simui"
    xcrun swiftc -emit-library -emit-module -module-name SimulatorKit \
        -I simui/CoreSimulator -target arm64-apple-macosx14.0 \
        -module-cache-path "$ROOT/build/module-cache" simui/SimulatorKit/SimulatorKit.swift \
        -o "$simui/libSimulatorKit.dylib" -emit-module-path "$simui/SimulatorKit.swiftmodule"
    xcrun clang -c -fobjc-arc -fmodules -fmodules-cache-path="$ROOT/build/module-cache" \
        -target arm64-apple-macosx14.0 simui/simui_src/SimulatorObjC.m -o "$simui/SimulatorObjC.o"
    xcrun swiftc -emit-library -module-name XRGyroControls \
        -I "$simui" -I simui/CoreSimulator -L "$simui" -lSimulatorKit \
        -import-objc-header simui/simui_src/simui-Bridging-Header.h \
        -target arm64-apple-macosx14.0 -module-cache-path "$ROOT/build/module-cache" \
        simui/simui_src/IndigoHID.swift simui/simui_src/SimulatorSupport.swift \
        "$simui/SimulatorObjC.o" /Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/CoreSimulator \
        -framework Carbon -o "$simui/XRGyroControls"
    cp -R XRGyroControls.simdeviceui "$OUT/bundle/"
    mkdir -p "$OUT/bundle/XRGyroControls.simdeviceui/Contents/MacOS"
    cp "$simui/XRGyroControls" "$OUT/bundle/XRGyroControls.simdeviceui/Contents/MacOS/"
    xcrun install_name_tool -change "$simui/libSimulatorKit.dylib" \
        '@rpath/SimulatorKit.framework/Versions/A/SimulatorKit' \
        "$OUT/bundle/XRGyroControls.simdeviceui/Contents/MacOS/XRGyroControls"
    codesign -s - --force "$OUT/bundle/XRGyroControls.simdeviceui"
    # Standalone input avoids installing an unsigned plugin into Xcode.
    # Swift requires the top-level executable source to be named main.swift.
    cp tools/input_main.swift "$simui/main.swift"
    xcrun swiftc -emit-executable -module-name XRInput \
        -I "$simui" -I simui/CoreSimulator -L "$simui" -lSimulatorKit \
        -import-objc-header simui/simui_src/simui-Bridging-Header.h \
        -target arm64-apple-macosx14.0 -module-cache-path "$ROOT/build/module-cache" \
        simui/simui_src/IndigoHID.swift simui/simui_src/SimulatorSupport.swift "$simui/main.swift" \
        "$simui/SimulatorObjC.o" /Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/CoreSimulator \
        -framework Carbon -o "$OUT/bundle/xr-input"
    xcrun install_name_tool -change "$simui/libSimulatorKit.dylib" \
        "$DEV/../SharedFrameworks/SimulatorKit.framework/Versions/A/SimulatorKit" "$OUT/bundle/xr-input"
    codesign -s - --force "$OUT/bundle/xr-input"
}

if [[ "${1:-}" == "--simui-only" ]]; then
    build_simui
    exit
fi
if [[ ! -f "$VULKAN_HEADERS/vulkan/vulkan.h" || ! -f "$LIBUSB_HEADERS/libusb.h" || ! -f "$RUNTIME_BUNDLE/libopenxr_monado.dylib" ]]; then
    echo 'Dependencies missing. Run tools/fetch-build-deps.sh, or set VULKAN_HEADERS, LIBUSB_HEADERS and XR_RUNTIME_BUNDLE.' >&2
    exit 1
fi
for source in openxr_src/*.cpp; do
    xcrun clang++ -c "$source" -o "$OUT/$(basename "$source" .cpp).o" \
        -std=c++20 -target "$TARGET" -isysroot "$SDK" -I "$OUT/include" \
        -I external/glm/include -I "$VULKAN_HEADERS" -I "$LIBUSB_HEADERS" \
        -Wno-format -Wno-deprecated-declarations
done
xcrun clang -c visionos_stereo_screenshots.m -o "$OUT/bridge.o" \
    -fobjc-arc -fmodules -fmodules-cache-path="$ROOT/build/module-cache" \
    -target "$TARGET" -isysroot "$SDK" -Wno-deprecated-declarations

# Reuse the upstream transport as a baseline. Its compatibility with new
# headset firmware must be verified separately; a successful link is not proof.
for library in "$RUNTIME_BUNDLE"/*.dylib; do
    [[ "$(basename "$library")" == "libSim2OpenXR.dylib" ]] && continue
    cp -f "$library" "$OUT/bundle/"
    chmod u+w "$OUT/bundle/$(basename "$library")"
done
# v0.7 MoltenVK incorrectly retains an iOS build command alongside the
# visionOS simulator command. Modern ld rejects that mixed platform image.
if xcrun vtool -show-build "$OUT/bundle/libMoltenVK.dylib" | grep -q 'platform IOS$'; then
    xcrun vtool -remove-build-version ios -output "$OUT/bundle/libMoltenVK.dylib" "$OUT/bundle/libMoltenVK.dylib"
fi
xcrun clang++ -dynamiclib "$OUT"/*.o -o "$OUT/bundle/libSim2OpenXR.dylib" \
    -target "$TARGET" -isysroot "$SDK" -L "$OUT/bundle" \
    -lopenxr_loader -lopenxr_monado -lMoltenVK -lusb-1.0.0 \
    -framework Foundation -framework Metal -framework CompositorServices \
    -Wl,-install_name,@rpath/libSim2OpenXR.dylib
# Pin loader-relative dependencies; the bundle must work after relocation.
python3 - "$OUT/bundle" <<'PY'
from pathlib import Path
import subprocess, sys
bundle = Path(sys.argv[1])
for library in bundle.glob('*.dylib'):
    lines = subprocess.check_output(['otool', '-L', str(library)], text=True).splitlines()[1:]
    for line in lines:
        dependency = line.strip().split(' (', 1)[0]
        name = Path(dependency).name
        if (bundle / name).is_file():
            subprocess.run(['install_name_tool', '-change', dependency, '@loader_path/' + name, str(library)], check=True)
    subprocess.run(['codesign', '-s', '-', '--force', str(library)], check=True)
PY
build_simui
xcrun clang tools/probe_runtime.c -I "$OUT/include" -target "$TARGET" -isysroot "$SDK" \
    -L "$OUT/bundle" -lopenxr_loader -Wl,-rpath,@executable_path -o "$OUT/bundle/xr-probe"
xcrun install_name_tool -change '@rpath/libopenxr_loader.dylib' '@executable_path/libopenxr_loader.dylib' "$OUT/bundle/xr-probe"
codesign -s - --force "$OUT/bundle/xr-probe"
xcrun clang tools/probe_usb.c -I "$LIBUSB_HEADERS" -target "$TARGET" -isysroot "$SDK" \
    -L "$OUT/bundle" -lusb-1.0.0 -Wl,-rpath,@executable_path -o "$OUT/bundle/xr-usb-probe"
xcrun install_name_tool -change '@rpath/libusb-1.0.0.dylib' '@executable_path/libusb-1.0.0.dylib' "$OUT/bundle/xr-usb-probe"
codesign -s - --force "$OUT/bundle/xr-usb-probe"
echo "Built: $OUT/bundle (hardware streaming is not implied by this build)."
