from __future__ import annotations

from dataclasses import dataclass
import sys
import time
import types
import unittest
from pathlib import Path
from unittest.mock import patch


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from av_core.can_protocol import LIGHTING_COMMAND_ID, THREAT_STATE_ID  # noqa: E402
from av_core.can_service import CanService  # noqa: E402
from av_core.models import AvCoreConfig, CockpitStatus, LampTestRequest  # noqa: E402


@dataclass
class FakeMessage:
    arbitration_id: int
    data: bytes
    is_extended_id: bool = False


class FakeBus:
    def __init__(self, *, fail_send: bool = False) -> None:
        self.received: list[FakeMessage] = []
        self.sent: list[FakeMessage] = []
        self.closed = False
        self.fail_send = fail_send

    def recv(self, timeout: float):
        if self.received:
            return self.received.pop(0)
        time.sleep(min(timeout, 0.002))
        return None

    def send(self, message, timeout: float):
        if self.fail_send:
            raise OSError("simulated bus-off")
        self.sent.append(message)

    def shutdown(self):
        self.closed = True


class CanServiceTest(unittest.TestCase):
    def test_fake_socketcan_publishes_state_tracks_heartbeat_and_tests_lamps(self) -> None:
        bus = FakeBus()
        changes: list[None] = []
        service = CanService(AvCoreConfig(), lambda: changes.append(None), bus_factory=lambda: bus)
        fake_module = types.SimpleNamespace(Message=FakeMessage)
        with patch.dict(sys.modules, {"can": fake_module}):
            service.start()
            self.addCleanup(service.stop)
            deadline = time.monotonic() + 1
            while service.status().state != "OK" and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual("OK", service.status().state)
            bus.received.append(FakeMessage(0x700, bytes((1, 0, 0, 4, 0, 1, 2, 3))))
            while service.modules()[0].state != "ONLINE" and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual("ONLINE", service.modules()[0].state)
            self.assertIsNone(service.start_lamp_test(LampTestRequest(target="LAMP", lamp="FIRE")))
            test_frames = [message for message in bus.sent if message.arbitration_id == LIGHTING_COMMAND_ID and message.data[1] == 4]
            self.assertEqual(1, len(test_frames))
            self.assertEqual(2000, int.from_bytes(test_frames[0].data[4:6], "little"))
            cockpit = CockpitStatus(available=True)
            cockpit.threat.available = True
            cockpit.threat.sector_mask = 0x81
            cockpit.threat.lock_state = "ATTEMPT"
            service.update_cockpit(cockpit, "LIVE")
            while not any(message.arbitration_id == THREAT_STATE_ID and message.data[1:3] == bytes((0x81, 1)) for message in bus.sent) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(any(message.arbitration_id == THREAT_STATE_ID and message.data[1:3] == bytes((0x81, 1)) for message in bus.sent))
            bus.received.append(FakeMessage(0x701, bytes((1, 0, 0, 1, 0, 4, 5, 6))))
            while service.modules()[1].state != "ONLINE" and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertIsNone(service.start_lamp_test(LampTestRequest(target="THREAT_PROC")))
        service.stop()
        self.assertTrue(bus.closed)
        self.assertGreater(len(changes), 0)

    def test_bus_failure_closes_socket_and_reconnects_publication(self) -> None:
        failed = FakeBus(fail_send=True)
        recovered = FakeBus()
        buses = iter((failed, recovered))
        service = CanService(AvCoreConfig(), lambda: None, bus_factory=lambda: next(buses))
        fake_module = types.SimpleNamespace(Message=FakeMessage)
        with patch.dict(sys.modules, {"can": fake_module}):
            service.start()
            self.addCleanup(service.stop)
            deadline = time.monotonic() + 3
            while (service.status().state != "OK" or not recovered.sent) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(failed.closed)
            self.assertEqual("OK", service.status().state)
            self.assertGreater(len(recovered.sent), 0)
            recovered.received.append(FakeMessage(0x700, bytes((1, 0, 0, 4, 0, 1, 2, 3))))
            while service.modules()[0].state != "ONLINE" and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual("ONLINE", service.modules()[0].state)


if __name__ == "__main__":
    unittest.main()
