from __future__ import annotations

import threading
import time

from . import __version__
from .config import ConfigStore
from .models import (
    AvCoreConfig,
    AvCoreStatus,
    CanStatus,
    ConfigUpdateResponse,
    ConfigurationStatus,
    LinkStatus,
    ModuleStatus,
)


class AvCoreRuntime:
    def __init__(self, store: ConfigStore, *, actual_http_port: int | None = None):
        self.store = store
        loaded = store.load()
        self._config = loaded.config
        self._config_error = loaded.error
        self._actual_http_port = actual_http_port or loaded.config.web.port
        self._started_at = time.monotonic()
        self._revision = 0
        self._lock = threading.RLock()

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
            self._config = config.model_copy(deep=True)
            self._config_error = None
            self._revision += 1
            return ConfigUpdateResponse(
                config=self._config,
                restart_required=self._restart_required_locked(),
            )

    def status(self) -> AvCoreStatus:
        with self._lock:
            config = self._config
            modules = [
                ModuleStatus(role="WARN_CTRL", installed=config.modules.warn_ctrl.installed),
                ModuleStatus(role="THREAT_PROC", installed=config.modules.threat_proc.installed),
                ModuleStatus(role="SENS_PROC", installed=config.modules.sens_proc.installed),
                ModuleStatus(role="INST_PROC", installed=config.modules.inst_proc.installed),
            ]
            return AvCoreStatus(
                version=__version__,
                uptime_ms=max(0, int((time.monotonic() - self._started_at) * 1000)),
                configuration=ConfigurationStatus(
                    state="ERROR" if self._config_error else "OK",
                    message=self._config_error,
                ),
                restart_required=self._restart_required_locked(),
                telemetry=LinkStatus(
                    host=config.telemetry.host,
                    port=config.telemetry.port,
                ),
                can=CanStatus(),
                modules=modules,
            )

    def _restart_required_locked(self) -> bool:
        return self._config.web.port != self._actual_http_port
