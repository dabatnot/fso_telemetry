from __future__ import annotations

import threading
import time
from typing import Callable

from . import __version__
from .alerts import AlertEngine
from .can_service import CanService
from .config import ConfigStore
from .fstl_service import FstlFrame, FstlService
from .models import (
    AvCoreConfig,
    AvCoreStatus,
    CockpitStatus,
    ConfigUpdateResponse,
    ConfigurationStatus,
    MissionStatus,
    TelemetryStatus,
    LampTestRequest,
)


class AvCoreRuntime:
    def __init__(
        self,
        store: ConfigStore,
        *,
        actual_http_port: int | None = None,
        fstl_factory: Callable[..., FstlService] = FstlService,
        can_factory: Callable[..., CanService] = CanService,
    ):
        self.store = store
        loaded = store.load()
        self._config = loaded.config
        self._config_error = loaded.error
        self._actual_http_port = actual_http_port or loaded.config.web.port
        self._started_at = time.monotonic()
        self._revision = 0
        self._lock = threading.RLock()
        self._alert_engine = AlertEngine()
        self._fstl_frame = FstlFrame()
        self._cockpit = CockpitStatus()
        self._last_session_id: str | None = None
        self._fstl = fstl_factory(self._config.telemetry, self._on_fstl_frame)
        self._can = can_factory(self._config, self._on_can_change)

    def start(self) -> None:
        self._fstl.start()
        self._can.start()

    def stop(self) -> None:
        self._can.stop()
        self._fstl.stop()

    @property
    def config(self) -> AvCoreConfig:
        with self._lock:
            return self._config.model_copy(deep=True)

    @property
    def revision(self) -> int:
        with self._lock:
            return self._revision

    def update_config(self, config: AvCoreConfig) -> ConfigUpdateResponse:
        with self._lock:
            self.store.save(config)
            telemetry_changed = self._config.telemetry != config.telemetry
            self._config = config.model_copy(deep=True)
            self._config_error = None
            self._recalculate_cockpit_locked(reset_hysteresis=True)
            self._revision += 1
            response = ConfigUpdateResponse(
                config=self._config,
                restart_required=self._restart_required_locked(),
            )
            cockpit = self._cockpit.model_copy(deep=True)
            telemetry_state = self._fstl_frame.state
        if telemetry_changed:
            self._fstl.reconfigure(config.telemetry)
        self._can.reconfigure(config)
        self._can.update_cockpit(cockpit, telemetry_state)
        return response

    def _on_fstl_frame(self, frame: FstlFrame) -> None:
        with self._lock:
            session_changed = frame.session_id != self._last_session_id
            self._fstl_frame = frame
            self._last_session_id = frame.session_id
            self._recalculate_cockpit_locked(reset_hysteresis=session_changed)
            self._revision += 1
            cockpit = self._cockpit.model_copy(deep=True)
            telemetry_state = frame.state
        self._can.update_cockpit(cockpit, telemetry_state)

    def _on_can_change(self) -> None:
        with self._lock:
            self._revision += 1

    def start_lamp_test(self, request: LampTestRequest) -> bool:
        return self._can.start_lamp_test(request)

    def _recalculate_cockpit_locked(self, *, reset_hysteresis: bool) -> None:
        frame = self._fstl_frame
        if frame.state != "LIVE":
            self._cockpit = self._alert_engine.unavailable()
            return
        self._cockpit = self._alert_engine.evaluate(
            player_entity_id=frame.player_entity_id,
            records=frame.records,
            derived=frame.derived,
            config=self._config.alerts,
            reset_hysteresis=reset_hysteresis,
        )

    def status(self) -> AvCoreStatus:
        with self._lock:
            config = self._config
            modules = self._can.modules()
            return AvCoreStatus(
                version=__version__,
                uptime_ms=max(0, int((time.monotonic() - self._started_at) * 1000)),
                configuration=ConfigurationStatus(
                    state="ERROR" if self._config_error else "OK",
                    message=self._config_error,
                ),
                restart_required=self._restart_required_locked(),
                telemetry=TelemetryStatus(
                    state=self._fstl_frame.state,
                    host=config.telemetry.host,
                    port=config.telemetry.port,
                    session_id=self._fstl_frame.session_id,
                    last_live_age_ms=self._last_live_age_ms_locked(),
                    error=self._fstl_frame.error,
                ),
                mission=MissionStatus(
                    active=self._fstl_frame.mission_active,
                    paused=self._fstl_frame.mission_paused,
                    generation=self._fstl_frame.mission_generation,
                    time_compression=self._fstl_frame.time_compression,
                ),
                cockpit=self._cockpit,
                can=self._can.status(),
                modules=modules,
            )

    def _restart_required_locked(self) -> bool:
        return self._config.web.port != self._actual_http_port

    def _last_live_age_ms_locked(self) -> int | None:
        observed = self._fstl_frame.last_live_monotonic
        return max(0, int((time.monotonic() - observed) * 1000)) if observed is not None else None
