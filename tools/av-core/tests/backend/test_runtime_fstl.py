from __future__ import annotations

import sys
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from av_core.config import ConfigStore  # noqa: E402
from av_core import fstl_service  # noqa: E402
from av_core.fstl_service import FstlFrame, FstlService  # noqa: E402
from av_core.models import TelemetryConfig  # noqa: E402
from av_core.runtime import AvCoreRuntime  # noqa: E402
import test_fstl_console_client_contract as contract  # noqa: E402


class FakeFstlService:
    def __init__(self, config, callback):
        self.config = config
        self.callback = callback
        self.started = False
        self.stopped = False
        self.reconfigurations = []

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def reconfigure(self, config):
        self.config = config
        self.reconfigurations.append(config)

    def emit(self, frame):
        self.callback(frame)


class RuntimeFstlTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.runtime = AvCoreRuntime(
            ConfigStore(Path(self.directory.name) / "av-core.json"),
            fstl_factory=FakeFstlService,
        )
        self.service: FakeFstlService = self.runtime._fstl  # type: ignore[assignment]

    @staticmethod
    def live_frame(engine_ratio: float = 0.49) -> FstlFrame:
        records = {
            "FLIGHT_STATE": [{"entity_id": "1"}],
            "HUD_ALERT_STATE": [{
                "entity_id": "1", "presence": 0, "primary_fire_threat_active": True,
                "missile_lock_state": 0, "missile_direction_sector_mask": 0,
            }],
            "THREAT_STATE": [{"entity_id": "1", "incoming_missiles": []}],
        }
        return FstlFrame(
            state="LIVE", session_id="42", last_live_monotonic=1.0,
            player_entity_id="1", records=records,
            mission_active=True, mission_paused=False,
            mission_generation=7, time_compression=1.0,
            derived={
                "entities.1.engine_integrity_ratio": {
                    "available": True, "reason": None, "value": engine_ratio,
                }
            },
        )

    def test_lifecycle_and_alerts_follow_fstl_frames(self) -> None:
        self.assertEqual("DISCONNECTED", self.runtime.status().telemetry.state)
        self.runtime.start()
        self.assertTrue(self.service.started)
        self.service.emit(self.live_frame())
        live = self.runtime.status()
        self.assertEqual("LIVE", live.telemetry.state)
        self.assertEqual("42", live.telemetry.session_id)
        self.assertTrue(live.cockpit.available)
        self.assertTrue(live.cockpit.warnings.fire)
        self.assertEqual("ACTIVE", live.cockpit.cautions.engine.state)

        self.service.emit(FstlFrame(state="STALE", session_id="42", last_live_monotonic=1.0))
        stale = self.runtime.status()
        self.assertEqual("STALE", stale.telemetry.state)
        self.assertFalse(stale.cockpit.available)
        self.assertFalse(stale.cockpit.warnings.master)
        self.service.emit(self.live_frame(engine_ratio=0.75))
        resumed = self.runtime.status()
        self.assertEqual("LIVE", resumed.telemetry.state)
        self.assertTrue(resumed.cockpit.available)
        self.assertEqual("CLEAR", resumed.cockpit.cautions.engine.state)
        self.runtime.stop()
        self.assertTrue(self.service.stopped)

    def test_prewarmed_and_paused_mission_are_exposed_separately(self) -> None:
        self.service.emit(FstlFrame(state="READY", session_id="41"))
        ready = self.runtime.status()
        self.assertEqual("READY", ready.telemetry.state)
        self.assertFalse(ready.mission.active)
        self.assertFalse(ready.cockpit.available)

        paused_frame = replace(
            self.live_frame(), mission_paused=True, time_compression=0.5
        )
        self.service.emit(paused_frame)
        paused = self.runtime.status()
        self.assertEqual("LIVE", paused.telemetry.state)
        self.assertTrue(paused.mission.active)
        self.assertTrue(paused.mission.paused)
        self.assertEqual(7, paused.mission.generation)
        self.assertEqual(0.5, paused.mission.time_compression)
        self.assertTrue(paused.cockpit.available)

    def test_threshold_change_recalculates_without_fstl_reconnect(self) -> None:
        self.service.emit(self.live_frame(engine_ratio=0.45))
        config = self.runtime.config
        config.alerts.engine.activate_below_percent = 40
        config.alerts.engine.clear_above_percent = 50
        self.runtime.update_config(config)
        self.assertEqual("CLEAR", self.runtime.status().cockpit.cautions.engine.state)
        self.assertEqual([], self.service.reconfigurations)

    def test_endpoint_change_requests_one_reconfiguration(self) -> None:
        config = self.runtime.config
        config.telemetry.port = 42043
        self.runtime.update_config(config)
        self.assertEqual(1, len(self.service.reconfigurations))
        self.assertEqual(42043, self.service.reconfigurations[0].port)


class FstlServiceRecoveryTest(unittest.TestCase):
    @staticmethod
    def wait_for(predicate, timeout: float = 2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.01)
        return None

    @staticmethod
    def negotiate(server: socket.socket, client_address, hello: bytes, session_id: int) -> None:
        for message_type, payload, sequence in (
            (3, contract.welcome_for(hello), 1),
            (4, contract.session_begin_payload(), 2),
            (6, contract.cockpit_snapshot_payload(), 3),
        ):
            server.sendto(
                contract.packet(
                    message_type,
                    payload,
                    session_id=session_id,
                    sequence=sequence,
                    sent_us=1_000_000 + sequence,
                    flags=2,
                ),
                client_address,
            )

    def test_live_client_rejects_legacy_profile_coverage(self) -> None:
        frames: list[FstlFrame] = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            service = FstlService(
                TelemetryConfig(host="127.0.0.1", port=server.getsockname()[1]),
                frames.append,
            )
            service.start()
            try:
                hello, client_address = server.recvfrom(1200)
                session = 0x1122334455667788
                for message_type, payload, sequence in (
                    (3, contract.welcome_for(hello), 1),
                    (4, contract.session_begin_payload(), 2),
                    (6, contract.v11_payload("minimal-with-player", ".bin"), 3),
                ):
                    server.sendto(
                        contract.packet(
                            message_type,
                            payload,
                            session_id=session,
                            sequence=sequence,
                            sent_us=1_000_000 + sequence,
                            flags=2,
                        ),
                        client_address,
                    )
                rejected = self.wait_for(
                    lambda: next(
                        (frame for frame in frames if frame.error and "0x07CB" in frame.error),
                        None,
                    )
                )
                self.assertIsNotNone(rejected)
                self.assertFalse(any(frame.state == "LIVE" for frame in frames))
            finally:
                service.stop()

    def test_stale_session_resumes_during_the_grace_period(self) -> None:
        frames: list[FstlFrame] = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, mock.patch.object(
            fstl_service, "RECOVERY_GRACE_SECONDS", 0.5
        ):
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            service = FstlService(
                TelemetryConfig(host="127.0.0.1", port=server.getsockname()[1], stale_after_ms=20),
                frames.append,
            )
            service.start()
            try:
                hello, client_address = server.recvfrom(1200)
                session = 0x1122334455667788
                self.negotiate(server, client_address, hello, session)
                self.assertTrue(self.wait_for(lambda: frames and frames[-1].state == "LIVE"))
                self.assertTrue(self.wait_for(lambda: frames and frames[-1].state == "STALE"))

                while True:
                    packet, address = server.recvfrom(1200)
                    if contract.reference.read_header(packet)["message_type"] == 12:
                        self.assertEqual(client_address, address)
                        break
                for part_index, payload in enumerate(contract.snapshot_parts(2)):
                    server.sendto(
                        contract.packet(
                            6,
                            payload,
                            session_id=session,
                            sequence=10 + part_index,
                            sent_us=2_000_000 + part_index,
                            flags=2,
                        ),
                        client_address,
                    )
                recovered = self.wait_for(
                    lambda: frames[-1]
                    if frames and frames[-1].state == "LIVE" and frames[-1].session_id == str(session)
                    else None
                )
                self.assertIsNotNone(recovered)
            finally:
                service.stop()

    def test_absent_producer_keeps_one_hello_generation(self) -> None:
        frames: list[FstlFrame] = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, mock.patch.object(
            fstl_service.fstl, "RELIABLE_WINDOW_US", 120_000
        ):
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            service = FstlService(
                TelemetryConfig(host="127.0.0.1", port=server.getsockname()[1], stale_after_ms=20),
                frames.append,
            )
            service.start()
            try:
                first, first_address = server.recvfrom(1200)
                renewed, renewed_address = server.recvfrom(1200)
                first_header = contract.reference.read_header(first)
                renewed_header = contract.reference.read_header(renewed)
                self.assertEqual(first_address, renewed_address)
                self.assertEqual(first[68:], renewed[68:])
                self.assertEqual(first_header["message_id"], renewed_header["message_id"])
                self.assertNotEqual(
                    0, renewed_header["flags"] & fstl_service.fstl.RETRANSMISSION
                )
            finally:
                service.stop()

    def test_stale_session_rehandshakes_on_same_endpoint_after_one_grace_period(self) -> None:
        frames: list[FstlFrame] = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, mock.patch.object(
            fstl_service, "RECOVERY_GRACE_SECONDS", 0.15
        ):
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            service = FstlService(
                TelemetryConfig(host="127.0.0.1", port=server.getsockname()[1], stale_after_ms=20),
                frames.append,
            )
            service.start()
            try:
                hello, first_address = server.recvfrom(1200)
                first_nonce = int.from_bytes(hello[68:76], "little")
                self.negotiate(server, first_address, hello, 0x1122334455667788)
                self.assertTrue(self.wait_for(lambda: frames and frames[-1].state == "LIVE"))
                self.assertTrue(self.wait_for(lambda: frames and frames[-1].state == "STALE"))

                resync_count = 0
                second_hello = None
                second_address = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet, address = server.recvfrom(1200)
                    message_type = contract.reference.read_header(packet)["message_type"]
                    if message_type == 12 and address == first_address:
                        resync_count += 1
                    if message_type == 2 and address == first_address:
                        second_hello, second_address = packet, address
                        break
                self.assertIsNotNone(second_hello)
                self.assertEqual(first_address, second_address)
                self.assertNotEqual(
                    first_nonce, int.from_bytes(second_hello[68:76], "little")
                )
                self.assertEqual(1, resync_count)
            finally:
                service.stop()

    def test_replacement_session_keeps_its_reliable_snapshot_window(self) -> None:
        frames: list[FstlFrame] = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, \
                mock.patch.object(fstl_service, "RECOVERY_GRACE_SECONDS", 0.05), \
                mock.patch.object(fstl_service.fstl, "RELIABLE_WINDOW_US", 500_000):
            server.bind(("127.0.0.1", 0))
            server.settimeout(2.0)
            service = FstlService(
                TelemetryConfig(host="127.0.0.1", port=server.getsockname()[1], stale_after_ms=20),
                frames.append,
            )
            service.start()
            try:
                first_hello, address = server.recvfrom(1200)
                self.negotiate(server, address, first_hello, 0x1122334455667788)
                self.assertTrue(self.wait_for(lambda: frames and frames[-1].state == "LIVE"))
                self.assertTrue(self.wait_for(lambda: frames and frames[-1].state == "STALE"))

                replacement_hello = None
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    packet, packet_address = server.recvfrom(1200)
                    if contract.reference.read_header(packet)["message_type"] == 2:
                        replacement_hello = packet
                        self.assertEqual(address, packet_address)
                        break
                self.assertIsNotNone(replacement_hello)
                replacement_session = 0x8877665544332211
                for message_type, payload, sequence in (
                    (3, contract.welcome_for(replacement_hello), 20),
                    (4, contract.session_begin_payload(), 21),
                ):
                    server.sendto(
                        contract.packet(message_type, payload, session_id=replacement_session,
                                        sequence=sequence, sent_us=2_000_000 + sequence, flags=2),
                        address,
                    )

                # This delay is longer than stale + recovery grace, but still
                # inside the accepted replacement session's reliable window.
                server.settimeout(0.3)
                third_hello = None
                try:
                    while True:
                        packet, _ = server.recvfrom(1200)
                        if contract.reference.read_header(packet)["message_type"] == 2:
                            third_hello = packet
                            break
                except socket.timeout:
                    pass
                self.assertIsNone(third_hello)

                server.sendto(
                    contract.packet(
                        6, contract.cockpit_snapshot_payload(),
                        session_id=replacement_session, sequence=22,
                        sent_us=2_000_022, flags=2,
                    ),
                    address,
                )
                self.assertTrue(self.wait_for(
                    lambda: frames and frames[-1].state == "LIVE"
                    and frames[-1].session_id == str(replacement_session)
                ))
            finally:
                service.stop()


class InstalledFstlImportTest(unittest.TestCase):
    def test_fstl_client_imports_from_an_isolated_install_directory(self) -> None:
        source = Path(__file__).resolve().parents[4] / "test" / "telemetry" / "protocol" / "tools"
        with tempfile.TemporaryDirectory() as directory:
            install = Path(directory) / "fstl-client"
            install.mkdir()
            for name in ("fstl_client_core.py", "fstl_reference_decoder.py"):
                shutil.copy2(source / name, install / name)
            environment = os.environ.copy()
            environment["AV_CORE_FSTL_CLIENT_DIR"] = str(install)
            environment["PYTHONPATH"] = str(BACKEND_ROOT)
            completed = subprocess.run(
                [sys.executable, "-c", "import av_core.fstl_service as service; print(service.fstl.__file__)"],
                cwd=directory,
                env=environment,
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertEqual((install / "fstl_client_core.py").resolve(), Path(completed.stdout.strip()).resolve())


if __name__ == "__main__":
    unittest.main()
