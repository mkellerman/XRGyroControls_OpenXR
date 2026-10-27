# Validation record

Session: 2026-10-26. Apple Silicon Mac, macOS 27.2, Xcode 27.1 (27A9275), visionOS 27.0 simulator (24M362), USB-connected Quest 3.

## Host kernel panic

After experimental USB tests, the user reported that reconnecting the Quest crashed the Mac. The local panic report confirms a kernel tag-check fault in `kernel_task`; its backtrace includes `com.apple.iokit.IOUSBHostFamily` and `com.apple.driver.usb.AppleUSBCommon`. This is a host kernel failure, not a simulator app crash. The experimental USB ABI path is a credible suspect; the report alone does not prove the exact cause.

All live testing stopped. No owned bridge/input/diagnostic processes were found running. The local bridge, patched USB library and diagnostic executables were renamed with `.quarantined` suffixes. The launcher blocks `inject` and `quest-link` before any device command, diagnostics exit before loading USB, and the source ABI fallback always returns false. Environment opt-in cannot bypass these blocks. The raw panic report remains local and was not published.

Earlier enumeration and queue-submission results below do not establish correct asynchronous completion or disconnect cleanup. Investigate those paths offline before considering further hardware testing. No working release is available.

## Resumed diagnosis — 2026-10-27

The owner requested resuming work after the panic. Full bridge injection remains disabled while isolated diagnostics proceed with an additional diagnostic opt-in.

- The actual Monado host destructor failed an offline resource-ownership harness: it closed the USB device while reader/writer workers still owned its resources. The patch stops and joins workers before freeing USB resources or device objects, guards repeated cleanup, serializes reads with handle replacement, frees configuration descriptors, and removes device-wide resets from recovery. The same harness now passes. This does not prove a kernel-panic fix.
- The handshake previously waited indefinitely. It now times out normally (60 seconds to allow headset interaction), joins workers and returns initialization failure. Diagnostic alarms that killed the process during USB work have been removed.
- The actual libusb `darwin_close` function left a CommonModes notification source attached because it removed it using DefaultMode. A real CoreFoundation run-loop harness with a fake USB boundary failed before matching the modes and passed afterward. The ABI patch includes that repair.
- Binary inspection corroborates the original three-argument callback and `arg0` byte count. No speculative callback remapping or immediate manual completion was introduced.
- A native macOS descriptor test read the 14-byte serial and closed the handle normally. The isolated simulator test then matched it: language descriptor completion returned 4 bytes, serial descriptor completion returned 30 bytes including its header, and ASCII conversion returned 14 bytes. Descriptor types/lengths were valid. Actual serial data was not printed by the diagnostic. ADB responded afterward.
- The repaired driver exchanged invite, code-generation and pairing messages with the real Quest, reaching its `Waiting for user to accept` stage. The first 20-second trial timed out, logged both worker exits and returned `XR_ERROR_INITIALIZATION_FAILED` without a forced kill. ADB remained responsive afterward. This is pairing progress, not a completed OpenXR connection.

The next test needs an operator in the headset and an active cast to observe any pairing prompt. Sustained video, tracking, full reconnect behavior and absence of kernel faults remain unverified.

Offline checks:

```sh
python3 tools/check-transport-lifecycle.py --source ../monado
python3 tools/check-usb-notifications.py --source ../libusb-quest3
python3 tools/check-usb-serial-decoder.py --source ../libusb-quest3
```

## Verified

- Native simulator bridge and standalone HID helper build and link.
- Seven launcher tests pass, including a USB Quest alongside another Android device, ambiguous Quests, unauthorized ADB, both Simulator/Device Hub app layouts and Link-before-injection ordering.
- Quest Link activity resolution and launch work on the connected Quest 3.
- Standalone HID connects to the simulator through current SimulatorKit Objective-C entry points.
- A clean simulator renders its living room and Home grid.
- The bridge loads with SIP enabled and skips the removed `getPose:atTime:` selector.
- The old third-texture preview causes a compositor bounds abort. The current layout uses two textures and copies the left eye into the Mac preview without exposing a third texture.
- Source textures are BGRA sRGB / 2D array textures; Vulkan import now uses the matching format and preserves texture usage/type/sample count.
- The current Quest exposes VID `0x2833`, PID `0x5013`, with an XRSP USB interface. The bundled 2023 runtime only recognizes the older PIDs.

## Failures and corrections

The first generic OpenXR initialization messages were misleading: inspection showed Monado selected **Simulated HMD**, not the Quest. Those runs briefly presented frames, then hit a Metal GPU timeout and a final-pass renderer abort. They are not evidence of Quest streaming.

The launcher now starts Quest Link before injecting and the bridge explicitly rejects a simulated OpenXR system by default. Monado was rebuilt with the new USB PID in both the prober table and driver dispatch, and with its simulated driver disabled. All 259 build steps completed; see [the reproducible build recipe](QUEST3_RUNTIME_BUILD.md).

A differential USB probe finds the physical Quest with native macOS libusb. Inside visionOS Simulator, the original patched libusb finds the registry entry for `2833:5013` but fails `DeviceRequestTO` with kernel error `0xe00002c2` (invalid argument) while reading the device descriptor, returning zero devices. A factory-metadata fallback made no difference.

Inspection identified changed scalar output counts in the current kernel's USB interface. An opt-in, checksum-guarded [compatibility patch](QUEST3_USB_ABI.md) successfully retrieved the 18-byte descriptor and enumerated the Quest inside the simulator. The rebuilt Monado driver then initialized the real Quest HMD, controller and hand devices and reported a 5 Gbit/s link. A further patch made control IN and bulk IN request submission return success, but the serial descriptor still failed to complete and the handshake did not finish within the diagnostic's 30-second bound. These results do not establish streaming.

The no-headset crash is fixed in the Monado patch: with simulated fallback disabled and no discovered HMD, the probe now exits with `XR_ERROR_INITIALIZATION_FAILED` rather than dereferencing a null HMD. This behavior was verified live.

The Chrome cast showed a PC Disconnected dialog. Headset logs confirmed that the system dialog blocked subsequent Link activity launches. Dismissing it is required before the next full test. Sustained video, tracking, controller selection and sleep/reconnect remain unverified.

The user reported Quest Home/a connection prompt. A live Chrome casting view is available for subsequent headset verification.

No security settings were changed. Apple's original Xcode plugin remains intact. A restorable local backup was made before an optional plugin-install attempt encountered administrator permissions; standalone mode subsequently removed the need for that installation.
