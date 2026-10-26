#!/usr/bin/env python3
"""Build the pinned patched libusb simulator transport (Apple Silicon/Xcode 27)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

REVISION = "8450cc93f6c8747a36a9ee246708bf650bb762a8"

def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    revision = run("git", "-C", source, "rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
    if revision != REVISION:
        raise RuntimeError("Use pinned libusb revision " + REVISION)
    if "quest3_verified_legacy_function" not in (source / "libusb/os/darwin_usb.c").read_text():
        raise RuntimeError("Apply patches/libusb-quest3-host-abi.patch first")
    env = dict(os.environ, lt_cv_sys_max_cmd_len="196608")
    # Autoconf's Darwin sysctl probe can be denied in a sandbox. A conservative
    # explicit maximum prevents malformed libtool object batching in that case.
    run("./bootstrap.sh", cwd=source, env=env)
    run("./configure", "--disable-static", "--enable-shared", cwd=source, env=env)
    run("make", "-j", args.jobs, cwd=source, env=env)
    output.mkdir(parents=True, exist_ok=True)
    library = output / "libusb-1.0.0.dylib"
    shutil.copy2(source / "libusb/.libs/libusb-1.0.0.dylib", library)
    run("xcrun", "vtool", "-remove-build-version", "macos", "-output", library, library)
    run("xcrun", "vtool", "-set-build-version", "12", "27.0", "27.0", "-replace", "-output", library, library)
    run("install_name_tool", "-id", "@rpath/" + library.name, library)
    for framework in ("IOKit", "CoreFoundation", "Security"):
        run("install_name_tool", "-change",
            f"/System/Library/Frameworks/{framework}.framework/Versions/A/{framework}",
            f"/System/Library/Frameworks/{framework}.framework/{framework}", library)
    run("codesign", "-s", "-", "--force", library)
    run("codesign", "--verify", "--strict", library)
    print("Built", library)
    print("Opt in with LIBUSB_QUEST3_HOST_ABI=1 only after verifying the bundled IOUSBLib shim.")

if __name__ == "__main__":
    main()
