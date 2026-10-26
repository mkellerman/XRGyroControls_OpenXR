# Quest 3 simulator USB ABI compatibility

**QUARANTINED — do not execute the historical procedure below.** After these tests, reconnecting the Quest caused a host kernel panic with a tag-check fault in `IOUSBHostFamily` / `AppleUSBCommon`. The precise cause is not established. Successful request submission did not establish correct completion or teardown. The ABI fallback now always refuses execution, regardless of its environment variable. The launcher and diagnostics also block live testing. Prior experimental binaries must not be reused.

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
only when explicitly enabled with `LIBUSB_QUEST3_HOST_ABI=1`. It verifies the
architecture and exact SHA-256 of the image owning the legacy interface method
before using the observed object layout to recover the kernel connection and
notification port. It does not alter the shim or redistribute a current private
host library. It does not change SIP or host security settings.

The main test observed successful descriptor retrieval after this correction:
result 0, output count 2, status 0, byte count 18, and one device `2833:5013`.
The real Quest Link HMD and controllers were subsequently found. This establishes
enumeration. A later bounded probe also observed control IN and bulk IN request
submission return success. The serial descriptor still failed to complete, and
the Link application was blocked by a headset system dialog. Sustained video and
asynchronous transfer completion remain unverified.

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

For a simulator command, export the opt-in with
`SIMCTL_CHILD_LIBUSB_QUEST3_HOST_ABI=1` before `xcrun simctl spawn`. The bridge's
launch environment must likewise contain `LIBUSB_QUEST3_HOST_ABI=1`.
The launcher forwards this exact opt-in when invoked as
`LIBUSB_QUEST3_HOST_ABI=1 ./inject.sh`; it is disabled by default.

After rebuilding the bridge with the patched runtime, run the enumeration-only
diagnostic without starting Link or injecting the compositor:

```sh
SIMCTL_CHILD_LIBUSB_QUEST3_HOST_ABI=1 xcrun simctl spawn "$SIMULATOR_UDID" \
  "$PWD/build/modern/bundle/xr-usb-probe" "$PWD/build/modern/bundle"
```

Expect `Meta USB 2833:5013` on the tested Quest 3. `xr-probe` accepts the same
bundle argument and attempts an OpenXR handshake, with a 30-second timeout.

Accepted IOUSBLib image hashes are the upstream release image
`2432fc490050e081404494acceef71569427dc10f52e0b84fa42046e747eebfc`
and its packaged, signed image
`f72c10d2a59a6474d8e02bb5f54e7d1bf6fc63ad849202f9161c346d98b04fac`.
Any other image is refused. New OS releases or shim changes require reinspection
and validation, rather than adding another hash without checking its layout.
