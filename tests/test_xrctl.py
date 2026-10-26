import importlib.util
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch
from types import SimpleNamespace
from contextlib import redirect_stdout
import io
import json

spec = importlib.util.spec_from_file_location("xrctl", Path(__file__).resolve().parents[1] / "tools/xrctl.py")
xrctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xrctl)


class DeviceSelectionTests(unittest.TestCase):
    def test_live_transport_is_blocked_before_any_device_or_xcode_command(self):
        for command in ("inject", "quest-link"):
            with self.subTest(command=command):
                with patch.object(xrctl, "run") as execute, patch("sys.argv", ["xrctl.py", command]), patch.dict(xrctl.os.environ, {"LIBUSB_QUEST3_HOST_ABI": "1"}):
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
