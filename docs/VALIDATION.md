# Validation record

Session: 2026-10-26. Apple Silicon Mac, macOS 27.2, Xcode 27.1 (27A9275), visionOS 27.0 simulator (24M362), USB-connected Quest 3.

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
