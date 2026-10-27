# Quest 3 simulator USB ABI compatibility

**Streaming remains quarantined.** Reconnecting the Quest after early tests caused a host kernel panic in `IOUSBHostFamily` / `AppleUSBCommon`; its exact cause remains unproven. Isolated diagnostics resumed after cleanup repairs and now verify descriptor completion. They require two explicit diagnostic flags. Prior experimental binaries must not be reused. See [the validation record](VALIDATION.md).

Host enumeration with Homebrew libusb succeeds, while the original simulator
shim returns zero devices. `LIBUSB_DEBUG=4` isolates the failure to a descriptor
request returning `kIOReturnBadArgument` (`0xe00002c2`). It is not a PID match or
missing factory problem.

The original arm64 `IOUSBLib_ios_hax.dylib` sends descriptor selector 7 through
`IOConnectCallMethod`, with a structure output and one scalar output. The current
host IOUSBLib sends the same eight scalar inputs through
`IOConnectCallScalarMethod`, expecting two scalar outputs. The data pointer and
length are already scalar inputs. The old simulator request shape fails against
the current host kernel. Host library inspection also identifies one scalar
output required by bulk IN asynchronous selector 6, and two outputs for control
IN asynchronous selector 7. The old simulator shim omits or undersizes these.

The checked compatibility patch retries only after `kIOReturnBadArgument` and
only when both `LIBUSB_QUEST3_HOST_ABI=1` and `LIBUSB_QUEST3_DIAGNOSTIC=1` are set. It verifies the
architecture and exact SHA-256 of the image owning the legacy interface method
before using the observed object layout to recover the kernel connection and
notification port. It does not alter the shim or redistribute a current private
host library. It does not change SIP or host security settings.

The main test observed successful descriptor retrieval after this correction:
result 0, output count 2, status 0, byte count 18, and one device `2833:5013`.
The real Quest Link HMD and controllers were subsequently found. This establishes
enumeration. After repairing notification-source removal to use CommonModes,
an isolated simulator control test also completed both string requests: 4 bytes
for language, 30 bytes for the serial descriptor including its header, and
14 bytes after ASCII conversion. A native macOS baseline returned the same
14-byte length. The diagnostic closes its handle after completion and does not
claim the streaming interface or print the serial value. ADB remained responsive.

The repaired Monado handshake exchanged pairing messages before waiting for
user acceptance and timing out normally. Full OpenXR identity, sustained video,
tracking and safe reconnect remain unverified. Callback ABI is unchanged;
extra immediate output scalars do not justify changing callback arguments.

## Reproduce the source build

Prerequisites: Apple Silicon, Xcode 27, Homebrew `autoconf automake libtool`.

```sh
git clone https://github.com/libusb/libusb.git ../deps/libusb
git -C ../deps/libusb checkout 8450cc93f6c8747a36a9ee246708bf650bb762a8
git -C ../deps/libusb apply "$PWD/patches/libusb-quest3-host-abi.patch"
python3 tools/build-quest3-libusb.py --source ../deps/libusb --output build/quest3-usb
cp build/quest3-usb/libusb-1.0.0.dylib build/quest3-runtime/
```

The patch contains the original project's libusb simulator changes as well as
the current host ABI fallback; apply it to the clean pinned revision. The script
sets a conservative libtool command maximum because sandboxed `sysctl kern.argmax`
can fail during configure. It builds normally with `make`, relabels the result
for visionOS Simulator 27, adjusts framework install names, and signs/verifies it.

For isolated simulator diagnostics, export both child environment flags shown
below. Full bridge injection remains blocked by the launcher.

After rebuilding the bridge with the patched runtime, run the enumeration-only
diagnostic without starting Link or injecting the compositor. Add `--serial`
after the bundle argument to check string-transfer completion too:

```sh
SIMCTL_CHILD_LIBUSB_QUEST3_HOST_ABI=1 SIMCTL_CHILD_LIBUSB_QUEST3_DIAGNOSTIC=1 \
  xcrun simctl spawn "$SIMULATOR_UDID" \
  "$PWD/build/modern/bundle/xr-usb-probe" "$PWD/build/modern/bundle"
```

Expect `Meta USB 2833:5013` on the tested Quest 3. `xr-probe` accepts the same
bundle argument and attempts an OpenXR handshake. The patched driver allows
60 seconds for pairing, then joins workers and cleans up normally on failure.
There is no diagnostic alarm that kills a process with USB work pending.

Accepted IOUSBLib image hashes are the upstream release image
`2432fc490050e081404494acceef71569427dc10f52e0b84fa42046e747eebfc`
and its packaged, signed image
`f72c10d2a59a6474d8e02bb5f54e7d1bf6fc63ad849202f9161c346d98b04fac`.
Any other image is refused. New OS releases or shim changes require reinspection
and validation, rather than adding another hash without checking its layout.
