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

### Successful physical pairing retry

With the keep-awake override and scrcpy active, a discovery retry exposed an alternate Quest USB ID: `2833:5012`. Native descriptor inspection confirmed its XRSP interface `ff/8a/01`, plus its ADB interface. The prober table and dispatch now recognize both `5012` and the earlier `5013` mode.

The successful sequence matched the original project's order: start the host runtime with the Quest Link app stopped, wait for the first-stage `Waiting for user to accept` log, then launch Quest Link. It completed the second invite/code-generation/pairing exchange, logged `Meta Quest Link driver ready`, and returned:

```text
OpenXR system: Monado: Meta Quest Link
Physical-device discovery passed. Video and input are not tested by this probe.
```

The probe exited with status 0, logged both worker exits and left ADB responsive. This verifies real physical OpenXR pairing/discovery; it does not verify stereo video, controller input, head tracking, or safe reconnect. No claim is made that the earlier kernel panic is fixed. The default bridge injector remains blocked pending a separately verified streaming startup path.

The earlier app-first tests timed out. Host-first is the observed working sequence; a controlled comparison isolating startup order from the alternate USB mode was not performed.

Offline checks:

```sh
python3 tools/check-transport-lifecycle.py --source ../monado
python3 tools/check-usb-notifications.py --source ../libusb-quest3
python3 tools/check-usb-serial-decoder.py --source ../libusb-quest3
```

## Verified

### Streaming investigation — 2026-10-27

#### Isolating the initial USB timeout

The same previously successful standalone pairing probe reproduced the initial
write timeout outside the injected compositor: both host-first and Link-first
attempts had 20 failed writes, never reached first-stage pairing, exited with
status 1 and performed normal 60-second cleanup. The driver printed packet
lengths with `%x`: its old `400 bytes` message means **0x400 / 1024 bytes**, not
400 decimal bytes. Diagnostics now print decimal lengths and the selected USB
interface/endpoints explicitly.

Quest-side logs simultaneously showed `xrspd` repeatedly failing to configure
`/dev/usb-ffs/xrsp/ep0`; descriptor writes failed with `No such process`, then
exhausted 40 retries. Restarting the Link activity alone did not recover it.
After a normal Quest reboot, the unchanged host-first probe reached
`Waiting for user to accept` with **zero initial-write timeouts**. Thus clearing
the headset's failed endpoint state recovered the initial exchange; no host USB
reset or new ABI change was needed. The second pairing stage still timed out;
a headset capture showed its `Finding position in room` tracking prompt, so
that run does not establish completed pairing or video. Keep-awake and scrcpy
were restored after reboot.

This is an observed recovery for this failure state, not proof of what originally
caused the headset daemon to fail or a guarantee against recurrence. Raw headset
captures and logs remain local. If the initial timeout recurs, inspect current
headset `xrspd` logs before changing host USB code; do not infer a daemon failure
from packet size alone.

#### Pairing activity sequencing

After tracking recovered, the injected runtime opened interface 0 with bulk IN
`0x81` / OUT `0x01` at 5 Gbit/s and no initial-write timeout. Starting Link manually
then triggered the missing first-stage invitation shortly before the driver's
60-second deadline. The runtime now signals `usb-ready` once its interface is
open, then `ready` when the first handshake finishes. The launcher can start the
activity at both stages rather than waiting for an invitation that needs the
activity to start first.

The first trial of that sequence force-stopped Link between the two activity
starts. The headset returned to Home and displayed its cable-reconnect request;
its activity log shows Link's own disconnect dialog at the handoff. That timing
makes forced termination a plausible cause. The launcher now sends intents
without force-stopping Link, including at initial injection. A regression test
reproduced the old force-stop between the two starts and passes with its removal;
all nine launcher tests pass. A subsequent fresh-headset trial completed both
handshakes without a forced activity restart and reached `OpenXR init success`.
That trial saw the first-handshake marker immediately, so the fallback's two-intent
sequence still needs its own completed hardware trial.

Lightning Launcher and Evolve Launcher were installed during the investigation.
At the read-only check, Meta Home was the selected default and the enabled
accessibility-service list was empty. No captured activity evidence attributed
the disconnect to either third-party launcher. Those observations do not establish
their state throughout earlier attempts.

#### Capture and startup checks

At 10:32 local time, the injected bridge completed both physical Quest pairing
stages, created its Vulkan device, headset and renderer, and logged
`OpenXR init success` / `Background OpenXR startup ready`. The user confirmed that
moving their head moved the view in Vision Pro Device Hub. The shared input
buffer independently contained a normalized orientation quaternion. This verifies
physical head-tracking delivery to the simulator for that session; controller
selection, video, stereo and reconnect robustness are not verified.

The headset remained at the Meta logo. Runtime frame-pacing logs reported zero
video encode/transmit time, and a process sample showed pose processing but no
video-submission worker. A legacy capture gate compared cached drawable identity
with the presented wrapper. The next build prepares capture from the current
presentation drawable, removes that identity gate and snapshots the frame's pose
index for its GPU callback. It logs the first queued copy, GPU completion and
OpenXR submission separately. The immediate reconnect retry consumed stale
session traffic and timed out with normal cleanup. After a cable reconnect,
the 10:41 trial paired again, completed its first copy with Metal status 4 and no
error, submitted video frames and reported nonzero encoding/transmission time.
A headset capture showed the simulator room, and the user confirmed a live room
following headset movement. Functional mono video/head tracking are established
for that trial, but image quality is not: the user reported misaligned eye images
and flashing, and the runtime logged out-of-order frame submissions.

The next candidate addresses two offline-reproduced errors. The real pose loop,
run with a fake headset, began 83 frames before its first completion; the updated
loop waits for submission completion and its check permits exactly one next frame.
The real `Headset::endFrame` API boundary submitted a duplicated mono image with
different per-eye camera poses/FOV; the updated composition copies the native
simulator FOV and one camera pose into both submitted views while preserving
array indices and original tracking data. Both checks now pass. The candidate
also admits one capture per fresh pose, snapshots callback pose indices and
serializes pose extraction. It builds in `build/frame-order/bundle` without
overwriting the running bundle. Hardware confirmation of the alignment/flicker
corrections and controller selection remain pending.

- Current native simulator drawable observed: one view, one BGRA8 sRGB array texture, 3840×2160. Apple's public API documents one-view Simulator rendering. Experimental capture now preserves native views and copies slice zero into a bridge-owned three-slot 2D ring; both Quest eyes receive the same image. Stereo output is not established.
- The previous startup gate required three different native texture identities. The current simulator reused one identity in observed startup calls, so that gate never initialized OpenXR. The bridge-owned ring removes that assumption.
- Both participants unconditionally called `ftruncate` on an existing POSIX shared-memory buffer. Darwin rejects the second truncate with `EINVAL`. Size-aware reuse fixes the observed early initialization failure. `tests/sharedmem_reopen.c` passes on this Mac: a child opens and reads the parent's buffer, the legacy second truncate reproduces `EINVAL`, and incompatible sizes are rejected.
- A subsequent synchronous initialization attempt reached physical USB communication but repeatedly timed out sending the initial 400-byte packet. RealityEnvironment was terminated by its 30-second process-launch watchdog (`FRONTBOARD`, `0x8BADF00D`); this is separate from the earlier host kernel panic. Blocking the compositor during USB initialization is a plausible cause, not a proven complete explanation.
- Experimental startup now dispatches OpenXR initialization to a background queue and pose reads return when no pose is available. One subsequent attempt reached the same USB timeout, returned `XR_ERROR_INITIALIZATION_FAILED` after the driver's 60-second bound, and logged `Background OpenXR startup failed`. No additional RealityEnvironment crash report appeared during that attempt. This does not establish sustained rendering or a completed watchdog fix.
- Eight launcher tests pass, including fresh pairing-marker ordering and stale-marker rejection. The full current SDK build passes. Default injection remains guarded; explicit diagnostic and host-ABI opt-ins enable the experimental streaming attempt.
- Live Quest video, head tracking and controller selection remain unverified. Research and available upstream workarounds are recorded in [UPSTREAM_RESEARCH.md](UPSTREAM_RESEARCH.md). Raw local crash reports were not published.

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
