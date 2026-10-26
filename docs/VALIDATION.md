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

A differential USB probe finds the physical Quest with native macOS libusb. Inside visionOS Simulator, the original patched libusb finds the registry entry for `2833:5013` but fails `DeviceRequestTO` with kernel error `0xe00002c2` (invalid argument) while reading the device descriptor, returning zero devices. A factory-metadata fallback made no difference. With simulated fallback disabled, the legacy runtime then dereferences a missing HMD during compositor creation. Current USB user-client compatibility is under investigation. Sustained video, tracking, controller selection and sleep/reconnect remain unverified.

The user reported Quest Home/a connection prompt. A live Chrome casting view is available for subsequent headset verification.

No security settings were changed. Apple's original Xcode plugin remains intact. A restorable local backup was made before an optional plugin-install attempt encountered administrator permissions; standalone mode subsequently removed the need for that installation.
