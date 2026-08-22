from __future__ import annotations

import copy
import os
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from .models import TelemetryConfig


def _load_fstl_modules() -> tuple[Any, Any]:
    configured = os.environ.get("AV_CORE_FSTL_CLIENT_DIR")
    candidates = [Path(configured)] if configured else []
    candidates.extend(parent / "test" / "telemetry" / "protocol" / "tools" for parent in Path(__file__).resolve().parents)
    for candidate in candidates:
        if (candidate / "fstl_client_core.py").is_file() and str(candidate) not in sys.path:
            sys.path.insert(0, str(candidate))
            break
    import fstl_client_core as client  # type: ignore[import-not-found]
    import fstl_reference_decoder as decoder  # type: ignore[import-not-found]

    return client, decoder


fstl, decoder = _load_fstl_modules()


WINDOWS_SIO_UDP_CONNRESET = 0x9800000C
RECOVERY_GRACE_SECONDS = 1.0
LOCAL_ERROR_RETRY_SECONDS = 0.25


def _configure_socket(sock: socket.socket) -> None:
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
        sock.fileno(), WINDOWS_SIO_UDP_CONNRESET,
        ctypes.byref(disabled), ctypes.sizeof(disabled),
        None, 0, ctypes.byref(bytes_returned), None, None,
    )
    if result != 0:
        error = ws2_32.WSAGetLastError()
        raise OSError(error, "failed to disable SIO_UDP_CONNRESET")


@dataclass(frozen=True)
class FstlFrame:
    state: str = "DISCONNECTED"
    session_id: str | None = None
    last_live_monotonic: float | None = None
    error: str | None = None
    player_entity_id: str | int | None = None
    records: dict[str, list[dict[str, Any]]] = field(default_factory=dict)
    derived: dict[str, Any] = field(default_factory=dict)
    mission_active: bool = False
    mission_paused: bool = False
    mission_generation: int | None = None
    time_compression: float | None = None


class FstlService:
    def __init__(self, config: TelemetryConfig, publish: Callable[[FstlFrame], None]) -> None:
        self._config = config.model_copy(deep=True)
        self._publish_callback = publish
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._reconfigure = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_live_monotonic: float | None = None

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="av-core-fstl", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._reconfigure.set()
        thread = self._thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=2.0)
        self._thread = None

    def reconfigure(self, config: TelemetryConfig) -> None:
        with self._lock:
            changed = self._config != config
            self._config = config.model_copy(deep=True)
        if changed:
            self._reconfigure.set()

    def _current_config(self) -> TelemetryConfig:
        with self._lock:
            return self._config.model_copy(deep=True)

    def _publish(self, **values: Any) -> None:
        self._publish_callback(FstlFrame(last_live_monotonic=self._last_live_monotonic, **values))

    @staticmethod
    def _open_socket(
        config: TelemetryConfig,
        previous_endpoint: tuple[Any, ...] | None,
    ) -> tuple[socket.socket, tuple[Any, ...]]:
        addresses = socket.getaddrinfo(config.host, config.port, type=socket.SOCK_DGRAM)
        if not addresses:
            raise OSError("FSTL endpoint did not resolve")
        last_error: OSError | None = None
        for family, socktype, protocol, _, address in addresses:
            rejected: list[socket.socket] = []
            try:
                for _ in range(4):
                    sock = socket.socket(family, socktype, protocol)
                    try:
                        _configure_socket(sock)
                        sock.settimeout(0.1)
                        sock.bind(("::", 0) if family == socket.AF_INET6 else ("0.0.0.0", 0))
                        sock.connect(address)
                        endpoint = sock.getsockname()
                        if previous_endpoint is None or endpoint != previous_endpoint:
                            return sock, endpoint
                        rejected.append(sock)
                    except OSError:
                        sock.close()
                        raise
                last_error = OSError("unable to allocate a fresh UDP endpoint")
            except OSError as exc:
                last_error = exc
            finally:
                for rejected_socket in rejected:
                    rejected_socket.close()
        raise last_error or OSError("unable to open FSTL socket")

    def _frame_from_client(self, client: Any, now_us: int, *, error: str | None = None) -> FstlFrame:
        state = client.state
        public_state = (
            "LIVE" if state.status == "Live" else
            "READY" if state.status == "Ready" else
            "STALE" if state.status == "Stale" else
            "DISCONNECTED"
        )
        records: dict[str, list[dict[str, Any]]] = {}
        derived: dict[str, Any] = {}
        player: str | int | None = None
        mission = state.records.get("MISSION_STATE", {})
        if public_state == "LIVE":
            for record in state.record_instances.values():
                records.setdefault(record["recordName"], []).append(copy.deepcopy(record["fields"]))
            session = state.records.get("SESSION_STATE", {})
            player = session.get("observed_player_entity_id")
            projection = fstl.DashboardProjection(
                state, now_us, state.smoothed_offset_us, state.clock_filter_valid
            ).build()
            derived = copy.deepcopy(projection["derived"])
        return FstlFrame(
            state=public_state,
            session_id=str(state.session_id) if state.session_id else None,
            last_live_monotonic=self._last_live_monotonic,
            error=error,
            player_entity_id=player,
            records=records,
            derived=derived,
            mission_active=public_state == "LIVE",
            mission_paused=public_state == "LIVE" and bool(mission.get("paused", 0)),
            mission_generation=int(mission["mission_generation"]) if mission.get("mission_generation") is not None else None,
            time_compression=float(mission["time_compression"]) if mission.get("time_compression") is not None else None,
        )

    def _run(self) -> None:
        while not self._stop.is_set():
            self._reconfigure.clear()
            config = self._current_config()
            self._publish(state="DISCONNECTED")
            local_error = False
            try:
                sock, _ = self._open_socket(config, None)
                with sock:
                    session_generation = 0
                    while not self._stop.is_set() and not self._reconfigure.is_set():
                        client = fstl.ConsoleClient(
                            sock,
                            config.stale_after_ms * 1000,
                            ignore_previous_session_datagrams=session_generation != 0,
                            required_state_domain_coverage=fstl.COCKPIT_SENSORS_COVERAGE,
                        )
                        session_generation += 1
                        client.begin()
                        last_progress_us = fstl.now_us()
                        recovery_started: float | None = None
                        recovery_resync_sent = False
                        while not self._stop.is_set() and not self._reconfigure.is_set():
                            now_us = fstl.now_us()
                            client.poll_reliable(now_us)
                            if client.state.status == "Disconnected" and not client.state.welcomed:
                                if not client.renew_hello_window(now_us):
                                    break
                            try:
                                datagram = sock.recv(fstl.MAX_DATAGRAM)
                                received_us, received_utc = fstl.local_observation()
                                if client.receive(datagram, received_us, received_utc):
                                    last_progress_us = received_us
                                    if client.state.status == "Live":
                                        self._last_live_monotonic = time.monotonic()
                                        recovery_started = None
                                        recovery_resync_sent = False
                                    self._publish_callback(self._frame_from_client(client, received_us))
                                if client.terminal_published:
                                    self._publish(state="DISCONNECTED")
                                    break
                            except socket.timeout:
                                pass
                            except (ValueError, decoder.DecodeFailure) as exc:
                                self._publish(state="DISCONNECTED", error=str(exc))
                                break
                            now_us, now_utc = fstl.local_observation()
                            previous = client.state.status
                            client.state.stale_if_needed(now_us, now_utc, config.stale_after_ms * 1000)
                            if previous != client.state.status:
                                self._publish_callback(self._frame_from_client(client, now_us))
                            if client.state.status == "Ready" and not client.state.session_begun:
                                last_network = client.state.last_network_activity_us or last_progress_us
                                silent_us = now_us - last_network
                                if silent_us >= config.stale_after_ms * 1000:
                                    self._publish(
                                        state="STALE",
                                        session_id=str(client.state.session_id),
                                    )
                                if silent_us >= config.stale_after_ms * 1000 + int(RECOVERY_GRACE_SECONDS * 1_000_000):
                                    break
                                recovery_started = None
                                recovery_resync_sent = False
                            elif client.state.status == "Stale":
                                if recovery_started is None:
                                    recovery_started = time.monotonic()
                                if not recovery_resync_sent and client.pending_resync is None:
                                    client._resync(now_us)
                                    recovery_resync_sent = True
                                if time.monotonic() - recovery_started >= RECOVERY_GRACE_SECONDS:
                                    break
                            elif (
                                client.state.session_begun
                                and client.state.welcomed
                                and not client.state.keyframe_applied
                                and now_us - last_progress_us >= fstl.RELIABLE_WINDOW_US
                            ):
                                # A replacement session has already been
                                # accepted. Do not apply the Live 1 s + 1 s
                                # policy a second time while its reliable
                                # SESSION_BEGIN/manifest/snapshot pipeline is
                                # progressing; otherwise each response can be
                                # invalidated by a newer nonce. The bounded
                                # reliable window remains the terminal limit.
                                break
                            else:
                                recovery_started = None
                                recovery_resync_sent = False
                        if not self._stop.is_set() and not self._reconfigure.is_set():
                            # A protocol recovery keeps the connected UDP socket
                            # and therefore the source endpoint. A fresh client
                            # state supplies a new nonce for atomic replacement.
                            self._publish(state="DISCONNECTED")
                            continue
                        break
            except OSError as exc:
                local_error = True
                self._publish(state="DISCONNECTED", error=str(exc))
            if self._stop.is_set():
                break
            if self._reconfigure.is_set():
                continue
            if local_error:
                self._reconfigure.wait(LOCAL_ERROR_RETRY_SECONDS)
