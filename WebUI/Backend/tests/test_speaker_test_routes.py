import unittest
import sys
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from Backend.api import speaker_test_routes as routes


class SpeakerTestRoutesTest(unittest.TestCase):
    def test_test_cases_expose_protocol_metadata(self):
        cases = routes.list_speaker_test_cases()
        case_ids = {item["id"] for item in cases}

        self.assertIn("hardware", case_ids)
        self.assertIn("tone", case_ids)
        self.assertIn("sweep", case_ids)
        self.assertIn("volume", case_ids)
        for item in cases:
            self.assertTrue(item["command"].startswith("SPK_"))
            self.assertTrue(item["expected"].startswith("SPK_"))
            self.assertGreater(item["timeout"], 0)

    def test_run_speaker_test_sends_command_and_returns_response(self):
        device = Mock()
        device.has_connections.return_value = True
        device.register_waiter.return_value = {"waiter_id": "waiter-1"}
        device.send_command_async.return_value = True
        device.await_waiter.return_value = "SPK_OK"
        ctx = Mock()

        with patch.object(routes, "_get_services", return_value=(device, None)):
            result = routes._run_speaker_test(ctx, "hardware")

        device.send_command_async.assert_called_once_with("SPK_TEST\n")
        device.cancel_waiter.assert_called_once_with("waiter-1")
        self.assertTrue(result["success"])
        self.assertEqual(result["response"], "SPK_OK")

    def test_run_speaker_test_fails_without_device_connection(self):
        device = Mock()
        device.has_connections.return_value = False
        ctx = Mock()

        with patch.object(routes, "_get_services", return_value=(device, None)):
            with self.assertRaisesRegex(RuntimeError, "设备未连接"):
                routes._run_speaker_test(ctx, "hardware")


if __name__ == "__main__":
    unittest.main()
