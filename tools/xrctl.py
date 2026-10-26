#!/usr/bin/env python3
"""Select one visionOS simulator and one USB Quest; never use ADB's default device."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def run(args, *, capture=True, check=True, timeout=60, env=None):
    return subprocess.run([str(a) for a in args], text=True, capture_output=capture,
                          check=check, timeout=timeout, env=env)


def developer_dir():
    if os.environ.get("DEVELOPER_DIR"):
        return Path(os.environ["DEVELOPER_DIR"])
    if os.environ.get("XCODE_BETA_PATH"):
        return Path(os.environ["XCODE_BETA_PATH"]) / "Contents/Developer"
    return Path(run(["xcode-select", "-p"]).stdout.strip())


def simulator_app(dev):
    for path in (dev / "Applications/Simulator.app", dev.parent / "Applications/DeviceHub.app"):
        if path.is_dir():
            return path
    raise RuntimeError("Neither Simulator.app nor DeviceHub.app exists in the selected Xcode.")


def select_simulator(data, requested=None):
    candidates = []
    for runtime, devices in data["devices"].items():
        if ".xrOS-" not in runtime and ".visionOS-" not in runtime:
            continue
        version = tuple(int(v) for v in runtime.split("-", 1)[1].replace(".", "-").split("-"))
        for device in devices:
            if device.get("isAvailable", False):
                candidates.append((device, runtime, version))
    if requested:
        candidates = [item for item in candidates if item[0]["udid"] == requested]
    if not candidates:
        raise RuntimeError("No available visionOS simulator" + (f" matching {requested}" if requested else "") +
                           ". Install a visionOS runtime and create an Apple Vision Pro simulator in Xcode.")
    # Preserve an already booted visionOS session; otherwise prefer the newest runtime.
    candidates.sort(key=lambda item: (item[0]["state"] == "Booted", item[2], item[0]["udid"]), reverse=True)
    return candidates[0][0], candidates[0][1]


def select_quest(output, requested=None):
    devices = []
    for line in output.splitlines()[1:]:
        fields = line.split()
        if len(fields) < 2:
            continue
        props = dict(field.split(":", 1) for field in fields[2:] if ":" in field)
        # Only USB Quest headsets are valid for this wired Monado transport.
        if "usb" in props and props.get("model", "").startswith("Quest"):
            devices.append((fields[0], fields[1], props))
        elif requested == fields[0] and fields[1] != "device":
            raise RuntimeError(f"{requested} is {fields[1]}; accept USB debugging inside the Quest.")
    if requested:
        devices = [d for d in devices if d[0] == requested]
    if len(devices) != 1:
        raise RuntimeError("Connect and authorize exactly one USB Quest, or set ANDROID_SERIAL for the desired headset. "
                           "Other Android devices are ignored.")
    serial, state, props = devices[0]
    if state != "device":
        raise RuntimeError(f"Quest {serial} is {state}; accept USB debugging inside the headset.")
    return serial, props["model"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["doctor", "run", "input", "quest-link", "inject"])
    parser.add_argument("--simulator", default=os.environ.get("SIMULATOR_UDID"))
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"))
    parser.add_argument("--bundle-dir", type=Path, default=ROOT / "build/modern/bundle")
    args = parser.parse_args()
    dev = developer_dir()
    env = dict(os.environ, DEVELOPER_DIR=str(dev))
    simctl = dev / "usr/bin/simctl"

    def sim(*a, **kw):
        return run([simctl, *a], env=env, **kw)

    if args.command == "doctor":
        print(run(["xcodebuild", "-version"], env=env).stdout.strip())
        print("SDK:", run(["xcrun", "--sdk", "xrsimulator", "--show-sdk-path"], env=env).stdout.strip())
        print("UI:", simulator_app(dev))
        failures = []
        try:
            device, runtime = select_simulator(json.loads(sim("list", "devices", "available", "--json").stdout), args.simulator)
            print("Simulator:", device["udid"], runtime, device["state"])
        except RuntimeError as error:
            failures.append(str(error))
        try:
            serial, model = select_quest(run(["adb", "devices", "-l"]).stdout, args.serial)
            print("Quest:", model, serial)
            print("Link:", run(["adb", "-s", serial, "shell", "cmd", "package", "resolve-activity", "--brief",
                                "com.oculus.xrstreamingclient"]).stdout.strip())
        except RuntimeError as error:
            failures.append(str(error))
        for error in failures:
            print("BLOCKED:", error)
        return bool(failures)

    if args.command == "run":
        device, runtime = select_simulator(json.loads(sim("list", "devices", "available", "--json").stdout), args.simulator)
        print("Using:", device["udid"], runtime, flush=True)
        if device["state"] != "Booted":
            sim("boot", device["udid"])
        sim("bootstatus", device["udid"], "-b", capture=False, timeout=240)
        app = simulator_app(dev)
        launch = ["open", app]
        if app.name == "Simulator.app":
            launch += ["--args", "-CurrentDeviceUDID", device["udid"]]
        run(launch)
        return 0

    if args.command == "input":
        device, _ = select_simulator(json.loads(sim("list", "devices", "available", "--json").stdout), args.simulator)
        if device["state"] != "Booted":
            raise RuntimeError("Selected simulator is not booted. Run ./run.sh first.")
        executable = args.bundle_dir.resolve() / "xr-input"
        if not executable.is_file():
            raise RuntimeError("Standalone input helper missing. Build first.")
        os.execv(str(executable), [str(executable), str(dev), device["udid"]])

    serial, model = select_quest(run(["adb", "devices", "-l"]).stdout, args.serial)

    def adb(*a, **kw):
        return run(["adb", "-s", serial, *a], **kw)

    # Validate the package before any simulator changes.
    activity = adb("shell", "cmd", "package", "resolve-activity", "--brief", "com.oculus.xrstreamingclient").stdout
    components = [line.strip() for line in activity.splitlines() if line.strip().startswith("com.oculus.xrstreamingclient/")]
    if len(components) != 1:
        raise RuntimeError(f"Quest Link launch activity unavailable on {model}: {activity.strip()}")
    print("Starting Quest Link on", model, serial, flush=True)
    adb("shell", "am", "force-stop", "com.oculus.xrstreamingclient")
    # -W waits for the first app frame. Quest Link may need the host transport
    # before it can produce that frame, so waiting here deadlocks startup.
    result = adb("shell", "am", "start", "-n", components[0], timeout=15)
    print(result.stdout.strip())
    if "Error:" in result.stdout or "Exception" in result.stdout:
        raise RuntimeError("Quest Link activity launch failed.")
    running = False
    for _ in range(20):
        if adb("shell", "pidof", "com.oculus.xrstreamingclient", check=False, timeout=5).stdout.strip():
            running = True
            break
        time.sleep(0.25)
    if not running:
        raise RuntimeError("Quest Link did not stay running. Dismiss any headset system dialog (such as PC Disconnected), then retry. Inspect adb logcat if the failure persists.")
    print("Quest Link process started. USB transport and video still require a live streaming check.")

    if args.command == "inject":
        device, _ = select_simulator(json.loads(sim("list", "devices", "available", "--json").stdout), args.simulator)
        if device["state"] != "Booted":
            raise RuntimeError("Selected simulator is not booted. Run ./run.sh first.")
        bundle = args.bundle_dir.resolve()
        library = bundle / "libSim2OpenXR.dylib"
        monado = bundle / "libopenxr_monado.dylib"
        if not library.is_file() or not monado.is_file():
            raise RuntimeError("Bridge/runtime libraries missing. Build first or pass --bundle-dir to a prepared bundle.")
        manifest = bundle / "openxr_monado-dev.json"
        manifest.write_text(json.dumps({"file_format_version": "1.0.0", "runtime": {"library_path": str(monado)}}, indent=2) + "\n")
        domain = f"user/{os.getuid()}/com.apple.backboardd"
        udid = device["udid"]
        # Do not carry visionOS 1 beta's frame-pacing and gaze preferences into
        # modern runtimes. Their names/semantics are private and have changed.
        sim("spawn", udid, "launchctl", "debug", domain, "--environment",
            f"DYLD_INSERT_LIBRARIES={library}", f"XR_RUNTIME_JSON={manifest}", f"XRHAX_ROOTDIR={bundle}",
            "XRT_COMPOSITOR_COMPUTE=1", "MVK_CONFIG_RESUME_LOST_DEVICE=1", "MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE=1",
            "QL_OVERRIDE_SCALE=0.75", "QL_THREADS_PER_SLICE=8")
        sim("spawn", udid, "launchctl", "kill", "TERM", domain)
        print("Injection requested; check simulator logs for [Sim2OpenXR]. This does not confirm streaming.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as error:
        print(f"Error: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr or error.stdout, file=sys.stderr)
        sys.exit(1)
