"""Runtime, measurement, capture and replay services for the simpit dashboard."""

from __future__ import annotations

import copy
import csv
import json
import math
import socket
import statistics
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable


REPO_ROOT = Path(__file__).resolve().parents[3]
PROTOCOL_TOOLS = REPO_ROOT / "test" / "telemetry" / "protocol" / "tools"
if str(PROTOCOL_TOOLS) not in sys.path:
    sys.path.insert(0, str(PROTOCOL_TOOLS))

import fstl_client_core as fstl  # noqa: E402
import fstl_reference_decoder as decoder  # noqa: E402
from capture_store import (  # noqa: E402
    CAPTURE_SCHEMA,
    LEGACY_CAPTURE_SCHEMAS,
    CaptureReader,
    CaptureLibrary,
    CaptureWriter,
    default_capture_directory,
    load_capture,
    load_checkpoint,
    open_capture,
)
from replay_udp import ReplayUdpProducer  # noqa: E402


LEGACY_CAPTURE_SCHEMA = LEGACY_CAPTURE_SCHEMAS[0]
HUD_ALERT_CAPTURE_FEATURE = "HUD_ALERT_STATE/v1"
SNAPSHOT_SCHEMA = "DashboardSnapshotV1"
WINDOWS_SIO_UDP_CONNRESET = 0x9800000C


def utc_now() -> str:
    return fstl.utc_iso8601_from_ns(time.time_ns())


def percentile(values: list[float], ratio: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(ratio * len(ordered)) - 1))
    return ordered[index]


def public_record(record: dict[str, Any]) -> dict[str, Any]:
    """Remove capture-only wire bytes from the dashboard JSON projection."""
    result = copy.deepcopy(record)
    result.pop("_encodedRecordHex", None)
    return result


def configure_live_udp_socket(sock: socket.socket) -> None:
    """Treat an absent UDP producer as silence instead of a fatal reset.

    Windows reports an ICMP port-unreachable response on a connected UDP
    socket as WSAECONNRESET.  FSO is expected to disappear and later return,
    so that response must not abort the bridge's HELLO retry window.
    """
    if sys.platform != "win32":
        return
    import ctypes

    bytes_returned = ctypes.c_ulong()
    disabled = ctypes.c_ulong(0)
    ws2_32 = ctypes.windll.ws2_32
    wsa_ioctl = ws2_32.WSAIoctl
    wsa_ioctl.argtypes = [
        ctypes.c_size_t,
        ctypes.c_ulong,
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    wsa_ioctl.restype = ctypes.c_int
    result = wsa_ioctl(
        sock.fileno(),
        WINDOWS_SIO_UDP_CONNRESET,
        ctypes.byref(disabled),
        ctypes.sizeof(disabled),
        None,
        0,
        ctypes.byref(bytes_returned),
        None,
        None,
    )
    if result != 0:
        error = ws2_32.WSAGetLastError()
        raise OSError(error, "failed to disable SIO_UDP_CONNRESET")


@dataclass
class ChannelMeasurement:
    record_name: str
    identity: str
    configured_hz: float
    samples_us: deque[int] = field(default_factory=lambda: deque(maxlen=512))
    received_us: deque[int] = field(default_factory=lambda: deque(maxlen=512))
    updates: int = 0
    repeated_samples: int = 0
    gap_count: int = 0
    last_sample_us: int | None = None

    def observe(self, sample_us: int, received_us: int) -> None:
        if self.last_sample_us == sample_us:
            self.repeated_samples += 1
            return
        if self.last_sample_us is not None and sample_us > self.last_sample_us:
            expected = 1_000_000.0 / self.configured_hz if self.configured_hz > 0 else 0.0
            if expected and sample_us - self.last_sample_us > expected * 1.8:
                self.gap_count += max(1, round((sample_us - self.last_sample_us) / expected) - 1)
        self.last_sample_us = sample_us
        self.samples_us.append(sample_us)
        self.received_us.append(received_us)
        self.updates += 1

    def snapshot(self, age_us: int | None) -> dict[str, Any]:
        intervals = [
            float(self.samples_us[index] - self.samples_us[index - 1])
            for index in range(1, len(self.samples_us))
            if self.samples_us[index] > self.samples_us[index - 1]
        ]
        observed_hz = (
            (len(self.samples_us) - 1) * 1_000_000.0
            / (self.samples_us[-1] - self.samples_us[0])
            if len(self.samples_us) > 1 and self.samples_us[-1] > self.samples_us[0]
            else None
        )
        median = statistics.median(intervals) if intervals else None
        jitter = (
            statistics.fmean(abs(value - median) for value in intervals)
            if median is not None
            else None
        )
        return {
            "recordName": self.record_name,
            "identity": self.identity,
            "configuredHz": self.configured_hz,
            "observedHz": observed_hz,
            "ageUs": age_us,
            "intervalP50Us": percentile(intervals, 0.50),
            "intervalP95Us": percentile(intervals, 0.95),
            "intervalMaxUs": max(intervals) if intervals else None,
            "jitterUs": jitter,
            "gapCount": self.gap_count,
            "repeatedSamples": self.repeated_samples,
            "updates": self.updates,
            "lastProducerSampleUs": str(self.last_sample_us) if self.last_sample_us is not None else None,
        }


class QualityTracker:
    def __init__(self, flight_hz: int, systems_hz: int, mission_heartbeat_ms: int) -> None:
        self.flight_hz = flight_hz
        self.systems_hz = systems_hz
        self.mission_heartbeat_ms = mission_heartbeat_ms
        self.channels: dict[str, ChannelMeasurement] = {}
        self.packet_count = 0
        self.packet_gap_count = 0
        self.decode_error_count = 0
        self.resync_count = 0
        self.last_transport_sequence: int | None = None
        self.history: deque[dict[str, Any]] = deque(maxlen=100_000)

    def configured_hz(self, record_name: str) -> float:
        if record_name in ("FLIGHT_STATE", "CONTROL_STATE"):
            return float(self.flight_hz)
        if record_name in ("SESSION_STATE", "MISSION_STATE"):
            return 1000.0 / self.mission_heartbeat_ms
        return float(self.systems_hz)

    def observe_packet(self, datagram: bytes) -> None:
        self.packet_count += 1
        try:
            header = decoder.read_header(datagram)
            sequence = int(header["packet_sequence"])
            if self.last_transport_sequence is not None:
                distance = (sequence - self.last_transport_sequence) & 0xFFFFFFFF
                if 1 < distance < 0x80000000:
                    self.packet_gap_count += distance - 1
            self.last_transport_sequence = sequence
            if header["message_type"] == 12:
                self.resync_count += 1
        except (ValueError, decoder.DecodeFailure, KeyError):
            self.decode_error_count += 1

    def observe_state(self, state: fstl.ConsoleState, received_us: int) -> None:
        for identity, record in state.record_instances.items():
            sample = record["fields"].get("producer_sample_time_us")
            if sample is None:
                continue
            key = identity
            measurement = self.channels.get(key)
            if measurement is None:
                measurement = ChannelMeasurement(
                    record_name=record["recordName"],
                    identity=identity,
                    configured_hz=self.configured_hz(record["recordName"]),
                )
                self.channels[key] = measurement
            before = measurement.updates
            measurement.observe(int(sample), received_us)
            if measurement.updates != before:
                self.history.append(
                    {
                        "recordName": record["recordName"],
                        "identity": identity,
                        "producerSampleUs": str(sample),
                        "receivedMonotonicUs": str(received_us),
                    }
                )

    def snapshot(self, projection: dict[str, Any]) -> dict[str, Any]:
        derived = projection.get("derived", {})
        channels = []
        for identity, measurement in sorted(self.channels.items()):
            age_item = derived.get(f"records.{identity}.age_us", {})
            age_us = age_item.get("value") if age_item.get("available") else None
            channels.append(measurement.snapshot(age_us))
        return {
            "packets": self.packet_count,
            "transportGapCount": self.packet_gap_count,
            "decodeErrorCount": self.decode_error_count,
            "resyncCount": self.resync_count,
            "channels": channels,
        }


class TelemetryRuntime:
    """Own one FSTL client and publish immutable dashboard snapshots."""

    def __init__(
        self,
        *,
        host: str,
        port: int,
        flight_hz: int,
        systems_hz: int,
        mission_heartbeat_ms: int,
        capture_dir: Path,
        replay_path: Path | None = None,
        stale_us: int = 3_000_000,
        recovery_reconnect_us: int = 10_000_000,
        capture_warn_bytes: int = 5 * 1024**3,
        capture_stop_bytes: int = 10 * 1024**3,
        capture_free_reserve_bytes: int = 2 * 1024**3,
    ) -> None:
        self.host = host
        self.port = port
        self.flight_hz = flight_hz
        self.systems_hz = systems_hz
        self.mission_heartbeat_ms = mission_heartbeat_ms
        self.capture = CaptureWriter(
            capture_dir,
            warn_bytes=capture_warn_bytes,
            stop_bytes=capture_stop_bytes,
            free_reserve_bytes=capture_free_reserve_bytes,
        )
        self.capture_library = CaptureLibrary(capture_dir)
        self.replay_path = replay_path
        self.stale_us = stale_us
        self.recovery_reconnect_us = recovery_reconnect_us
        self.quality = QualityTracker(flight_hz, systems_hz, mission_heartbeat_ms)
        self.client: fstl.ConsoleClient | None = None
        self.mode = "replay" if replay_path else "live"
        self.replay_state = {
            "path": str(replay_path) if replay_path else None,
            "captureId": None,
            "playing": bool(replay_path),
            "speed": 1.0,
            "position": 0,
            "packetCount": 0,
            "positionUs": 0,
            "durationUs": 0,
            "timelineUnit": "microseconds",
            "seeking": False,
            "previewing": False,
            "previewPositionUs": None,
            "activeRangeId": None,
            "loop": False,
            "ranges": [],
            "sessionBoundaries": [],
            "captureSchema": None,
            "contractFeatures": [],
        }
        self.replay_udp_state = {
            "running": False,
            "bindHost": "127.0.0.1",
            "port": self.port,
            "lanEnabled": False,
            "clientCount": 0,
            "maxClients": 4,
            "endpoint": None,
            "error": None,
        }
        self._recovery_state = "idle"
        self._snapshot = self._empty_snapshot("Synchronizing" if replay_path else "Disconnected")
        self._version = 0
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._replay_wakeup = threading.Event()
        self._mode_change = threading.Event()
        self._soft_resync_live = threading.Event()
        self._restart_live = threading.Event()
        self._replay_seek_target: int | None = None
        self._replay_seek_time_us: int | None = None
        self._replay_preview_time_us: int | None = None
        self._replay_udp_client: fstl.ConsoleClient | None = None
        self._capture_session_index = -1
        self._capture_session_id = 0
        self._capture_last_checkpoint_us = -5_000_000
        self._replay_source_session = 0
        self._thread: threading.Thread | None = None
        self.replay_udp = ReplayUdpProducer(
            lambda: self._replay_udp_client.state if self.mode == "replay" and self._replay_udp_client is not None else None,
            lambda: int(self.replay_state.get("positionUs", 0)),
            self._on_replay_udp_state,
            lambda: bool(self.replay_state.get("playing", False)),
        )

    @property
    def version(self) -> int:
        with self._lock:
            return self._version

    def latest(self) -> dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._snapshot)

    def _on_replay_udp_state(self, state: dict[str, Any]) -> None:
        with self._lock:
            self.replay_udp_state = copy.deepcopy(state)
            if hasattr(self, "_snapshot"):
                self._snapshot["replayUdp"] = copy.deepcopy(state)
                self._snapshot["publishedAtUtc"] = utc_now()
                self._version += 1

    def _empty_snapshot(self, status: str) -> dict[str, Any]:
        return {
            "schema": SNAPSHOT_SCHEMA,
            "publishedAtUtc": utc_now(),
            "mode": self.mode,
            "connection": {
                "status": status,
                "recoveryState": self._recovery_state,
                "host": self.host,
                "port": self.port,
                "sessionId": "0",
                "lastLiveObservedUtc": None,
                "staleReason": None,
            },
            "session": {},
            "mission": {},
            "playerEntityId": None,
            "records": {},
            "recordInstances": {},
            "manifest": {},
            "derived": {},
            "transport": {"synchronized": False, "baseline": 0, "deltaSequence": 0, "manifestId": 0},
            "quality": {
                "packets": 0,
                "transportGapCount": 0,
                "decodeErrorCount": 0,
                "resyncCount": 0,
                "channels": [],
            },
            "capture": {"active": False, "path": None, **self.capture.metrics()},
            "replay": copy.deepcopy(self.replay_state),
            "replayUdp": copy.deepcopy(self.replay_udp_state),
        }

    def _publish(self, at_us: int, at_utc: str) -> None:
        client = self.client
        if client is None:
            return
        state = client.state
        projection = fstl.DashboardProjection(
            state,
            at_us,
            state.smoothed_offset_us,
            state.clock_filter_valid,
            self.flight_hz,
            self.systems_hz,
            self.mission_heartbeat_ms,
        ).build()
        records_by_name: dict[str, list[dict[str, Any]]] = {}
        for record in state.record_instances.values():
            records_by_name.setdefault(record["recordName"], []).append(copy.deepcopy(record["fields"]))
        for records in records_by_name.values():
            records.sort(key=lambda fields: (str(fields.get("entity_id", "")), str(fields.get("subsystem_id", ""))))
        session = copy.deepcopy(state.records.get("SESSION_STATE", {}))
        mission = copy.deepcopy(state.records.get("MISSION_STATE", {}))
        snapshot = {
            "schema": SNAPSHOT_SCHEMA,
            "publishedAtUtc": at_utc,
            "mode": self.mode,
            "connection": {
                "status": state.status,
                "recoveryState": self._recovery_state,
                "host": self.host,
                "port": self.port,
                "sessionId": str(state.session_id),
                "lastLiveObservedUtc": state.last_state_utc,
                "staleReason": state.stale_reason if state.status == "Stale" else None,
            },
            "session": session,
            "mission": mission,
            "playerEntityId": session.get("observed_player_entity_id"),
            "records": records_by_name,
            "recordInstances": {
                identity: public_record(record)
                for identity, record in state.record_instances.items()
            },
            "manifest": {
                "id": state.manifest_id,
                "records": {
                    identity: public_record(record)
                    for identity, record in state.manifest_records.items()
                },
            },
            "derived": projection["derived"],
            "transport": {
                "synchronized": projection["synchronized"],
                "synchronizationConditions": projection["synchronizationConditions"],
                "baseline": state.baseline,
                "deltaSequence": state.delta_sequence,
                "manifestId": state.manifest_id,
                "requiredManifestId": state.required_manifest_id,
            },
            "quality": self.quality.snapshot(projection),
            "capture": {
                "active": self.capture.active,
                "path": str(self.capture.path) if self.capture.path else None,
                "id": self.capture.capture_id,
                **self.capture.metrics(),
            },
            "replay": copy.deepcopy(self.replay_state),
            "replayUdp": copy.deepcopy(self.replay_udp_state),
        }
        with self._lock:
            self._snapshot = snapshot
            self._version += 1

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="telemetry-dashboard-runtime", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            self._mode_change.clear()
            if self.mode == "replay":
                self._run_replay()
            else:
                self._run_live()
            if not self._mode_change.is_set():
                break

    def stop(self) -> None:
        self._stop.set()
        self._mode_change.set()
        self._replay_wakeup.set()
        self.replay_udp.stop()
        self.capture.stop()
        if self._thread is not None:
            self._thread.join(timeout=3.0)

    def _set_recovery_overlay(
        self,
        recovery_state: str,
        *,
        status: str | None = None,
        clear_session: bool = False,
        error: str | None = None,
    ) -> None:
        """Publish connection progress without erasing the last cockpit image."""
        if recovery_state not in ("idle", "resyncing", "reconnecting"):
            raise ValueError("invalid recovery state")
        with self._lock:
            self._recovery_state = recovery_state
            connection = self._snapshot["connection"]
            connection["recoveryState"] = recovery_state
            if status is not None:
                connection["status"] = status
            if clear_session:
                connection["sessionId"] = "0"
            if error is None:
                connection.pop("error", None)
            else:
                connection["error"] = error
            self._snapshot["publishedAtUtc"] = utc_now()
            self._version += 1

    def request_live_resync(self) -> dict[str, Any]:
        if self.mode != "live":
            raise ValueError("resync is available only in live mode")
        with self._lock:
            connection = self._snapshot["connection"]
            if connection.get("sessionId") in (None, "0") or self._recovery_state == "reconnecting":
                raise ValueError("resync requires an active session")
            already_pending = self._soft_resync_live.is_set() or self._recovery_state == "resyncing"
        self._soft_resync_live.set()
        self._set_recovery_overlay("resyncing")
        return {"accepted": True, "action": "resync", "pending": already_pending}

    def request_live_reconnect(self) -> dict[str, Any]:
        if self.mode != "live":
            raise ValueError("reconnect is available only in live mode")
        already_pending = self._restart_live.is_set() or self._recovery_state == "reconnecting"
        self._soft_resync_live.clear()
        self._restart_live.set()
        self._set_recovery_overlay("reconnecting", status="Disconnected", clear_session=True)
        return {"accepted": True, "action": "reconnect", "pending": already_pending}

    def _open_live_socket(
        self,
        family: socket.AddressFamily,
        previous_endpoint: tuple[Any, ...] | None,
    ) -> tuple[socket.socket, tuple[Any, ...]]:
        """Open a fresh connected endpoint, never reusing the previous tuple."""
        rejected: list[socket.socket] = []
        try:
            for _ in range(4):
                sock = socket.socket(family, socket.SOCK_DGRAM)
                configure_live_udp_socket(sock)
                sock.settimeout(0.1)
                bind_address: tuple[Any, ...]
                bind_address = ("::", 0) if family == socket.AF_INET6 else ("0.0.0.0", 0)
                sock.bind(bind_address)
                sock.connect((self.host, self.port))
                endpoint = sock.getsockname()
                if previous_endpoint is None or endpoint != previous_endpoint:
                    return sock, endpoint
                # Keep the undesired port occupied while asking the OS for a
                # second ephemeral endpoint. This makes hard reconnects
                # observably distinct even on Windows, which readily reuses
                # recently closed UDP ports.
                rejected.append(sock)
            raise OSError("unable to allocate a fresh UDP endpoint")
        finally:
            for rejected_socket in rejected:
                rejected_socket.close()

    def _capture_checkpoint(self) -> None:
        if not self.capture.active or self.client is None or self.client.state.status != "Live":
            return
        metrics = self.capture.metrics()
        timeline_us = int(metrics["durationUs"])
        if timeline_us - self._capture_last_checkpoint_us < 5_000_000:
            return
        state = self.client.state
        mission = state.records.get("MISSION_STATE", {})
        if mission.get("mission_name"):
            self.capture.metadata(mission=mission["mission_name"])
        payload = json.dumps(
            {
                "sessionId": str(state.session_id),
                "baseline": state.baseline,
                "deltaSequence": state.delta_sequence,
                "manifestId": state.manifest_id,
                "requiredManifestId": state.required_manifest_id,
                "packetIndex": int(metrics["packetCount"]),
                "recordInstances": state.record_instances,
                "baselineRecordInstances": state.baseline_record_instances,
                "manifestRecords": state.manifest_records,
            },
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        self.capture.checkpoint(max(0, self._capture_session_index), timeline_us, payload)
        self._capture_last_checkpoint_us = timeline_us

    def _capture_observe_lifecycle(self, datagram: bytes) -> None:
        if not self.capture.active:
            return
        try:
            header = decoder.read_header(datagram)
        except decoder.DecodeFailure:
            return
        if header["message_type"] != 3 or header["session_id"] == 0 or header["session_id"] == self._capture_session_id:
            return
        self._capture_session_index += 1
        self._capture_session_id = int(header["session_id"])
        timeline_us = int(self.capture.metrics()["durationUs"])
        self.capture.session_boundary(self._capture_session_index, self._capture_session_id, timeline_us, "welcome")
        self._capture_last_checkpoint_us = timeline_us - 5_000_000

    def _receive_live_datagram(
        self, datagram: bytes, received_us: int, received_utc: str
    ) -> bool:
        if self.client is None:
            raise ValueError("live client is not initialized")
        changed = self.client.receive(datagram, received_us, received_utc)
        # Persist only datagrams accepted by the same decoder used by the live
        # dashboard. Valid incomplete fragments return False without raising
        # and remain eligible for capture.
        self.capture.packet(datagram, received_us, received_utc)
        self._capture_observe_lifecycle(datagram)
        self.quality.observe_packet(datagram)
        return changed

    def _run_live(self) -> None:
        family = socket.AF_INET6 if ":" in self.host else socket.AF_INET
        recovering_from_silence = False
        previous_endpoint: tuple[Any, ...] | None = None
        while not self._stop.is_set() and not self._mode_change.is_set():
            try:
                sock, previous_endpoint = self._open_live_socket(family, previous_endpoint)
                with sock:
                    self.client = fstl.ConsoleClient(sock, self.stale_us)
                    self.quality = QualityTracker(self.flight_hz, self.systems_hz, self.mission_heartbeat_ms)
                    self.client.begin()
                    at_us, at_utc = fstl.local_observation()
                    last_progress_us = at_us
                    if not recovering_from_silence:
                        self._publish(at_us, at_utc)
                    next_recovery_request_us = 0
                    while not self._stop.is_set() and not self._mode_change.is_set():
                        if self._restart_live.is_set():
                            self._restart_live.clear()
                            recovering_from_silence = True
                            break
                        at_us = fstl.now_us()
                        if self._soft_resync_live.is_set():
                            self._soft_resync_live.clear()
                            if self.client.state.session_begun and self.client.state.session_id:
                                self.client._resync(at_us)
                                next_recovery_request_us = at_us + fstl.RELIABLE_WINDOW_US
                        pending_resync_before_poll = self.client.pending_resync is not None
                        self.client.poll_reliable(at_us)
                        if (
                            pending_resync_before_poll
                            and self.client.pending_resync is None
                            and self.client.state.status == "Live"
                        ):
                            self._set_recovery_overlay("idle")
                        if (
                            self.client.state.status == "Disconnected"
                            and not self.client.state.welcomed
                        ):
                            if not recovering_from_silence:
                                _, disconnected_utc = fstl.local_observation()
                                self._publish(at_us, disconnected_utc)
                            break
                        changed = False
                        try:
                            datagram = sock.recv(fstl.MAX_DATAGRAM)
                            received_us, received_utc = fstl.local_observation()
                            changed = self._receive_live_datagram(
                                datagram, received_us, received_utc
                            )
                            if changed:
                                last_progress_us = received_us
                                self.quality.observe_state(self.client.state, received_us)
                                if self.client.state.status == "Live":
                                    recovering_from_silence = False
                                    self._recovery_state = (
                                        "resyncing" if self.client.pending_resync is not None else "idle"
                                    )
                                    next_recovery_request_us = 0
                                    self._publish(received_us, received_utc)
                                    self._capture_checkpoint()
                                elif not recovering_from_silence:
                                    self._publish(received_us, received_utc)
                        except socket.timeout:
                            pass
                        now, now_utc = fstl.local_observation()
                        previous = self.client.state.status
                        self.client.state.stale_if_needed(now, now_utc, self.stale_us)
                        if previous != self.client.state.status:
                            self._publish(now, now_utc)
                            if (
                                self.client.state.status == "Stale"
                                and self.client.state.stale_reason == "silence"
                            ):
                                recovering_from_silence = True
                                self._recovery_state = "resyncing"
                                self._set_recovery_overlay("resyncing", status="Stale")
                        needs_recovery = (
                            self.client.state.session_begun
                            and (
                                (
                                    self.client.state.status == "Stale"
                                    and self.client.state.stale_reason == "silence"
                                )
                                or (
                                    self.client.state.status == "Synchronizing"
                                    and not self.client.state.keyframe_applied
                                )
                            )
                        )
                        if needs_recovery:
                            recovery_due = (
                                self.client.state.status == "Stale"
                                or now - last_progress_us >= self.stale_us
                            )
                            if (
                                recovery_due
                                and now >= next_recovery_request_us
                                and self.client.pending_resync is None
                            ):
                                # Preserve the producer-side client identity
                                # across an intentional game pause.  The
                                # reliable request remains queued/retried until
                                # FSO resumes, and a fresh keyframe atomically
                                # returns the dashboard to Live.
                                self.client._resync(now)
                                next_recovery_request_us = now + fstl.RELIABLE_WINDOW_US
                            if now - last_progress_us >= self.recovery_reconnect_us:
                                recovering_from_silence = True
                                self._set_recovery_overlay(
                                    "reconnecting", status="Disconnected", clear_session=True
                                )
                                break
                        else:
                            next_recovery_request_us = 0
                        if self.client.terminal_published:
                            self._publish(now, now_utc)
                            break
            except (OSError, ValueError, decoder.DecodeFailure) as exc:
                if recovering_from_silence:
                    self._set_recovery_overlay(
                        "reconnecting", status="Disconnected", clear_session=True, error=str(exc)
                    )
                else:
                    with self._lock:
                        self._snapshot = self._empty_snapshot("Disconnected")
                        self._snapshot["connection"]["error"] = str(exc)
                        self._version += 1
            if self.capture.active and not self._stop.is_set() and not self._mode_change.is_set():
                continue
            if self._mode_change.wait(1.0):
                return
            if not self._stop.is_set():
                continue

    def _new_replay_client(self) -> fstl.ConsoleClient:
        client = fstl.ConsoleClient(None, 3_000_000)
        return client

    def _restore_replay_checkpoint(
        self,
        client: fstl.ConsoleClient,
        checkpoint: dict[str, Any],
        *,
        update_source_session: bool = True,
    ) -> None:
        saved = checkpoint["state"]
        state = client.state
        state.session_id = int(saved["sessionId"])
        state.baseline = int(saved["baseline"])
        state.delta_sequence = int(saved["deltaSequence"])
        state.manifest_id = int(saved["manifestId"])
        state.required_manifest_id = int(saved["requiredManifestId"])
        state.record_instances = copy.deepcopy(saved["recordInstances"])
        state.baseline_record_instances = copy.deepcopy(saved["baselineRecordInstances"])
        state.manifest_records = copy.deepcopy(saved["manifestRecords"])
        state.records = {}
        for identity in sorted(state.record_instances):
            record = state.record_instances[identity]
            state.records[record["recordName"]] = copy.deepcopy(record["fields"])
        state.baseline_records = {}
        for identity in sorted(state.baseline_record_instances):
            record = state.baseline_record_instances[identity]
            state.baseline_records[record["recordName"]] = copy.deepcopy(record["fields"])
        state.welcomed = True
        state.session_begun = True
        state.manifest_applied = state.required_manifest_id in (0, state.manifest_id)
        state.keyframe_applied = True
        state.status = "Live"
        if update_source_session:
            self._replay_source_session = state.session_id

    def _report_replay_failure(self, exc: Exception) -> None:
        """Keep the last valid replay image usable after a failed seek/load."""
        self.replay_state["seeking"] = False
        self.replay_state["previewing"] = False
        self.replay_state["previewPositionUs"] = None
        with self._lock:
            self._snapshot["connection"]["error"] = str(exc)
            self._snapshot["replay"] = copy.deepcopy(self.replay_state)
            self._snapshot["publishedAtUtc"] = utc_now()
            self._version += 1

    @staticmethod
    def _receive_replay_datagram(
        client: fstl.ConsoleClient,
        datagram: bytes,
        received_us: int,
        observed_utc: str,
    ) -> bool:
        """Apply a captured datagram without replaying raw packet-loss failure.

        A live capture can contain several incomplete fragmented messages. The
        live observer recovers by renegotiating, but replay owns validated state
        checkpoints and must not reproduce that transport outage. If a new
        group exhausts the four-message reassembly window, discard the oldest
        incomplete captured group and retry the already validated datagram.
        """
        fragment_keys_before = set(client.fragments)
        try:
            return client.receive(datagram, received_us, observed_utc)
        except ValueError as exc:
            if str(exc) != "reassembly quota":
                raise
            header = decoder.read_header(datagram)
            current_key = (int(header["session_id"]), int(header["message_id"]))
            if current_key in fragment_keys_before or len(fragment_keys_before) < 4:
                raise
            client.fragments.pop(current_key, None)
            oldest_key = min(
                fragment_keys_before,
                key=lambda key: client.fragments[key].first_seen_us,
            )
            client.fragments.pop(oldest_key, None)
            return client.receive(datagram, received_us, observed_utc)

    def _reconstruct_replay_client(
        self,
        capture: CaptureReader,
        target: int,
        *,
        preview: bool,
    ) -> fstl.ConsoleClient:
        client = self._new_replay_client()
        checkpoint = load_checkpoint(
            self.replay_path,
            capture.timeline_at(target - 1) if target else 0,
        ) if self.replay_path is not None else None
        start_index = 0
        if checkpoint is not None:
            self._restore_replay_checkpoint(
                client, checkpoint, update_source_session=not preview
            )
            start_index = min(
                target,
                int(
                    checkpoint["state"].get(
                        "packetIndex",
                        capture.index_at_time(int(checkpoint["timelineUs"])),
                    )
                ),
            )
        for seek_index in range(start_index, target):
            item = capture.packet(seek_index)
            client = self._prepare_replay_packet_client(
                client,
                item["datagram"],
                preview=preview,
                restart_udp=False,
            )
            self._receive_replay_datagram(
                client,
                item["datagram"],
                int(item["receivedMonotonicUs"]),
                item["observedAtUtc"],
            )
        return client

    def _run_replay(self) -> None:
        assert self.replay_path is not None
        try:
            with open_capture(self.replay_path) as capture:
                self._run_replay_capture(capture)
        except Exception as exc:
            self._report_replay_failure(exc)
            while not self._stop.is_set() and not self._mode_change.wait(0.1):
                pass

    def _run_replay_capture(self, capture: CaptureReader) -> None:
        try:
            header = capture.header
            self.replay_state["captureSchema"] = header.get("schema")
            self.replay_state["captureId"] = header.get("captureId")
            features = header.get("contractFeatures", [])
            self.replay_state["contractFeatures"] = (
                list(features) if isinstance(features, list) else []
            )
            self.replay_state["packetCount"] = capture.packet_count
            self.replay_state["durationUs"] = capture.duration_us
            capture_id = self.replay_state.get("captureId")
            self.replay_state["ranges"] = self.capture_library.ranges(str(capture_id)) if capture_id else []
            self.replay_state["sessionBoundaries"] = self.capture_library.sessions(str(capture_id)) if capture_id else []
            self._publish_replay_metadata()
            self.client = self._new_replay_client()
            self._replay_udp_client = self.client
            self._replay_source_session = 0
            self.quality = QualityTracker(self.flight_hz, self.systems_hz, self.mission_heartbeat_ms)
            previous_us: int | None = None
            while not self._stop.is_set() and not self._mode_change.is_set():
                if self._replay_preview_time_us is not None:
                    preview_us = min(
                        int(self.replay_state["durationUs"]),
                        max(0, self._replay_preview_time_us),
                    )
                    self._replay_preview_time_us = None
                    preview_target = capture.index_at_time(preview_us)
                    committed_client = self.client
                    try:
                        preview_client = self._reconstruct_replay_client(
                            capture, preview_target, preview=True
                        )
                        self.client = preview_client
                        self.replay_state["previewing"] = True
                        self.replay_state["previewPositionUs"] = preview_us
                        if preview_target:
                            preview_item = capture.packet(preview_target - 1)
                            self._publish(
                                int(preview_item["receivedMonotonicUs"]),
                                preview_item["observedAtUtc"],
                            )
                        else:
                            with self._lock:
                                self._snapshot = self._empty_snapshot("Synchronizing")
                                self._snapshot["replay"] = copy.deepcopy(self.replay_state)
                                self._version += 1
                    except Exception as exc:
                        self._report_replay_failure(exc)
                    finally:
                        self.client = committed_client
                    continue
                if self._replay_seek_time_us is not None:
                    requested_us = min(int(self.replay_state["durationUs"]), max(0, self._replay_seek_time_us))
                    self._replay_seek_time_us = None
                    self.replay_state["seeking"] = True
                    target = capture.index_at_time(requested_us)
                    self._replay_seek_target = target
                if self._replay_seek_target is not None:
                    target = min(capture.packet_count, max(0, self._replay_seek_target))
                    self._replay_seek_target = None
                    previous_client = self.client
                    previous_source_session = self._replay_source_session
                    try:
                        candidate_client = self._reconstruct_replay_client(
                            capture, target, preview=False
                        )
                    except Exception as exc:
                        self.client = previous_client
                        self._replay_source_session = previous_source_session
                        self._report_replay_failure(exc)
                        continue
                    self.replay_udp.restart_sessions()
                    self.client = candidate_client
                    self._replay_udp_client = self.client
                    self.quality = QualityTracker(
                        self.flight_hz, self.systems_hz, self.mission_heartbeat_ms
                    )
                    self.replay_state["position"] = target
                    self.replay_state["positionUs"] = capture.timeline_at(target - 1) if target else 0
                    self.replay_state["seeking"] = False
                    self.replay_state["previewing"] = False
                    self.replay_state["previewPositionUs"] = None
                    previous_us = None
                    if target:
                        item = capture.packet(target - 1)
                        self._publish(int(item["receivedMonotonicUs"]), item["observedAtUtc"])
                        self.replay_udp.state_available()
                    else:
                        with self._lock:
                            self._snapshot = self._empty_snapshot("Synchronizing")
                            self._snapshot["replay"] = copy.deepcopy(self.replay_state)
                            self._version += 1
                    continue
                if self.replay_state["position"] >= capture.packet_count:
                    if self.replay_state["playing"]:
                        self.replay_state["playing"] = False
                        self._publish_replay_metadata()
                    self._replay_wakeup.wait(0.1)
                    self._replay_wakeup.clear()
                    continue
                if not self.replay_state["playing"]:
                    self._replay_wakeup.wait(0.1)
                    self._replay_wakeup.clear()
                    previous_us = None
                    continue
                index = int(self.replay_state["position"])
                item = capture.packet(index)
                received_us = int(item["receivedMonotonicUs"])
                item_timeline_us = int(item.get("timelineUs", 0))
                active_range = next((item_range for item_range in self.replay_state["ranges"] if item_range["id"] == self.replay_state.get("activeRangeId")), None)
                if active_range is not None and item_timeline_us > int(active_range["endUs"]):
                    if self.replay_state.get("loop"):
                        self._replay_seek_time_us = int(active_range["startUs"])
                    else:
                        self.replay_state["playing"] = False
                        self._replay_seek_time_us = int(active_range["endUs"])
                    self._replay_wakeup.set()
                    continue
                if previous_us is not None:
                    delay = max(0.0, (item_timeline_us - previous_us) / 1_000_000.0)
                    deadline = time.monotonic() + delay
                    while not self._stop.is_set() and not self._mode_change.is_set() and time.monotonic() < deadline:
                        if self._replay_wakeup.wait(min(0.1, deadline - time.monotonic())):
                            self._replay_wakeup.clear()
                            break
                    if self._stop.is_set():
                        return
                    if (
                        not self.replay_state["playing"]
                        or self._replay_seek_target is not None
                        or self._replay_seek_time_us is not None
                        or self._replay_preview_time_us is not None
                    ):
                        previous_us = None
                        continue
                previous_us = item_timeline_us
                datagram = item["datagram"]
                observed_utc = item["observedAtUtc"]
                self.replay_state["position"] = index + 1
                self.replay_state["positionUs"] = item_timeline_us
                previous_client = self.client
                self.client = self._prepare_replay_packet_client(
                    self.client,
                    datagram,
                    preview=False,
                    restart_udp=True,
                )
                if self.client is not previous_client:
                    self._replay_udp_client = self.client
                    self.quality = QualityTracker(
                        self.flight_hz, self.systems_hz, self.mission_heartbeat_ms
                    )
                try:
                    self.quality.observe_packet(datagram)
                    changed = self._receive_replay_datagram(
                        self.client, datagram, received_us, observed_utc
                    )
                    self.replay_udp.observe_capture_datagram(datagram)
                    if changed:
                        self.quality.observe_state(self.client.state, received_us)
                        self._publish(received_us, observed_utc)
                        self.replay_udp.state_available()
                except Exception as exc:
                    self.replay_state["playing"] = False
                    previous_us = None
                    self._report_replay_failure(exc)
        except Exception:
            raise

    def replay_control(
        self,
        *,
        playing: bool | None = None,
        speed: float | None = None,
        position: int | None = None,
        position_us: int | None = None,
        active_range_id: str | None | object = ...,
        loop: bool | None = None,
    ) -> dict[str, Any]:
        if self.mode != "replay":
            raise ValueError("not in replay mode")
        if speed is not None:
            if speed != 1.0:
                raise ValueError("replay speed is fixed at 1.0")
        if playing is not None:
            self.replay_state["playing"] = playing
        if position is not None:
            if position < 0 or position > int(self.replay_state["packetCount"]):
                raise ValueError("replay position out of range")
            self._replay_seek_target = position
        if position_us is not None:
            if position_us < 0 or position_us > int(self.replay_state["durationUs"]):
                raise ValueError("replay time out of range")
            self._replay_seek_time_us = position_us
        if active_range_id is not ...:
            if active_range_id is not None and not any(item["id"] == active_range_id for item in self.replay_state["ranges"]):
                raise ValueError("capture range not found")
            self.replay_state["activeRangeId"] = active_range_id
            if active_range_id is not None:
                selected = next(item for item in self.replay_state["ranges"] if item["id"] == active_range_id)
                self._replay_seek_time_us = int(selected["startUs"])
        if loop is not None:
            self.replay_state["loop"] = loop
        self._publish_replay_metadata()
        self._replay_wakeup.set()
        return copy.deepcopy(self.replay_state)

    def replay_preview(self, position_us: int) -> dict[str, Any]:
        if self.mode != "replay":
            raise ValueError("not in replay mode")
        if position_us < 0 or position_us > int(self.replay_state["durationUs"]):
            raise ValueError("replay time out of range")
        self._replay_preview_time_us = position_us
        self._replay_wakeup.set()
        return {"accepted": True, "positionUs": position_us}

    def captures(self, query: str = "") -> list[dict[str, Any]]:
        return self.capture_library.list(query)

    def load_replay(self, capture_id: str) -> dict[str, Any]:
        path = self.capture_library.resolve(capture_id)
        if self.capture.active:
            raise ValueError("stop the active capture before loading a replay")
        with self._lock:
            self._replay_seek_target = None
            self._replay_seek_time_us = None
            self._replay_preview_time_us = None
            self._replay_source_session = 0
            self.mode = "replay"
            self.replay_path = path
            self.replay_state.update({
                "path": str(path), "captureId": capture_id, "playing": False,
                "speed": 1.0, "position": 0, "positionUs": 0, "packetCount": 0,
                "durationUs": 0, "seeking": False, "activeRangeId": None,
                "loop": False, "ranges": [], "sessionBoundaries": [],
            })
            self._snapshot = self._empty_snapshot("Synchronizing")
            self._version += 1
        self._mode_change.set()
        self._replay_wakeup.set()
        self.start()
        return copy.deepcopy(self.replay_state)

    def return_to_live(self) -> dict[str, Any]:
        self.replay_udp.stop()
        self._replay_udp_client = None
        with self._lock:
            self._replay_seek_target = None
            self._replay_seek_time_us = None
            self._replay_preview_time_us = None
            self._replay_source_session = 0
            self.mode = "live"
            self.replay_path = None
            self.replay_state.update({"path": None, "captureId": None, "playing": False, "position": 0, "positionUs": 0, "packetCount": 0, "durationUs": 0, "activeRangeId": None, "loop": False, "ranges": [], "sessionBoundaries": []})
            self._snapshot = self._empty_snapshot("Disconnected")
            self._version += 1
        self._mode_change.set()
        self._replay_wakeup.set()
        self.start()
        return {"mode": "live"}

    def _prepare_replay_packet_client(
        self,
        client: fstl.ConsoleClient,
        datagram: bytes,
        *,
        preview: bool,
        restart_udp: bool,
    ) -> fstl.ConsoleClient:
        try:
            header = decoder.read_header(datagram)
        except (ValueError, decoder.DecodeFailure):
            return client
        if header["message_type"] != 3 or header["fragment_count"] != 1:
            return client
        source_session = int(header["session_id"])
        if client.state.session_id not in (0, source_session):
            if restart_udp:
                self.replay_udp.restart_sessions()
            client = self._new_replay_client()
        if not preview:
            self._replay_source_session = source_session
        welcome = decoder.decode_message(
            3,
            header["flags"],
            datagram[fstl.HEADER_SIZE :],
            {"senderRole": "producer", "allowedSenderRoles": ["producer"]},
        )["fields"]
        client.state.hello_sent = True
        nonce = int(welcome["client_nonce"])
        t0 = int(welcome["client_send_t0_us"])
        client.state.hello_nonce = nonce
        client.state.hello_t0_us = t0
        client.hello_message_id = 1
        client.hello_payload_bytes = fstl.hello_payload(nonce, t0)
        client.hello_first_us = t0
        return client

    def replay_udp_settings(
        self, *, bind_host: str, port: int, lan_enabled: bool
    ) -> dict[str, Any]:
        if self.mode != "replay":
            raise ValueError("replay UDP is available only in replay mode")
        return self.replay_udp.configure(
            bind_host=bind_host, port=port, lan_enabled=lan_enabled
        )

    def start_replay_udp(self) -> dict[str, Any]:
        if self.mode != "replay":
            raise ValueError("replay UDP is available only in replay mode")
        result = self.replay_udp.start()
        self.replay_udp.state_available()
        return result

    def stop_replay_udp(self) -> dict[str, Any]:
        return self.replay_udp.stop()

    def create_range(self, capture_id: str, name: str, start_us: int, end_us: int) -> dict[str, Any]:
        result = self.capture_library.create_range(capture_id, name, start_us, end_us)
        if self.replay_state.get("captureId") == capture_id:
            self.replay_state["ranges"] = self.capture_library.ranges(capture_id)
            self._publish_replay_metadata()
        return result

    def update_range(self, capture_id: str, range_id: str, name: str, start_us: int, end_us: int) -> dict[str, Any]:
        result = self.capture_library.update_range(capture_id, range_id, name=name, start_us=start_us, end_us=end_us)
        if self.replay_state.get("captureId") == capture_id:
            self.replay_state["ranges"] = self.capture_library.ranges(capture_id)
            self._publish_replay_metadata()
        return result

    def delete_range(self, capture_id: str, range_id: str) -> None:
        self.capture_library.delete_range(capture_id, range_id)
        if self.replay_state.get("captureId") == capture_id:
            if self.replay_state.get("activeRangeId") == range_id:
                self.replay_state["activeRangeId"] = None
            self.replay_state["ranges"] = self.capture_library.ranges(capture_id)
            self._publish_replay_metadata()

    def _publish_replay_metadata(self) -> None:
        with self._lock:
            self._snapshot["replay"] = copy.deepcopy(self.replay_state)
            self._snapshot["publishedAtUtc"] = utc_now()
            self._version += 1

    def start_capture(self, *, name: str | None = None, expected_duration_us: int | None = None) -> Path:
        if self.mode != "live":
            raise ValueError("capture is available only in live mode")
        if expected_duration_us is not None and expected_duration_us <= 0:
            raise ValueError("expected duration must be positive")
        path = self.capture.start(
            {
                "name": name or f"Session {time.strftime('%Y-%m-%d %H:%M:%S')}",
                "expectedDurationUs": expected_duration_us,
                "telemetryHost": self.host,
                "telemetryPort": self.port,
                "flightHz": self.flight_hz,
                "systemsHz": self.systems_hz,
                "missionHeartbeatMs": self.mission_heartbeat_ms,
                "contractFeatures": [HUD_ALERT_CAPTURE_FEATURE],
            }
        )
        # Force a fresh negotiation so every user-started capture contains the
        # WELCOME, SESSION_BEGIN, manifest and keyframe needed for standalone replay.
        self._soft_resync_live.clear()
        self._capture_session_index = -1
        self._capture_session_id = 0
        self._capture_last_checkpoint_us = -5_000_000
        self._restart_live.set()
        self._set_recovery_overlay("reconnecting", status="Disconnected", clear_session=True)
        return path

    def stop_capture(self) -> Path | None:
        path = self.capture.stop()
        if self.client is not None:
            at_us, at_utc = fstl.local_observation()
            self._publish(at_us, at_utc)
        return path

    def export(self, export_dir: Path, catalog: list[dict[str, Any]]) -> dict[str, str]:
        export_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
        summary_path = export_dir / f"dashboard-summary-{stamp}.json"
        channels_path = export_dir / f"dashboard-channels-{stamp}.csv"
        observations_path = export_dir / f"dashboard-observations-{stamp}.json"
        snapshot = self.latest()
        summary_path.write_text(json.dumps(snapshot, indent=2, ensure_ascii=False), encoding="utf-8")
        with channels_path.open("w", encoding="utf-8", newline="") as stream:
            fieldnames = [
                "recordName",
                "identity",
                "producerSampleUs",
                "receivedMonotonicUs",
            ]
            writer = csv.DictWriter(stream, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(self.quality.history)
        observations = {
            "schema": "DashboardObservationExportV1",
            "createdAtUtc": utc_now(),
            "notAvailable": [
                {
                    "id": item["id"],
                    "label": item["label"],
                    "tab": item["tab"],
                    "reason": item.get("reason", "source-not-produced"),
                }
                for item in catalog
                if item.get("availability") == "nd"
                and (
                    not item.get("source", "").startswith("future:")
                    or not snapshot["records"].get(item["source"].split(":", 1)[1])
                )
            ],
            "invalid": [],
            "staleChannels": [
                channel
                for channel in snapshot["quality"]["channels"]
                if channel["ageUs"] is not None
                and channel["ageUs"] > (3_000_000 / max(channel["configuredHz"], 0.001) + 100_000)
            ],
            "interruptions": {
                "transportGapCount": snapshot["quality"]["transportGapCount"],
                "decodeErrorCount": snapshot["quality"]["decodeErrorCount"],
                "resyncCount": snapshot["quality"]["resyncCount"],
            },
        }
        observations_path.write_text(
            json.dumps(observations, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        return {
            "summary": str(summary_path),
            "channels": str(channels_path),
            "observations": str(observations_path),
        }
