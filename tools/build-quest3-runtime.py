#!/usr/bin/env python3
"""Build patched Monado and package its dylibs for visionOS Simulator 27.

Prerequisites: Xcode 27+, cmake, ninja, capnp, nlohmann-json, glslang, cjson,
MoltenVK, libusb headers, x264, jpeg-turbo. Uses the existing v0.7 USB shim.
Apply patches/monado-quest3-usb.patch to Monado before invoking this script.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess

MONADO_REVISION = "41abf1e75d443e40c01e3a844d2cdb198d84ded7"
EIGEN_REVISION = "3147391d946bb4b6c68edd901f2add6ac1f31f8c"

def run(*args, capture=False):
    return subprocess.run([str(x) for x in args], check=True, text=True,
                          capture_output=capture)

def copy_library(source, destination):
    # Homebrew dylibs may be read-only. Replace the output inode rather than
    # overwriting a previous signed package in place.
    temporary = destination.with_name(destination.name + ".copying")
    if temporary.exists():
        temporary.unlink()
    shutil.copy2(source, temporary)
    temporary.replace(destination)

def normalize_zerofill(path):
    # The v0.7 libusb shim has a nonzero __thread_bss file offset. Current ld
    # rejects it after relabeling to macOS; normalize only this link-time copy.
    data = bytearray(path.read_bytes())
    if struct.unpack_from("<I", data)[0] != 0xFEEDFACF:
        raise RuntimeError("Expected a thin little-endian 64-bit libusb shim")
    count = struct.unpack_from("<I", data, 16)[0]
    position = 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == 0x19:
            sections = struct.unpack_from("<I", data, position + 64)[0]
            for index in range(sections):
                section = position + 72 + index * 80
                flags = struct.unpack_from("<I", data, section + 64)[0]
                if flags & 0xFF in (1, 12, 18):
                    struct.pack_into("<I", data, section + 48, 0)
        position += size
    path.write_bytes(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--legacy-runtime", required=True, type=Path)
    parser.add_argument("--vulkan-headers", required=True, type=Path,
                        help="Directory containing vulkan/vulkan.h")
    parser.add_argument("--eigen", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--brew-prefix", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    source, legacy, vulkan, eigen, out = [p.resolve() for p in
        (args.source, args.legacy_runtime, args.vulkan_headers, args.eigen, args.output)]
    prefix = args.brew_prefix or Path(run("brew", "--prefix", capture=True).stdout.strip())
    revision = run("git", "-C", source, "rev-parse", "HEAD", capture=True).stdout.strip()
    if revision != MONADO_REVISION:
        raise RuntimeError("Use the pinned Monado revision " + MONADO_REVISION)
    eigen_revision = run("git", "-C", eigen, "rev-parse", "HEAD", capture=True).stdout.strip()
    if eigen_revision != EIGEN_REVISION:
        raise RuntimeError("Use the pinned Eigen 3.4.0 revision " + EIGEN_REVISION)
    if "QUEST_3_XRSP_ADB_PID" not in (source / "src/xrt/drivers/quest_link/ql_interface.h").read_text():
        raise RuntimeError("Apply the Quest 3 USB patch before building")
    build = source / "build-quest3"
    stub = build / "link-stubs/libusb-1.0.0.dylib"
    stub.parent.mkdir(parents=True, exist_ok=True)
    copy_library(legacy / stub.name, stub)
    run("xcrun", "vtool", "-remove-build-version", "12", "-output", stub, stub)
    run("xcrun", "vtool", "-set-build-version", "macos", "27.0", "27.0", "-replace", "-output", stub, stub)
    normalize_zerofill(stub)
    flags = {
        "CMAKE_BUILD_TYPE": "RelWithDebInfo", "CMAKE_POLICY_VERSION_MINIMUM": "3.5",
        "EIGEN3_INCLUDE_DIR": eigen, "Vulkan_INCLUDE_DIR": vulkan,
        "Vulkan_LIBRARY": prefix / "opt/molten-vk/lib/libMoltenVK.dylib",
        "LIBUSB1_LIBRARY": stub, "XRT_ENABLE_GPL": 1, "XRT_BUILD_DRIVER_QUEST_LINK": 1,
        "XRT_BUILD_DRIVER_EUROC": 0, "XRT_BUILD_DRIVER_NS": 0, "XRT_BUILD_DRIVER_PSVR": 0,
        "XRT_HAVE_OPENCV": 0, "XRT_HAVE_XCB": 0, "XRT_HAVE_XLIB": 0,
        "XRT_HAVE_XRANDR": 0, "XRT_HAVE_SDL2": 0, "XRT_HAVE_VT": 0,
        "XRT_FEATURE_WINDOW_PEEK": 0, "XRT_BUILD_DRIVER_QWERTY": 0,
        "XRT_BUILD_DRIVER_WMR": 0, "XRT_BUILD_DRIVER_SIMULATED": 0, "XRT_FEATURE_SERVICE": 0,
        "XRT_FEATURE_STEAMVR_PLUGIN": 0, "XRT_MODULE_IPC": 0,
        "BUILD_TESTING": 0, "XRT_MODULE_MONADO_GUI": 0, "XRT_MODULE_MONADO_CLI": 0,
    }
    run("cmake", "-S", source, "-B", build, "-G", "Ninja",
        *[f"-D{key}={value}" for key, value in flags.items()])
    run("cmake", "--build", build, "--target", "openxr_monado", "-j", args.jobs)
    out.mkdir(parents=True, exist_ok=True)
    for library in legacy.glob("*.dylib"):
        if library.name != "libSim2OpenXR.dylib":
            copy_library(library, out / library.name)
    runtime = build / "src/xrt/targets/openxr/libopenxr_monado.dylib"
    additions = [runtime, prefix / "opt/capnp/lib/libcapnp.1.5.0.dylib",
                 prefix / "opt/capnp/lib/libkj.1.5.0.dylib",
                 prefix / "opt/x264/lib/libx264.165.dylib"]
    for library in additions:
        destination = out / library.name
        copy_library(library, destination)
        run("xcrun", "vtool", "-remove-build-version", "macos", "-output", destination, destination)
        run("xcrun", "vtool", "-set-build-version", "12", "27.0", "27.0", "-replace", "-output", destination, destination)
        run("install_name_tool", "-id", "@rpath/" + destination.name, destination)
    molten = out / "libMoltenVK.dylib"
    run("xcrun", "vtool", "-remove-build-version", "ios", "-output", molten, molten)
    for library in out.glob("*.dylib"):
        dependencies = run("otool", "-L", library, capture=True).stdout.splitlines()[1:]
        for line in dependencies:
            dependency = line.strip().split(" (", 1)[0]
            name = Path(dependency).name
            if name != library.name and (out / name).is_file():
                run("install_name_tool", "-change", dependency, "@loader_path/" + name, library)
        run("codesign", "-s", "-", "--force", library)
        run("codesign", "--verify", "--strict", library)
    print("Packaged simulator runtime:", out)
    print("Build/load success does not prove a physical headset streaming connection.")

if __name__ == "__main__":
    main()
