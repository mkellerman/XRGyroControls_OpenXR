# Rebuilding the Quest 3 transport

The 2023 Monado transport's USB probe table and driver switch do not recognize
VID `0x2833` / PID `0x5013`, observed on the connected current Quest 3. Apply
`patches/monado-quest3-usb.patch` before building. The patch adds the new ID without
removing any existing device IDs.

## Source inputs

- Monado: `https://github.com/shinyquagsire23/monado.git`, revision
  `41abf1e75d443e40c01e3a844d2cdb198d84ded7` (`macos_ql_merge`).
- Eigen 3.4.0: `https://gitlab.com/libeigen/eigen.git`, revision
  `3147391d946bb4b6c68edd901f2add6ac1f31f8c`. The legacy FindEigen3 module cannot
  parse the current Homebrew Eigen 5 version headers.
- Vulkan-Headers: revision `73814d006638aa00bd553215b054f0f02e457e22` from
  `https://github.com/KhronosGroup/Vulkan-Headers.git`.
- The checksum-verified upstream `XRGyroControls_OpenXR_v0.7.zip` transport bundle.
  Its archive SHA-256 is
  `0237d1c78105f9ff30886eaa60ff543ac75c9aa24e162622592468358691ff21`.

## Native build dependencies

```sh
brew install cmake ninja capnp nlohmann-json glslang cjson molten-vk libusb x264 jpeg-turbo
```

Validated versions: CMake 4.4.4, Ninja 1.13.2, capnp 1.5.0_1, nlohmann-json 3.12.0,
glslang 16.6.0, cJSON 1.7.19, MoltenVK 1.4.2, libusb 1.0.30, x264 r3222,
jpeg-turbo 3.2.0. The package script currently selects capnp/KJ dylib ABI 1.5.0 and
x264 ABI 165 explicitly; adapt and revalidate those filenames before using another
ABI version.

## Prepare sources

Use any workspace directory for these checkouts. Commands below assume a `deps/`
folder beside this repository; change the paths if needed.

```sh
mkdir -p ../deps
git clone https://github.com/shinyquagsire23/monado.git ../deps/monado
git -C ../deps/monado checkout 41abf1e75d443e40c01e3a844d2cdb198d84ded7
git -C ../deps/monado apply "$PWD/patches/monado-quest3-usb.patch"
git clone https://gitlab.com/libeigen/eigen.git ../deps/eigen3
git -C ../deps/eigen3 checkout 3147391d946bb4b6c68edd901f2add6ac1f31f8c
git clone https://github.com/KhronosGroup/Vulkan-Headers.git ../deps/Vulkan-Headers
git -C ../deps/Vulkan-Headers checkout 73814d006638aa00bd553215b054f0f02e457e22
```

Obtain/extract the verified upstream runtime using this repository's existing
`tools/fetch-build-deps.sh`. Then invoke the configurable package script:

```sh
python3 tools/build-quest3-runtime.py \
  --source ../deps/monado \
  --legacy-runtime build/deps/runtime \
  --vulkan-headers ../deps/Vulkan-Headers/include \
  --eigen ../deps/eigen3 \
  --output build/quest3-runtime
XR_RUNTIME_BUNDLE="$PWD/build/quest3-runtime" ./build.sh
```

`--brew-prefix` supports a nondefault Homebrew location; otherwise `brew --prefix`
is used. `--jobs` defaults to 8.

## What the package script does

1. Checks the Monado and Eigen source revisions and that the USB-ID patch exists.
2. Makes a link-only copy of the upstream simulator-patched libusb. Its platform
   metadata is relabeled to macOS and its invalid zero-fill `__thread_bss` section
   offset is normalized. This preserves the runtime's libusb compatibility version
   **4**, matching the original USB shim, instead of requiring Homebrew libusb's
   compatibility version 7. The original runtime input is not modified.
3. Configures the minimal native macOS Monado runtime, with Quest Link, Vulkan and
   x264 enabled and the simulated driver, service, GUI, IPC, X11/XCB, SDL, other hardware drivers and
   OpenCV disabled. It builds `openxr_monado` with the current compiler.
4. Copies the original simulator libraries, replaces Monado with the source build,
   and adds capnp 1.5.0, KJ 1.5.0 and x264 ABI 165 libraries. The original patched
   USB library and the original MoltenVK simulator backend are retained.
5. Relabels newly built native runtime/dependency dylibs to **visionOS Simulator
   27.0**, relocates bundled dependencies to `@loader_path`, repairs the legacy
   MoltenVK image's extra iOS platform metadata, and ad-hoc signs/verifies each
   output library.

The output folder contains transport libraries only, not `libSim2OpenXR.dylib`;
use it as `XR_RUNTIME_BUNDLE` when building the bridge. No simulator or physical
headset operations are performed by this build script.

## Validation performed

- All 259 native compilation/link steps completed successfully.
- The package script was run end-to-end into a second clean output folder and
  completed with exit code 0.
- The packaged Monado, capnp and KJ images pass `codesign --verify --strict`.
- The packaged Monado's Vulkan and libusb imports are all exported by the retained
  MoltenVK and USB shim libraries respectively.
- Packaged runtime dependency paths are local to the bundle, excluding Apple
  system libraries.
- Exported OpenXR negotiation and optional Quest eye-sidechannel symbols exist.

These are build/static-package checks. The parent bridge test must separately
verify that `xrGetSystemProperties().systemName` names the real Quest, followed by
sustained video and input. Monado's `Simulated HMD` is not evidence of a physical
connection, even when `xrCreateInstance` and `xrGetSystem` succeed.

The package script disables `XRT_BUILD_DRIVER_SIMULATED` to make missing USB hardware fail without creating a fake HMD or initializing its GPU compositor. This change was rebuilt successfully into `quest3-runtime-nosim` during diagnosis.

## Current host USB ABI correction

See `QUEST3_USB_ABI.md` for the libusb revision, patch, build script and opt-in instructions. Build with `python3 tools/build-quest3-libusb.py --source ../deps/libusb --output build/quest3-usb`, then copy its signed `libusb-1.0.0.dylib` into the custom runtime. Export `LIBUSB_QUEST3_HOST_ABI=1` in the injected simulator process, or `SIMCTL_CHILD_LIBUSB_QUEST3_HOST_ABI=1` for a simctl-spawned probe. Enumeration has been verified against the actual Quest 3: 18-byte device descriptor, VID2833/PID5013, real Quest Link HMD/controllers found. Asynchronous bulk transport and sustained video remain pending hardware validation. The fallback verifies and caches the exact legacy shim image SHA per image base/path, and retries only invalid-argument descriptor/control-IN/bulk-IN requests.
