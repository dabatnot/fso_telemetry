from __future__ import annotations

from dataclasses import dataclass
import threading
import time
from typing import Any, Callable

from .can_protocol import (
    CAUTION_STATE_ID,
    LIGHTING_COMMAND_ID,
    LIGHTING_STATE_ID,
    NODE_STATUS_IDS,
    THREAT_LAMP_NAMES,
    THREAT_STATE_ID,
    WARNING_STATE_ID,
    NodeStatus,
    decode_lighting_state,
    decode_node_status,
    encode_caution_state,
    encode_lamp_test,
    encode_lighting_configuration,
    encode_threat_state,
    encode_warning_state,
)
from .models import AvCoreConfig, CanStatus, CockpitStatus, LampTestRequest, ModuleStatus


@dataclass
class _ObservedNode:
    status: NodeStatus
    observed_at: float


class CanService:
    def __init__(self, config: AvCoreConfig, on_change: Callable[[], None], *, bus_factory: Callable[[], Any] | None = None):
        self._config = config.model_copy(deep=True)
        self._on_change = on_change
        self._bus_factory = bus_factory or self._open_socketcan
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._bus: Any | None = None
        self._state = "UNAVAILABLE"
        self._error: str | None = None
        self._receive_errors = 0
        self._transmit_errors = 0
        self._cockpit = CockpitStatus()
        self._telemetry_state = "DISCONNECTED"
        self._nodes: dict[str, _ObservedNode] = {}
        self._reported_module_states: tuple[str, ...] | None = None
        self._lighting_dirty = True

    @staticmethod
    def _open_socketcan() -> Any:
        import can

        return can.Bus(interface="socketcan", channel="can0")

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="av-core-can", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        thread, self._thread = self._thread, None
        if thread is not None:
            thread.join(timeout=2)
        with self._lock:
            bus, self._bus = self._bus, None
        if bus is not None:
            bus.shutdown()

    def reconfigure(self, config: AvCoreConfig) -> None:
        with self._lock:
            self._config = config.model_copy(deep=True)
            self._lighting_dirty = True

    def update_cockpit(self, cockpit: CockpitStatus, telemetry_state: str) -> None:
        with self._lock:
            self._cockpit = cockpit.model_copy(deep=True)
            self._telemetry_state = telemetry_state

    def start_lamp_test(self, request: LampTestRequest) -> str | None:
        with self._lock:
            bus = self._bus
            warn = self._node_state_locked("WARN_CTRL", time.monotonic())
            available = self._state == "OK" and bus is not None and warn.state == "ONLINE"
        if not available:
            return "WARN_CTRL_UNAVAILABLE"
        needs_threat = request.target == "THREAT_PROC" or (
            request.target == "LAMP" and request.lamp in THREAT_LAMP_NAMES
        )
        if needs_threat:
            with self._lock:
                threat = self._node_state_locked("THREAT_PROC", time.monotonic())
            if threat.state != "ONLINE":
                return "THREAT_PROC_UNAVAILABLE"
        return None if self._send(LIGHTING_COMMAND_ID, encode_lamp_test(request)) else "WARN_CTRL_UNAVAILABLE"

    def status(self) -> CanStatus:
        with self._lock:
            return CanStatus(
                state=self._state,
                receive_errors=self._receive_errors,
                transmit_errors=self._transmit_errors,
                error=self._error,
            )

    def modules(self) -> list[ModuleStatus]:
        with self._lock:
            now = time.monotonic()
            return [self._node_state_locked(role, now) for role in NODE_STATUS_IDS]

    def _node_state_locked(self, role: str, now: float) -> ModuleStatus:
        key = {"WARN_CTRL": "warn_ctrl", "THREAT_PROC": "threat_proc", "SENS_PROC": "sens_proc", "INST_PROC": "inst_proc"}[role]
        installed = getattr(self._config.modules, key).installed
        observed = self._nodes.get(role)
        if self._state in ("ERROR", "BUS_OFF"):
            state = "CAN_ERROR"
        elif self._state != "OK":
            state = "UNAVAILABLE"
        elif observed is None or (now - observed.observed_at) * 1000 > self._config.can.node_timeout_ms:
            state = "ABSENT" if installed else "UNAVAILABLE"
        else:
            state = observed.status.health
        age = None if observed is None else max(0, int((now - observed.observed_at) * 1000))
        return ModuleStatus(
            role=role,
            installed=installed,
            state=state,
            protocol_id=NODE_STATUS_IDS[role],
            uid=observed.status.uid if observed else None,
            firmware_version=observed.status.firmware_version if observed else None,
            last_heartbeat_ms=age,
        )

    def _run(self) -> None:
        next_state = 0.0
        next_config = 0.0
        retry_at = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            with self._lock:
                bus = self._bus
            if bus is None:
                if now >= retry_at:
                    retry_at = now + 1.0
                    self._connect()
                self._stop.wait(0.05)
                continue
            if now >= next_state:
                self._publish_state()
                self._notify_module_transition(now)
                next_state = now + 0.05
            with self._lock:
                config_due = self._lighting_dirty or now >= next_config
                config = self._config.lighting.model_copy(deep=True)
                self._lighting_dirty = False
            if config_due:
                for payload in encode_lighting_configuration(config):
                    self._send(LIGHTING_COMMAND_ID, payload)
                next_config = now + 1.0
            self._receive_once()

    def _connect(self) -> None:
        try:
            bus = self._bus_factory()
        except Exception as exc:
            with self._lock:
                failed_state = "BUS_OFF" if self._state == "BUS_OFF" else "UNAVAILABLE"
            self._set_state(failed_state, str(exc))
            return
        with self._lock:
            self._bus = bus
            self._state = "OK"
            self._error = None
            self._lighting_dirty = True
        self._on_change()

    def _publish_state(self) -> None:
        with self._lock:
            cockpit = self._cockpit.model_copy(deep=True)
            diagnostics = self._diagnostics_locked(time.monotonic())
        self._send(WARNING_STATE_ID, encode_warning_state(cockpit))
        self._send(CAUTION_STATE_ID, encode_caution_state(cockpit, diagnostics))
        self._send(THREAT_STATE_ID, encode_threat_state(cockpit, live=self._telemetry_state == "LIVE"))

    def _diagnostics_locked(self, now: float) -> dict[str, int]:
        result = {name: 0 for name in ("AV_CORE", "FLT_DATA", "AV_BUS", "SENS_PROC", "THREAT_PROC", "INST_PROC", "WARN_CTRL")}
        result["FLT_DATA"] = 0 if self._telemetry_state == "LIVE" else 2
        for role in ("SENS_PROC", "THREAT_PROC", "INST_PROC", "WARN_CTRL"):
            status = self._node_state_locked(role, now)
            result[role] = 1 if status.state == "DEGRADED" else 2 if status.state in ("ABSENT", "CAN_ERROR") else 0
        return result

    def _notify_module_transition(self, now: float) -> None:
        with self._lock:
            states = tuple(self._node_state_locked(role, now).state for role in NODE_STATUS_IDS)
            changed = states != self._reported_module_states
            self._reported_module_states = states
        if changed:
            self._on_change()

    def _receive_once(self) -> None:
        with self._lock:
            bus = self._bus
        if bus is None:
            return
        try:
            message = bus.recv(timeout=0.01)
            if message is None:
                return
            payload = bytes(message.data)
            if message.arbitration_id in NODE_STATUS_IDS.values():
                status = decode_node_status(message.arbitration_id, payload)
                with self._lock:
                    self._nodes[status.role] = _ObservedNode(status, time.monotonic())
                self._on_change()
            elif message.arbitration_id == LIGHTING_STATE_ID:
                decode_lighting_state(payload)
        except ValueError:
            with self._lock:
                self._receive_errors += 1
            self._on_change()
        except Exception as exc:
            self._disconnect("ERROR", exc)

    def _send(self, arbitration_id: int, payload: bytes) -> bool:
        with self._lock:
            bus = self._bus
        if bus is None:
            return False
        try:
            import can

            bus.send(can.Message(arbitration_id=arbitration_id, data=payload, is_extended_id=False), timeout=0.02)
            return True
        except Exception as exc:
            with self._lock:
                self._transmit_errors += 1
            self._disconnect("BUS_OFF", exc)
            return False

    def _disconnect(self, state: str, exc: Exception) -> None:
        with self._lock:
            bus, self._bus = self._bus, None
            self._state = state
            self._error = str(exc)
        if bus is not None:
            try:
                bus.shutdown()
            except Exception:
                pass
        self._on_change()

    def _set_state(self, state: str, error: str | None) -> None:
        changed = False
        with self._lock:
            changed = self._state != state or self._error != error
            self._state = state
            self._error = error
        if changed:
            self._on_change()
