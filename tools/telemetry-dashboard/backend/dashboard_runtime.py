"""Runtime, measurement, capture and replay services for the simpit dashboard."""

from __future__ import annotations

import base64
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


LEGACY_CAPTURE_SCHEMA = "FSTL-dashboard-capture-v1"
CAPTURE_SCHEMA = "FSTL-dashboard-capture-v2"
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


class CaptureWriter:
    def __init__(self, capture_dir: Path) -> None:
        self.capture_dir = capture_dir
        self.capture_dir.mkdir(parents=True, exist_ok=True)
        self.path: Path | None = None
        self._stream: Any = None
        self._lock = threading.RLock()

    @property
    def active(self) -> bool:
        return self._stream is not None

    def start(self, metadata: dict[str, Any]) -> Path:
        with self._lock:
            self.stop()
            stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
            self.path = self.capture_dir / f"telemetry-{stamp}.fstlcap.jsonl"
            self._stream = self.path.open("w", encoding="utf-8", newline="\n")
            self._stream.write(
                json.dumps(
                    {
                        "schema": CAPTURE_SCHEMA,
                        "kind": "header",
                        "createdAtUtc": utc_now(),
                        **metadata,
                    },
                    separators=(",", ":"),
                )
                + "\n"
            )
            self._stream.flush()
            return self.path

    def stop(self) -> Path | None:
        with self._lock:
            path = self.path
            if self._stream is not None:
                self._stream.flush()
                self._stream.close()
            self._stream = None
            return path

    def packet(self, datagram: bytes, monotonic_us: int, observed_at_utc: str) -> None:
        with self._lock:
            if self._stream is None:
                return
            self._stream.write(
                json.dumps(
                    {
                        "kind": "datagram",
                        "observedAtUtc": observed_at_utc,
                        "receivedMonotonicUs": str(monotonic_us),
                        "datagramBase64": base64.b64encode(datagram).decode("ascii"),
                    },
                    separators=(",", ":"),
                )
                + "\n"
            )
            self._stream.flush()


def load_capture(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    header: dict[str, Any] | None = None
    packets: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            item = json.loads(line)
            if line_number == 1:
                if item.get("schema") not in (LEGACY_CAPTURE_SCHEMA, CAPTURE_SCHEMA) or item.get("kind") != "header":
                    raise ValueError("capture header is not a supported FSTL dashboard capture")
                header = item
                continue
            if item.get("kind") != "datagram":
                raise ValueError(f"invalid capture item on line {line_number}")
            datagram = base64.b64decode(item["datagramBase64"], validate=True)
            packets.append(
                {
                    **item,
                    "datagram": datagram,
                    "receivedMonotonicUs": int(item["receivedMonotonicUs"]),
                }
            )
    if header is None:
        raise ValueError("empty capture")
    return header, packets


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
    ) -> None:
        self.host = host
        self.port = port
        self.flight_hz = flight_hz
        self.systems_hz = systems_hz
        self.mission_heartbeat_ms = mission_heartbeat_ms
        self.capture = CaptureWriter(capture_dir)
        self.replay_path = replay_path
        self.stale_us = stale_us
        self.recovery_reconnect_us = recovery_reconnect_us
        self.quality = QualityTracker(flight_hz, systems_hz, mission_heartbeat_ms)
        self.client: fstl.ConsoleClient | None = None
        self.mode = "replay" if replay_path else "live"
        self.replay_state = {
            "path": str(replay_path) if replay_path else None,
            "playing": bool(replay_path),
            "speed": 1.0,
            "position": 0,
            "packetCount": 0,
            "captureSchema": None,
            "contractFeatures": [],
        }
        self._snapshot = self._empty_snapshot("Synchronizing" if replay_path else "Disconnected")
        self._version = 0
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._replay_wakeup = threading.Event()
        self._restart_live = threading.Event()
        self._replay_seek_target: int | None = None
        self._thread: threading.Thread | None = None

    @property
    def version(self) -> int:
        with self._lock:
            return self._version

    def latest(self) -> dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._snapshot)

    def _empty_snapshot(self, status: str) -> dict[str, Any]:
        return {
            "schema": SNAPSHOT_SCHEMA,
            "publishedAtUtc": utc_now(),
            "mode": self.mode,
            "connection": {
                "status": status,
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
            "capture": {"active": False, "path": None},
            "replay": copy.deepcopy(self.replay_state),
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
            "recordInstances": copy.deepcopy(state.record_instances),
            "manifest": {
                "id": state.manifest_id,
                "records": copy.deepcopy(state.manifest_records),
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
            "capture": {"active": self.capture.active, "path": str(self.capture.path) if self.capture.path else None},
            "replay": copy.deepcopy(self.replay_state),
        }
        with self._lock:
            self._snapshot = snapshot
            self._version += 1

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        target = self._run_replay if self.replay_path else self._run_live
        self._thread = threading.Thread(target=target, name="telemetry-dashboard-runtime", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._replay_wakeup.set()
        self.capture.stop()
        if self._thread is not None:
            self._thread.join(timeout=3.0)

    def _run_live(self) -> None:
        family = socket.AF_INET6 if ":" in self.host else socket.AF_INET
        recovering_from_silence = False
        while not self._stop.is_set():
            try:
                with socket.socket(family, socket.SOCK_DGRAM) as sock:
                    configure_live_udp_socket(sock)
                    sock.settimeout(0.1)
                    sock.connect((self.host, self.port))
                    self.client = fstl.ConsoleClient(sock, self.stale_us)
                    self.quality = QualityTracker(self.flight_hz, self.systems_hz, self.mission_heartbeat_ms)
                    self.client.begin()
                    at_us, at_utc = fstl.local_observation()
                    if not recovering_from_silence:
                        self._publish(at_us, at_utc)
                    synchronizing_since_us: int | None = None
                    next_recovery_request_us = 0
                    while not self._stop.is_set():
                        if self._restart_live.is_set():
                            self._restart_live.clear()
                            break
                        at_us = fstl.now_us()
                        self.client.poll_reliable(at_us)
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
                            self.capture.packet(datagram, received_us, received_utc)
                            self.quality.observe_packet(datagram)
                            changed = self.client.receive(datagram, received_us, received_utc)
                            if changed:
                                self.quality.observe_state(self.client.state, received_us)
                                if self.client.state.status == "Live":
                                    recovering_from_silence = False
                                    synchronizing_since_us = None
                                    next_recovery_request_us = 0
                                    self._publish(received_us, received_utc)
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
                            if synchronizing_since_us is None:
                                synchronizing_since_us = now
                            recovery_due = (
                                self.client.state.status == "Stale"
                                or now - synchronizing_since_us >= self.stale_us
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
                            if (
                                now - synchronizing_since_us
                                >= self.recovery_reconnect_us
                            ):
                                # Some producer-side slots acknowledge RESYNC
                                # after a focus loss but never schedule another
                                # keyframe.  Preserve the published stale image
                                # and rotate to a fresh endpoint.  If FSO is
                                # still paused, this bounded cycle repeats
                                # until one negotiation can publish Live state.
                                recovering_from_silence = True
                                break
                        else:
                            synchronizing_since_us = None
                            next_recovery_request_us = 0
                        if self.client.terminal_published:
                            self._publish(now, now_utc)
                            break
            except (OSError, ValueError, decoder.DecodeFailure) as exc:
                if not recovering_from_silence:
                    with self._lock:
                        self._snapshot = self._empty_snapshot("Disconnected")
                        self._snapshot["connection"]["error"] = str(exc)
                        self._version += 1
            if self.capture.active and not self._stop.is_set():
                continue
            if not self._stop.wait(1.0):
                continue

    def _new_replay_client(self) -> fstl.ConsoleClient:
        client = fstl.ConsoleClient(None, 3_000_000)
        return client

    def _prepare_replay_handshake(self, client: fstl.ConsoleClient, packets: list[dict[str, Any]]) -> None:
        for item in packets:
            datagram = item["datagram"]
            header = decoder.read_header(datagram)
            if header["message_type"] != 3 or header["fragment_count"] != 1:
                continue
            welcome = decoder.decode_message(
                3,
                header["flags"],
                datagram[fstl.HEADER_SIZE :],
                {"senderRole": "producer", "allowedSenderRoles": ["producer"]},
            )["fields"]
            nonce, t0 = int(welcome["client_nonce"]), int(welcome["client_send_t0_us"])
            client.state.hello_sent = True
            client.state.hello_nonce = nonce
            client.state.hello_t0_us = t0
            client.hello_message_id = 1
            client.hello_payload_bytes = fstl.hello_payload(nonce, t0)
            client.hello_first_us = t0
            return

    def _run_replay(self) -> None:
        assert self.replay_path is not None
        try:
            header, packets = load_capture(self.replay_path)
            self.replay_state["captureSchema"] = header.get("schema")
            features = header.get("contractFeatures", [])
            self.replay_state["contractFeatures"] = (
                list(features) if isinstance(features, list) else []
            )
            self.replay_state["packetCount"] = len(packets)
            self.client = self._new_replay_client()
            self._prepare_replay_handshake(self.client, packets)
            self.quality = QualityTracker(self.flight_hz, self.systems_hz, self.mission_heartbeat_ms)
            previous_us: int | None = None
            while not self._stop.is_set():
                if self._replay_seek_target is not None:
                    target = min(len(packets), max(0, self._replay_seek_target))
                    self._replay_seek_target = None
                    self.client = self._new_replay_client()
                    self._prepare_replay_handshake(self.client, packets)
                    self.quality = QualityTracker(
                        self.flight_hz, self.systems_hz, self.mission_heartbeat_ms
                    )
                    for seek_index in range(target):
                        item = packets[seek_index]
                        received_us = int(item["receivedMonotonicUs"])
                        self.quality.observe_packet(item["datagram"])
                        if self.client.receive(item["datagram"], received_us, item["observedAtUtc"]):
                            self.quality.observe_state(self.client.state, received_us)
                    self.replay_state["position"] = target
                    previous_us = None
                    if target:
                        item = packets[target - 1]
                        self._publish(int(item["receivedMonotonicUs"]), item["observedAtUtc"])
                    continue
                if self.replay_state["position"] >= len(packets):
                    self.replay_state["playing"] = False
                    self._replay_wakeup.wait(0.1)
                    self._replay_wakeup.clear()
                    continue
                if not self.replay_state["playing"]:
                    self._replay_wakeup.wait(0.1)
                    self._replay_wakeup.clear()
                    previous_us = None
                    continue
                index = int(self.replay_state["position"])
                item = packets[index]
                received_us = int(item["receivedMonotonicUs"])
                if previous_us is not None:
                    delay = max(0.0, (received_us - previous_us) / 1_000_000.0 / self.replay_state["speed"])
                    deadline = time.monotonic() + delay
                    while not self._stop.is_set() and time.monotonic() < deadline:
                        if self._replay_wakeup.wait(min(0.1, deadline - time.monotonic())):
                            self._replay_wakeup.clear()
                            break
                    if self._stop.is_set():
                        return
                previous_us = received_us
                datagram = item["datagram"]
                observed_utc = item["observedAtUtc"]
                self.quality.observe_packet(datagram)
                if self.client.receive(datagram, received_us, observed_utc):
                    self.quality.observe_state(self.client.state, received_us)
                    self._publish(received_us, observed_utc)
                self.replay_state["position"] = index + 1
        except (OSError, ValueError, KeyError, json.JSONDecodeError, decoder.DecodeFailure) as exc:
            with self._lock:
                self._snapshot = self._empty_snapshot("Disconnected")
                self._snapshot["connection"]["error"] = str(exc)
                self._version += 1

    def replay_control(
        self,
        *,
        playing: bool | None = None,
        speed: float | None = None,
        position: int | None = None,
    ) -> dict[str, Any]:
        if self.mode != "replay":
            raise ValueError("not in replay mode")
        if speed is not None:
            if speed not in (0.25, 0.5, 1.0, 2.0, 4.0):
                raise ValueError("unsupported replay speed")
            self.replay_state["speed"] = speed
        if playing is not None:
            self.replay_state["playing"] = playing
        if position is not None:
            if position < 0 or position > int(self.replay_state["packetCount"]):
                raise ValueError("replay position out of range")
            self._replay_seek_target = position
        self._replay_wakeup.set()
        return copy.deepcopy(self.replay_state)

    def start_capture(self) -> Path:
        if self.mode != "live":
            raise ValueError("capture is available only in live mode")
        path = self.capture.start(
            {
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
        self._restart_live.set()
        at_us, at_utc = fstl.local_observation()
        self._publish(at_us, at_utc)
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
