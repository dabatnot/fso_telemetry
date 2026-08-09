"""Independent FSTL 1.1 replay producer for physical cockpit clients."""

from __future__ import annotations

import hashlib
import secrets
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable

import fstl_client_core as fstl
import fstl_reference_decoder as decoder


MAX_CLIENTS = 4
RETRY_US = 250_000
RELIABLE_LIFETIME_US = 5_000_000
HEARTBEAT_US = 1_000_000
PEER_IDLE_TIMEOUT_US = 10_000_000
PAUSED_STATE_REFRESH_US = 2_000_000
CAPTURED_FRAGMENT_LIMIT = 4


def _transaction_parts(
    records: dict[str, dict[str, Any]], max_region_bytes: int = 900_000
) -> list[tuple[bytes, int]]:
    parts: list[tuple[bytes, int]] = []
    current = bytearray()
    count = 0
    for identity in sorted(records):
        value = records[identity].get("_encodedRecordHex")
        if not isinstance(value, str):
            raise ValueError("capture does not retain a validated record envelope")
        encoded = bytes.fromhex(value)
        if current and len(current) + len(encoded) > max_region_bytes:
            parts.append((bytes(current), count))
            current.clear()
            count = 0
        current.extend(encoded)
        count += 1
    if current:
        parts.append((bytes(current), count))
    if not parts or len(parts) > 64:
        raise ValueError("replay transaction cannot be represented in FSTL")
    return parts


def _mission_time(state: fstl.ConsoleState) -> int:
    values = [
        int(record["fields"].get("producer_sample_time_us", 0))
        for record in state.record_instances.values()
    ]
    return max(values, default=0)


@dataclass
class PendingMessage:
    message_type: int
    base_flags: int
    frame_id: int
    mission_time_us: int
    message_id: int
    payload: bytes
    first_sent_us: int
    next_send_us: int


@dataclass
class ReplayPeer:
    endpoint: tuple[str, int]
    session_id: int
    clock_offset_us: int = 0
    packet_sequence: int = 0
    message_id: int = 0
    frame_id: int = 0
    session_started: bool = False
    baseline: int = 0
    source_baseline: int = 0
    delta_sequence: int = 0
    manifest_id: int = 0
    last_received_us: int = 0
    last_heartbeat_us: int = 0
    last_state_refresh_us: int = 0
    pending: dict[int, PendingMessage] = field(default_factory=dict)


@dataclass
class CapturedFragments:
    header: dict[str, int]
    first_seen_us: int
    pieces: dict[int, bytes] = field(default_factory=dict)


class ReplayUdpProducer:
    """Serve reconstructed FSTL state with one transport session per peer."""

    def __init__(
        self,
        state_provider: Callable[[], fstl.ConsoleState | None],
        position_provider: Callable[[], int],
        state_changed: Callable[[dict[str, Any]], None],
        playing_provider: Callable[[], bool] | None = None,
    ) -> None:
        self._state_provider = state_provider
        self._position_provider = position_provider
        self._state_changed = state_changed
        self._playing_provider = playing_provider or (lambda: False)
        self._socket: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._lock = threading.RLock()
        self._peers: dict[tuple[str, int], ReplayPeer] = {}
        self._retired: dict[tuple[tuple[str, int], int], ReplayPeer] = {}
        self._captured: dict[tuple[int, int], CapturedFragments] = {}
        self._settings = {"bindHost": "127.0.0.1", "port": 0, "lanEnabled": False}
        self._error: str | None = None
        self._refused_clients = 0
        self._rejected_commands = 0
        clock_now = fstl.now_us()
        self._clock_source_position_us = max(0, int(self._position_provider()))
        self._clock_position_us = self._clock_source_position_us
        self._clock_observed_us = clock_now
        self._clock_playing = bool(self._playing_provider())

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            endpoint = None
            if self._socket is not None:
                host, port = self._socket.getsockname()[:2]
                endpoint = f"{host}:{port}"
            return {
                "running": self._socket is not None,
                **self._settings,
                "clientCount": len(self._peers),
                "maxClients": MAX_CLIENTS,
                "endpoint": endpoint,
                "error": self._error,
                "refusedClients": self._refused_clients,
                "rejectedCommands": self._rejected_commands,
            }

    def configure(self, *, bind_host: str, port: int, lan_enabled: bool) -> dict[str, Any]:
        if not 0 <= port <= 65535:
            raise ValueError("UDP port is out of range")
        if not lan_enabled and bind_host not in ("127.0.0.1", "::1", "localhost"):
            raise ValueError("LAN must be enabled before binding a non-loopback interface")
        with self._lock:
            if self._socket is not None:
                raise ValueError("stop the replay UDP server before changing its bind settings")
            self._settings = {"bindHost": bind_host, "port": port, "lanEnabled": lan_enabled}
            return self.snapshot()

    def start(self) -> dict[str, Any]:
        with self._lock:
            if self._socket is not None:
                return self.snapshot()
            family = socket.AF_INET6 if ":" in self._settings["bindHost"] else socket.AF_INET
            sock = socket.socket(family, socket.SOCK_DGRAM)
            try:
                sock.bind((self._settings["bindHost"], self._settings["port"]))
                sock.settimeout(0.05)
            except OSError as exc:
                sock.close()
                self._error = str(exc)
                self._notify()
                raise
            self._socket = sock
            self._error = None
            self._stop.clear()
            self._thread = threading.Thread(target=self._run, name="fstl-replay-udp", daemon=True)
            self._thread.start()
            result = self.snapshot()
        self._notify()
        return result

    def stop(self) -> dict[str, Any]:
        with self._lock:
            sock = self._socket
            self._socket = None
            self._stop.set()
            self._peers.clear()
            self._retired.clear()
            self._captured.clear()
        if sock is not None:
            sock.close()
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1.0)
        self._thread = None
        self._notify()
        return self.snapshot()

    def restart_sessions(self) -> None:
        """Terminate all transports before a seek, loop or source-session boundary."""
        with self._lock:
            peers = list(self._peers.values())
            now = fstl.now_us()
            sample = self._virtual_position(now)
            for peer in peers:
                if peer.session_started:
                    payload = struct.pack("<BBHIQ", 4, 1, 0, peer.baseline, sample)
                    self._send(peer, 13, payload, reliable=True, mission_time_us=0, frame_id=0, now_us=now)
                    self._retired[(peer.endpoint, peer.session_id)] = peer
            self._peers.clear()
            self._captured.clear()
        self._notify()

    def state_available(self) -> None:
        with self._lock:
            state = self._state_provider()
            for peer in list(self._peers.values()):
                if not peer.session_started:
                    self._begin_session(peer, fstl.now_us())
                elif state is not None and (
                    state.baseline != peer.source_baseline
                    or state.manifest_id != peer.manifest_id
                ):
                    self._send_snapshot(peer, fstl.now_us(), snapshot_flag=2)

    def synchronize_clock(self) -> None:
        """Anchor the virtual producer clock to an authoritative playhead."""
        with self._lock:
            now = fstl.now_us()
            position = max(0, int(self._position_provider()))
            self._clock_source_position_us = position
            self._clock_position_us = position
            self._clock_observed_us = now
            self._clock_playing = bool(self._playing_provider())

    def observe_capture_datagram(self, datagram: bytes) -> None:
        """Re-emit validated delta/event messages after transport reassembly."""
        try:
            header = decoder.read_header(datagram)
        except (ValueError, decoder.DecodeFailure):
            return
        message_type = int(header["message_type"])
        if message_type not in (7, 8):
            return
        key = (int(header["session_id"]), int(header["message_id"]))
        now = fstl.now_us()
        with self._lock:
            # A joining peer receives a synthetic snapshot for the current
            # cursor, so retaining source fragments while no peer is listening
            # has no value and can only grow memory on an incomplete capture.
            if self._socket is None or not self._peers:
                self._captured.clear()
                return
            expired = [
                captured_key
                for captured_key, captured in self._captured.items()
                if now - captured.first_seen_us >= RELIABLE_LIFETIME_US
            ]
            for captured_key in expired:
                self._captured.pop(captured_key, None)
            group = self._captured.get(key)
            if group is None:
                if len(self._captured) >= CAPTURED_FRAGMENT_LIMIT:
                    oldest_key = min(
                        self._captured,
                        key=lambda captured_key: self._captured[captured_key].first_seen_us,
                    )
                    self._captured.pop(oldest_key, None)
                group = CapturedFragments(header, now)
                self._captured[key] = group
            group.pieces[int(header["fragment_index"])] = datagram[fstl.HEADER_SIZE :]
            if len(group.pieces) != int(header["fragment_count"]):
                return
            payload = b"".join(group.pieces[index] for index in range(int(header["fragment_count"])))
            self._captured.pop(key, None)
            if decoder.crc32_iso_hdlc(payload) != int(header["message_crc32"]):
                return
            for peer in list(self._peers.values()):
                if not peer.session_started:
                    self._begin_session(peer, now)
                if not peer.session_started:
                    continue
                if message_type == 7:
                    baseline, sequence = struct.unpack_from("<II", payload)
                    if baseline != peer.source_baseline:
                        self._send_snapshot(peer, now, snapshot_flag=4)
                        continue
                    peer.delta_sequence += 1
                    payload = struct.pack("<II", peer.baseline, peer.delta_sequence) + payload[8:]
                    reliable = False
                else:
                    reliable = len(payload) > 20 and payload[20] == 2
                self._send(
                    peer,
                    message_type,
                    payload,
                    reliable=reliable,
                    mission_time_us=int(header["mission_time_us"]),
                    frame_id=self._next_frame(peer),
                    now_us=now,
                )

    def _notify(self) -> None:
        try:
            self._state_changed(self.snapshot())
        except Exception:
            pass

    def _run(self) -> None:
        while not self._stop.is_set():
            sock = self._socket
            if sock is None:
                return
            try:
                datagram, endpoint = sock.recvfrom(fstl.MAX_DATAGRAM)
                self._receive(datagram, (str(endpoint[0]), int(endpoint[1])))
            except socket.timeout:
                pass
            except OSError as exc:
                if not self._stop.is_set():
                    with self._lock:
                        self._error = str(exc)
                    self._notify()
                return
            self._poll(fstl.now_us())

    def _receive(self, datagram: bytes, endpoint: tuple[str, int]) -> None:
        try:
            header = decoder.read_header(datagram)
            message_type = int(header["message_type"])
            decoder.decode_transport_sequence(
                [datagram],
                "replay-client-ingress",
                {
                    "acceptedMinorRange": [0, 1]
                    if message_type == 2
                    else [1, 1]
                },
            )
            payload = datagram[fstl.HEADER_SIZE :]
            decoded = decoder.decode_message(
                message_type,
                int(header["flags"]),
                payload,
                {"senderRole": "client", "allowedSenderRoles": ["client"]},
            )["fields"]
        except (ValueError, KeyError, decoder.DecodeFailure):
            return
        now = fstl.now_us()
        with self._lock:
            if message_type == 2:
                supports_v11 = (
                    int(decoded["min_major"]) <= 1 <= int(decoded["max_major"])
                    and int(decoded["min_minor"]) <= 1 <= int(decoded["max_minor"])
                )
                if not supports_v11:
                    self._refused_clients += 1
                    self._send_rejected_welcome(endpoint, decoded, now, status=1)
                    self._notify()
                    return
                peer = self._peers.get(endpoint)
                if peer is None and len(self._peers) >= MAX_CLIENTS:
                    self._refused_clients += 1
                    self._send_rejected_welcome(endpoint, decoded, now, status=3)
                    self._notify()
                    return
                peer = ReplayPeer(
                    endpoint,
                    secrets.randbits(63) or 1,
                    clock_offset_us=now - self._virtual_position(now),
                    last_received_us=now,
                )
                self._peers[endpoint] = peer
                self._send_welcome(peer, decoded, now)
                self._begin_session(peer, now)
                self._notify()
                return
            peer = self._peers.get(endpoint)
            if peer is None or int(header["session_id"]) != peer.session_id:
                retired = self._retired.get((endpoint, int(header["session_id"])))
                if retired is None or message_type != 10:
                    return
                target = int(decoded["target_message_id"])
                if int(decoded["ack_flags"]) & fstl.ACK_VALIDATED:
                    retired.pending.pop(target, None)
                return
            peer.last_received_us = now
            if message_type == 10:
                target = int(decoded["target_message_id"])
                if int(decoded["ack_flags"]) & fstl.ACK_VALIDATED:
                    peer.pending.pop(target, None)
            elif message_type == 11:
                target = int(decoded["target_message_id"])
                pending = peer.pending.get(target)
                if pending is not None:
                    pending.next_send_us = now
            elif message_type == 12:
                self._send(
                    peer,
                    10,
                    fstl.ack_payload(header, fstl.ACK_APPLIED),
                    reliable=False,
                    mission_time_us=0,
                    frame_id=0,
                    now_us=now,
                )
                self._send_snapshot(peer, now, snapshot_flag=4)
            elif message_type == 9:
                pass
            else:
                self._rejected_commands += 1
                self._notify()

    def _send_rejected_welcome(
        self,
        endpoint: tuple[str, int],
        hello: dict[str, Any],
        now: int,
        *,
        status: int,
    ) -> None:
        payload = self._welcome_payload(hello, now, status=status)
        self._send_datagrams(
            endpoint,
            3,
            0,
            0,
            0,
            1,
            payload,
            now,
            version_minor=0 if status == 1 else 1,
        )

    def _send_welcome(self, peer: ReplayPeer, hello: dict[str, Any], now: int) -> None:
        payload = self._welcome_payload(hello, now, status=0)
        self._send(peer, 3, payload, reliable=True, mission_time_us=0, frame_id=0, now_us=now)

    @staticmethod
    def _welcome_payload(hello: dict[str, Any], now: int, *, status: int) -> bytes:
        return struct.pack(
            "<QQQQBBBBQQHHHHQ",
            int(hello["client_nonce"]),
            int(hello["client_send_t0_us"]),
            now,
            now,
            status,
            1 if status == 0 else 0,
            1 if status == 0 else 0,
            int(hello.get("requested_visibility_mode", 0)) if status == 0 else 0,
            0,
            0,
            1000 if status == 0 else 0,
            5000 if status == 0 else 0,
            0,
            0,
            0x4653544C5245504C,
        )

    def _begin_session(self, peer: ReplayPeer, now: int) -> None:
        state = self._state_provider()
        if state is None or not state.record_instances or not state.baseline:
            return
        mission = state.records.get("MISSION_STATE", {})
        mission_id = int(mission.get("mission_instance_id", 0))
        payload = struct.pack(
            "<IQQII", 0, self._wire_time(peer), mission_id, 1, state.manifest_id
        )
        self._send(peer, 4, payload, reliable=True, mission_time_us=0, frame_id=0, now_us=now)
        peer.session_started = True
        if state.manifest_id and state.manifest_records:
            self._send_manifest(peer, state, now)
        self._send_snapshot(peer, now, snapshot_flag=1)

    def _send_manifest(self, peer: ReplayPeer, state: fstl.ConsoleState, now: int) -> None:
        parts = _transaction_parts(state.manifest_records)
        transaction = b"".join(region for region, _ in parts)
        transaction_hash = hashlib.sha256(transaction).digest()
        for part_index, (region, count) in enumerate(parts):
            payload = struct.pack(
                "<IHHI32sQHH",
                state.manifest_id,
                part_index,
                len(parts),
                len(transaction),
                transaction_hash,
                _mission_time(state),
                1,
                count,
            ) + region
            self._send(peer, 5, payload, reliable=True, mission_time_us=0, frame_id=0, now_us=now)
        peer.manifest_id = state.manifest_id

    def _send_snapshot(self, peer: ReplayPeer, now: int, *, snapshot_flag: int) -> None:
        state = self._state_provider()
        if state is None or not state.record_instances or not state.baseline:
            return
        if state.manifest_id != peer.manifest_id and state.manifest_records:
            self._send_manifest(peer, state, now)
        parts = _transaction_parts(state.record_instances)
        transaction = b"".join(region for region, _ in parts)
        transaction_hash = hashlib.sha256(transaction).digest()
        sample = _mission_time(state)
        peer.baseline = (peer.baseline + 1) & 0xFFFFFFFF
        if peer.baseline == 0:
            peer.baseline = 1
        frame_id = self._next_frame(peer)
        for part_index, (region, count) in enumerate(parts):
            payload = struct.pack(
                "<IHHI32sQIHH",
                peer.baseline,
                part_index,
                len(parts),
                len(transaction),
                transaction_hash,
                sample,
                state.manifest_id,
                snapshot_flag,
                count,
            ) + region
            self._send(
                peer,
                6,
                payload,
                reliable=True,
                mission_time_us=sample,
                frame_id=frame_id,
                now_us=now,
                extra_flags=0x04,
            )
        peer.source_baseline = state.baseline
        peer.delta_sequence = 0
        peer.last_state_refresh_us = now

    @staticmethod
    def _next_frame(peer: ReplayPeer) -> int:
        peer.frame_id = (peer.frame_id + 1) & 0xFFFFFFFF
        return peer.frame_id or 1

    def _send(
        self,
        peer: ReplayPeer,
        message_type: int,
        payload: bytes,
        *,
        reliable: bool,
        mission_time_us: int,
        frame_id: int,
        now_us: int,
        extra_flags: int = 0,
    ) -> None:
        peer.message_id = (peer.message_id + 1) & 0xFFFFFFFF
        if peer.message_id == 0:
            peer.message_id = 1
        flags = extra_flags | (fstl.ACK_REQUIRED if reliable else 0)
        self._send_datagrams(
            peer.endpoint,
            message_type,
            flags,
            peer.session_id,
            frame_id,
            peer.message_id,
            payload,
            self._wire_time(peer),
            peer,
            mission_time_us,
        )
        if reliable:
            peer.pending[peer.message_id] = PendingMessage(
                message_type,
                flags,
                frame_id,
                mission_time_us,
                peer.message_id,
                payload,
                now_us,
                now_us + RETRY_US,
            )

    def _send_datagrams(
        self,
        endpoint: tuple[str, int],
        message_type: int,
        flags: int,
        session_id: int,
        frame_id: int,
        message_id: int,
        payload: bytes,
        sent_us: int,
        peer: ReplayPeer | None = None,
        mission_time_us: int = 0,
        version_minor: int = 1,
    ) -> None:
        sock = self._socket
        if sock is None:
            return
        message_crc = decoder.crc32_iso_hdlc(payload)
        fragment_count = max(1, (len(payload) + decoder.MAX_FRAGMENT_PAYLOAD - 1) // decoder.MAX_FRAGMENT_PAYLOAD)
        base_flags = flags | (1 if fragment_count > 1 else 0)
        for fragment_index in range(fragment_count):
            offset = fragment_index * decoder.MAX_FRAGMENT_PAYLOAD
            fragment = payload[offset : offset + decoder.MAX_FRAGMENT_PAYLOAD]
            sequence = 0
            if peer is not None:
                peer.packet_sequence = (peer.packet_sequence + 1) & 0xFFFFFFFF
                sequence = peer.packet_sequence
            prefix = struct.pack(
                "<IBBBBHHQIIqQIHHIII",
                fstl.MAGIC,
                1,
                version_minor,
                message_type,
                base_flags,
                fstl.HEADER_SIZE,
                len(fragment),
                session_id,
                sequence,
                frame_id,
                mission_time_us,
                sent_us,
                message_id,
                fragment_index,
                fragment_count,
                len(payload),
                offset,
                message_crc,
            )
            checksum = decoder.crc32_iso_hdlc(prefix + bytes(4) + fragment)
            sock.sendto(prefix + struct.pack("<I", checksum) + fragment, endpoint)

    def _poll(self, now: int) -> None:
        peers_changed = False
        with self._lock:
            for endpoint, peer in list(self._peers.items()):
                if now - peer.last_received_us >= PEER_IDLE_TIMEOUT_US:
                    self._peers.pop(endpoint, None)
                    peers_changed = True
            for peer in list(self._peers.values()):
                if now - peer.last_heartbeat_us >= HEARTBEAT_US:
                    peer.last_heartbeat_us = now
                    payload = fstl.heartbeat_payload(peer.message_id + 1, 1, self._wire_time(peer), 0, 0)
                    self._send(peer, 9, payload, reliable=False, mission_time_us=0, frame_id=0, now_us=now)
                if (
                    peer.session_started
                    and not self._playing_provider()
                    and now - peer.last_state_refresh_us >= PAUSED_STATE_REFRESH_US
                ):
                    self._send_snapshot(peer, now, snapshot_flag=2)
                self._poll_pending(peer, now)
            for key, peer in list(self._retired.items()):
                self._poll_pending(peer, now)
                if not peer.pending:
                    self._retired.pop(key, None)
        if peers_changed:
            self._notify()

    def _poll_pending(self, peer: ReplayPeer, now: int) -> None:
        for pending in list(peer.pending.values()):
            if now - pending.first_sent_us >= RELIABLE_LIFETIME_US:
                peer.pending.pop(pending.message_id, None)
                continue
            if now < pending.next_send_us:
                continue
            pending.next_send_us = now + RETRY_US
            self._send_datagrams(
                peer.endpoint,
                pending.message_type,
                pending.base_flags | fstl.RETRANSMISSION,
                peer.session_id,
                pending.frame_id,
                pending.message_id,
                pending.payload,
                self._wire_time(peer),
                peer,
                pending.mission_time_us,
            )

    def _wire_time(self, peer: ReplayPeer) -> int:
        return max(0, peer.clock_offset_us + self._virtual_position(fstl.now_us()))

    def _virtual_position(self, now_us: int) -> int:
        reported = max(0, int(self._position_provider()))
        playing = bool(self._playing_provider())
        if reported != self._clock_source_position_us:
            self._clock_source_position_us = reported
            self._clock_position_us = reported
            self._clock_observed_us = now_us
        elif playing != self._clock_playing:
            if self._clock_playing:
                self._clock_position_us += max(0, now_us - self._clock_observed_us)
            self._clock_observed_us = now_us
        self._clock_playing = playing
        if playing:
            return self._clock_position_us + max(0, now_us - self._clock_observed_us)
        return self._clock_position_us
