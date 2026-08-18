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
HARD_RECONNECT_SECONDS = 10.0


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
    def _open_socket(config: TelemetryConfig) -> socket.socket:
        addresses = socket.getaddrinfo(config.host, config.port, type=socket.SOCK_DGRAM)
        if not addresses:
            raise OSError("FSTL endpoint did not resolve")
        last_error: OSError | None = None
        for family, socktype, protocol, _, address in addresses:
            sock = socket.socket(family, socktype, protocol)
            try:
                _configure_socket(sock)
                sock.settimeout(0.1)
                sock.connect(address)
                return sock
            except OSError as exc:
                last_error = exc
                sock.close()
        raise last_error or OSError("unable to open FSTL socket")

    def _frame_from_client(self, client: Any, now_us: int, *, error: str | None = None) -> FstlFrame:
        state = client.state
        public_state = "LIVE" if state.status == "Live" else "STALE" if state.status == "Stale" else "DISCONNECTED"
        records: dict[str, list[dict[str, Any]]] = {}
        derived: dict[str, Any] = {}
        player: str | int | None = None
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
        )

    def _run(self) -> None:
        while not self._stop.is_set():
            self._reconfigure.clear()
            config = self._current_config()
            self._publish(state="DISCONNECTED")
            try:
                with self._open_socket(config) as sock:
                    client = fstl.ConsoleClient(sock, config.stale_after_ms * 1000)
                    client.begin()
                    last_progress_us = fstl.now_us()
                    recovery_started: float | None = None
                    while not self._stop.is_set() and not self._reconfigure.is_set():
                        now_us = fstl.now_us()
                        client.poll_reliable(now_us)
                        if client.state.status == "Disconnected" and not client.state.welcomed:
                            break
                        try:
                            datagram = sock.recv(fstl.MAX_DATAGRAM)
                            received_us, received_utc = fstl.local_observation()
                            if client.receive(datagram, received_us, received_utc):
                                last_progress_us = received_us
                                if client.state.status == "Live":
                                    self._last_live_monotonic = time.monotonic()
                                    recovery_started = None
                                self._publish_callback(self._frame_from_client(client, received_us))
                            if client.terminal_published:
                                self._publish(state="DISCONNECTED")
                                break
                        except socket.timeout:
                            pass
                        now_us, now_utc = fstl.local_observation()
                        previous = client.state.status
                        client.state.stale_if_needed(now_us, now_utc, config.stale_after_ms * 1000)
                        if previous != client.state.status:
                            self._publish_callback(self._frame_from_client(client, now_us))
                        if client.state.status == "Stale":
                            if recovery_started is None:
                                recovery_started = time.monotonic()
                            if client.pending_resync is None:
                                client._resync(now_us)
                            if time.monotonic() - recovery_started >= HARD_RECONNECT_SECONDS:
                                break
                        elif client.state.status == "Synchronizing" and now_us - last_progress_us >= config.stale_after_ms * 1000:
                            if client.pending_resync is None:
                                client._resync(now_us)
                            if recovery_started is None:
                                recovery_started = time.monotonic()
                            if time.monotonic() - recovery_started >= HARD_RECONNECT_SECONDS:
                                break
                        else:
                            recovery_started = None
            except (OSError, ValueError, decoder.DecodeFailure) as exc:
                self._publish(state="DISCONNECTED", error=str(exc))
            if self._stop.is_set():
                break
            if self._reconfigure.wait(1.0):
                continue
