# XRGyroControls_OpenXR — current Xcode / Quest 3 port

**Not a working streaming release: a Mac kernel panic occurred on Quest USB reconnection after experimental transport tests.** Its exact cause remains unproven. Full bridge injection remains disabled. After cleanup repairs, native/simulator control reads and real Quest OpenXR pairing/discovery now pass. The successful diagnostic initializes the host transport before launching Quest Link and recognizes both USB modes `5012` and `5013`. Simulator diagnostics require `LIBUSB_QUEST3_DIAGNOSTIC=1` and `LIBUSB_QUEST3_HOST_ABI=1`. Video, tracking/input and safe reconnect remain unverified. The live-streaming commands below remain historical.

Experimental work on streaming Apple's visionOS Simulator from an Apple Silicon Mac to a USB-connected Quest 3. visionOS runs on the Mac. Based on [Max Thomas's original project](https://github.com/shinyquagsire23/XRGyroControls_OpenXR).

**Work in progress. This is not yet a verified working release.** Current source builds with Xcode 27.1 / visionOS 27 SDK and loads in visionOS 27.0 with SIP enabled. Sustained Quest video and input have not passed end-to-end testing. See [validation notes](docs/VALIDATION.md).

## Changes

- Discover the selected Xcode and available visionOS simulators, including Xcode 27's Device Hub layout.
- Select one USB Quest explicitly, ignoring TVs, phones and wireless ADB devices.
- Build natively for visionOS Simulator with pinned dependency inputs, rather than relabeling macOS object files.
- Use checked Objective-C HID methods from a standalone input helper instead of guessed Swift vtable offsets. The default path leaves Xcode's plugins, device profiles and security settings unchanged.
- Reject Monado's simulated fallback instead of reporting it as a Quest connection. The verified diagnostic sequence initializes host USB pairing first, then launches Quest Link.
- Handle modern drawable acquisition and texture metadata, preserve opaque view state, and use the actual Metal pixel format for Vulkan imports.
- Treat Quest Pro eye tracking as optional; Quest 3 has no eye tracker.

## Build

Requirements: Apple Silicon, Xcode with visionOS SDK/runtime, Python 3, Git, curl, unzip, and Android platform tools (`adb`). The current build was checked with Xcode 27.1 and targets the selected visionOS Simulator SDK version. Other SDK/runtime versions have not been tested.

```sh
bash tools/fetch-build-deps.sh
./build.sh
python3 tools/xrctl.py doctor
```

Dependencies stay in `build/deps`. The initial transport baseline is the checksum-pinned upstream v0.7 binary bundle. That old runtime does not recognize current Quest 3 USB PID `0x5013`. The [transport build recipe](docs/QUEST3_RUNTIME_BUILD.md) adds the ID and disables simulated fallback. The [experimental USB compatibility patch](docs/QUEST3_USB_ABI.md) restores enumeration on the tested Mac and allows incoming transfer submission, but transfer completion and physical streaming remain unverified. Building the bridge alone does not resolve the transport changes.

For custom build inputs, set `VULKAN_HEADERS` (containing `vulkan/`), `LIBUSB_HEADERS` (containing `libusb.h`) and `XR_RUNTIME_BUNDLE` (containing the simulator transport dylibs). `DEVELOPER_DIR` takes precedence over `XCODE_BETA_PATH`, then `xcode-select -p`.

## Live test

Connect the Quest with a USB data cable, enable Developer Mode, and accept USB debugging inside the headset.

```sh
./run.sh
./inject.sh
python3 tools/xrctl.py input
```

The input helper stays running; stop it with Ctrl-C. Select the visionOS device in Device Hub to see the Mac preview. Specify `SIMULATOR_UDID` and `ANDROID_SERIAL` (or the corresponding CLI flags) when using multiple devices.

`./build_run.sh` builds, boots, injects and runs input. `./build_inject.sh` uses an already booted simulator. `./install.sh` checks prerequisites in standalone mode; it does not install a global plugin.

A successful test requires sustained stereo video in the Quest, head movement and controller selection. An activity launch or generic OpenXR initialization message is not sufficient. Check the logged `OpenXR system` identity to rule out Monado's simulated HMD. `XRHAX_ALLOW_SIMULATED=1` is reserved for diagnostics and must never be used as hardware validation.

If ADB stalls after a transport test, reconnect only that headset's transport with `adb -s SERIAL reconnect`. If the simulator renderer crashes, stop the helper and reboot the selected simulator before retesting. Do not rebuild dependency libraries in place while a simulator has them loaded.

## Checks

```sh
python3 -m unittest discover -s tests -v
bash -n build.sh run.sh inject.sh install.sh uninstall.sh tools/*.sh
git diff --check
```

`tools/rebuild-hook.sh` is a fast compositor-only build; use the full build for C++ or input changes.

## Legacy workflow

The original instructions are archived in [docs/LEGACY.md](docs/LEGACY.md), and the old build is in `tools/build-legacy.sh`. Those beta-SDK/security instructions do not describe the standalone path above. An optional administrator plugin installation is available via `./install.sh --legacy-plugin`, with a backup/restore path through `./uninstall.sh --legacy-plugin`; this optional mode has not been validated with Device Hub.
