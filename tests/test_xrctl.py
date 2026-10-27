import importlib.util
from pathlib import Path
import unittest
import tempfile
import json
import io
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("xrctl", Path(__file__).resolve().parents[1] / "tools/xrctl.py")
xrctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xrctl)


class DeviceSelectionTests(unittest.TestCase):
    def test_link_can_trigger_initial_invite_after_usb_interface_is_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory)
            for name in ("libSim2OpenXR.dylib", "libopenxr_monado.dylib"):
                (bundle / name).touch()
            marker = bundle / ".quest-pairing-ready"
            calls = []
            def execute(args, **kwargs):
                args = [str(a) for a in args]
                calls.append(args)
                output = ""
                if args == ["adb", "devices", "-l"]:
                    output = "List of devices attached\nquest device usb:1 model:Quest_3\n"
                elif "resolve-activity" in args:
                    output = "com.oculus.xrstreamingclient/.MainActivity\n"
                elif "--json" in args:
                    output = json.dumps({"devices": {"com.apple.CoreSimulator.SimRuntime.xrOS-27-0":
                        [dict(udid="sim", state="Booted", isAvailable=True)]}})
                elif "launchctl" in args and "kill" in args:
                    marker.write_text("usb-ready\n")
                elif "am" in args and "start" in args:
                    marker.write_text("ready\n")
                elif "pidof" in args:
                    output = "1234\n"
                return SimpleNamespace(stdout=output, stderr="", returncode=0)
            argv = ["xrctl.py", "inject", "--serial", "quest", "--simulator", "sim", "--bundle-dir", directory]
            with patch.object(xrctl, "run", side_effect=execute), patch.object(xrctl, "developer_dir", return_value=bundle), patch("sys.argv", argv), patch.object(xrctl.time, "monotonic", side_effect=[0, 0, 1, 26]), patch.object(xrctl.time, "sleep"), patch.dict(xrctl.os.environ, {"LIBUSB_QUEST3_HOST_ABI": "1", "LIBUSB_QUEST3_DIAGNOSTIC": "1"}), redirect_stdout(io.StringIO()):
                self.assertEqual(xrctl.main(), 0)
            starts = [i for i, args in enumerate(calls) if "am" in args and "start" in args]
            self.assertEqual(len(starts), 2)
            self.assertFalse(any("force-stop" in args for args in calls))
            self.assertFalse(any("force-stop" in args for args in calls[starts[0] + 1:starts[1]]),
                "Pairing handoff must not terminate the running Link activity")
            injected = next(i for i, args in enumerate(calls) if "launchctl" in args and "debug" in args)
            self.assertLess(injected, starts[0])

    def test_stream_start_waits_for_fresh_host_pairing_and_keeps_adb_targeted(self):
        for host_ready in (False, True):
            with self.subTest(host_ready=host_ready), tempfile.TemporaryDirectory() as directory:
                bundle = Path(directory)
                for name in ("libSim2OpenXR.dylib", "libopenxr_monado.dylib"):
                    (bundle / name).touch()
                marker = bundle / ".quest-pairing-ready"
                marker.write_text("ready\n")  # A prior session must not authorize launch.
                calls = []
                def execute(args, **kwargs):
                    args = [str(a) for a in args]
                    calls.append(args)
                    output = ""
                    if args == ["adb", "devices", "-l"]:
                        output = "List of devices attached\nquest device usb:1 model:Quest_3\n"
                    elif "resolve-activity" in args:
                        output = "com.oculus.xrstreamingclient/.MainActivity\n"
                    elif "--json" in args:
                        output = json.dumps({"devices": {"com.apple.CoreSimulator.SimRuntime.xrOS-27-0":
                            [dict(udid="sim", state="Booted", isAvailable=True)]}})
                    elif "launchctl" in args and "kill" in args and host_ready:
                        marker.write_text("ready\n")
                    elif "pidof" in args:
                        output = "1234\n"
                    return SimpleNamespace(stdout=output, stderr="", returncode=0)
                argv = ["xrctl.py", "inject", "--serial", "quest", "--simulator", "sim", "--bundle-dir", directory]
                ticks = [0, 0] if host_ready else [0, 100]
                with patch.object(xrctl, "run", side_effect=execute), patch.object(xrctl, "developer_dir", return_value=bundle), patch("sys.argv", argv), patch.object(xrctl.time, "monotonic", side_effect=ticks), patch.dict(xrctl.os.environ, {"LIBUSB_QUEST3_HOST_ABI": "1", "LIBUSB_QUEST3_DIAGNOSTIC": "1"}), redirect_stdout(io.StringIO()):
                    if host_ready:
                        self.assertEqual(xrctl.main(), 0)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "Host USB pairing did not become ready"):
                            xrctl.main()
                starts = [i for i, args in enumerate(calls) if "am" in args and "start" in args]
                self.assertEqual(bool(starts), host_ready)
                if host_ready:
                    injected = next(i for i, args in enumerate(calls) if "launchctl" in args and "debug" in args)
                    self.assertLess(injected, starts[0])
                    self.assertNotIn("-W", calls[starts[0]])
                    self.assertIn("LIBUSB_QUEST3_DIAGNOSTIC=1", calls[injected])
                else:
                    self.assertFalse(marker.exists())
                for args in calls:
                    if args[0] == "adb" and "devices" not in args:
                        self.assertEqual(args[1:3], ["-s", "quest"])

    def test_live_transport_is_blocked_before_any_device_or_xcode_command(self):
        for command in ("inject", "quest-link"):
            with self.subTest(command=command):
                with patch.object(xrctl, "run") as execute, patch("sys.argv", ["xrctl.py", command]), patch.dict(xrctl.os.environ, {"LIBUSB_QUEST3_HOST_ABI": "1", "LIBUSB_QUEST3_DIAGNOSTIC": "0"}):
                    with self.assertRaisesRegex(RuntimeError, "disabled after a host USB kernel panic"):
                        xrctl.main()
                    execute.assert_not_called()

    def test_device_hub_and_legacy_simulator_app_layouts(self):
        with tempfile.TemporaryDirectory() as directory:
            dev = Path(directory) / "Contents/Developer"
            hub = dev.parent / "Applications/DeviceHub.app"
            hub.mkdir(parents=True)
            self.assertEqual(xrctl.simulator_app(dev), hub)
            simulator = dev / "Applications/Simulator.app"
            simulator.mkdir(parents=True)
            self.assertEqual(xrctl.simulator_app(dev), simulator)

    def test_tv_and_wireless_quest_cannot_receive_usb_link_commands(self):
        devices = """List of devices attached
tv:5555 device product:canvas model:SmartTV_4K device:tv transport_id:1
wireless:5555 device product:eureka model:Quest_3 device:eureka transport_id:2
quest-usb device usb:52428800X product:eureka model:Quest_3 device:eureka transport_id:3
"""
        self.assertEqual(xrctl.select_quest(devices), ("quest-usb", "Quest_3"))
        with self.assertRaises(RuntimeError):
            xrctl.select_quest(devices, "tv:5555")

    def test_multiple_usb_quests_require_explicit_selection(self):
        devices = "List of devices attached\na device usb:1 model:Quest_3\nb device usb:2 model:Quest_Pro\n"
        with self.assertRaises(RuntimeError):
            xrctl.select_quest(devices)
        self.assertEqual(xrctl.select_quest(devices, "b"), ("b", "Quest_Pro"))

    def test_unauthorized_headset_is_actionable(self):
        with self.assertRaisesRegex(RuntimeError, "USB debugging"):
            xrctl.select_quest("List of devices attached\nquest unauthorized usb:1\n", "quest")

    def test_select_newest_available_visionos_without_touching_ios(self):
        data = {"devices": {
            "com.apple.CoreSimulator.SimRuntime.iOS-27-0": [dict(udid="ios", state="Booted", isAvailable=True)],
            "com.apple.CoreSimulator.SimRuntime.xrOS-2-5": [dict(udid="old", state="Shutdown", isAvailable=True)],
            "com.apple.CoreSimulator.SimRuntime.xrOS-27-0": [dict(udid="new", state="Shutdown", isAvailable=True)],
            "com.apple.CoreSimulator.SimRuntime.xrOS-28-0": [dict(udid="broken", state="Shutdown", isAvailable=False)],
        }}
        self.assertEqual(xrctl.select_simulator(data)[0]["udid"], "new")
        data["devices"]["com.apple.CoreSimulator.SimRuntime.xrOS-2-5"][0]["state"] = "Booted"
        self.assertEqual(xrctl.select_simulator(data)[0]["udid"], "old")
        self.assertEqual(xrctl.select_simulator(data, "new")[0]["udid"], "new")
        with self.assertRaises(RuntimeError):
            xrctl.select_simulator(data, "ios")

    def test_no_visionos_runtime_fails_before_launch(self):
        with self.assertRaisesRegex(RuntimeError, "No available visionOS"):
            xrctl.select_simulator({"devices": {}})


if __name__ == "__main__":
    unittest.main()
