from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator
from pydantic.alias_generators import to_camel


class PublicModel(BaseModel):
    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
        extra="forbid",
    )


class TelemetryConfig(PublicModel):
    host: str = "127.0.0.1"
    port: int = Field(default=42042, ge=1, le=65535)
    stale_after_ms: int = Field(default=1000, ge=1, le=60000)

    @field_validator("host")
    @classmethod
    def validate_host(cls, value: str) -> str:
        host = value.strip()
        if not host or len(host) > 253:
            raise ValueError("host must contain between 1 and 253 characters")
        return host


class CanConfig(PublicModel):
    node_timeout_ms: int = Field(default=3000, ge=1, le=60000)


class WebConfig(PublicModel):
    port: int = Field(default=8080, ge=1, le=65535)


class ModuleInstallation(PublicModel):
    installed: bool


class ModulesConfig(PublicModel):
    warn_ctrl: ModuleInstallation = Field(default_factory=lambda: ModuleInstallation(installed=True))
    threat_proc: ModuleInstallation = Field(default_factory=lambda: ModuleInstallation(installed=True))
    sens_proc: ModuleInstallation = Field(default_factory=lambda: ModuleInstallation(installed=False))
    inst_proc: ModuleInstallation = Field(default_factory=lambda: ModuleInstallation(installed=False))


class PercentThreshold(PublicModel):
    activate_below_percent: int = Field(ge=0, le=100)
    clear_above_percent: int = Field(ge=0, le=100)

    @model_validator(mode="after")
    def validate_hysteresis(self) -> "PercentThreshold":
        if self.clear_above_percent <= self.activate_below_percent:
            raise ValueError("clearAbovePercent must be greater than activateBelowPercent")
        return self


class CountermeasureThreshold(PublicModel):
    activate_at_or_below_count: int = Field(default=3, ge=0, le=255)
    activate_at_or_below_percent: int = Field(default=20, ge=0, le=100)


class AlertsConfig(PublicModel):
    engine: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=50, clear_above_percent=55)
    )
    shield: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=30, clear_above_percent=35)
    )
    hull: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=40, clear_above_percent=45)
    )
    weapon_energy: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=20, clear_above_percent=30)
    )
    afterburner_fuel: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=20, clear_above_percent=25)
    )
    ammo: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=20, clear_above_percent=25)
    )
    countermeasures: CountermeasureThreshold = Field(default_factory=CountermeasureThreshold)
    subsystem: PercentThreshold = Field(
        default_factory=lambda: PercentThreshold(activate_below_percent=40, clear_above_percent=45)
    )


class RgbColor(PublicModel):
    r: int = Field(ge=0, le=255)
    g: int = Field(ge=0, le=255)
    b: int = Field(ge=0, le=255)


class LightingConfig(PublicModel):
    warning_color: RgbColor = Field(default_factory=lambda: RgbColor(r=255, g=96, b=0))
    caution_color: RgbColor = Field(default_factory=lambda: RgbColor(r=255, g=96, b=0))
    max_brightness_percent: int = Field(default=30, ge=0, le=100)
    slow_flash_hz: float = Field(default=1.0, ge=0.25, le=10.0)
    fast_flash_hz: float = Field(default=4.0, ge=0.25, le=10.0)

    @model_validator(mode="after")
    def validate_flash_rates(self) -> "LightingConfig":
        if self.fast_flash_hz <= self.slow_flash_hz:
            raise ValueError("fastFlashHz must be greater than slowFlashHz")
        return self


class AvCoreConfig(PublicModel):
    schema_version: Literal[1] = 1
    telemetry: TelemetryConfig = Field(default_factory=TelemetryConfig)
    can: CanConfig = Field(default_factory=CanConfig)
    web: WebConfig = Field(default_factory=WebConfig)
    modules: ModulesConfig = Field(default_factory=ModulesConfig)
    alerts: AlertsConfig = Field(default_factory=AlertsConfig)
    lighting: LightingConfig = Field(default_factory=LightingConfig)


class LampTestRequest(PublicModel):
    target: Literal["ALL"]
    active: bool


class ConfigurationStatus(PublicModel):
    state: Literal["OK", "ERROR"]
    message: str | None


class TelemetryStatus(PublicModel):
    state: Literal["LIVE", "STALE", "DISCONNECTED"] = "DISCONNECTED"
    host: str
    port: int
    session_id: str | None = None
    last_live_age_ms: int | None = Field(default=None, ge=0)
    error: str | None = None


class WarningStatus(PublicModel):
    available: bool = False
    master: bool = False
    fire: bool = False
    missile: bool = False
    blast: bool = False
    collision: bool = False
    emp: bool = False


class PercentCautionStatus(PublicModel):
    state: Literal["ACTIVE", "CLEAR", "UNAVAILABLE"] = "UNAVAILABLE"
    value_percent: float | None = Field(default=None, ge=0, le=100)


class SensorCautionStatus(PublicModel):
    state: Literal["ACTIVE", "CLEAR", "UNAVAILABLE"] = "UNAVAILABLE"
    sensor_state: Literal["ONLINE", "DEGRADED", "OFFLINE"] | None = None


class CountermeasureCautionStatus(PublicModel):
    state: Literal["ACTIVE", "CLEAR", "UNAVAILABLE"] = "UNAVAILABLE"
    value_percent: float | None = Field(default=None, ge=0, le=100)
    value_count: int | None = Field(default=None, ge=0)


class CautionStatus(PublicModel):
    master: bool = False
    engine: PercentCautionStatus = Field(default_factory=PercentCautionStatus)
    sensor: SensorCautionStatus = Field(default_factory=SensorCautionStatus)
    shield: PercentCautionStatus = Field(default_factory=PercentCautionStatus)
    hull: PercentCautionStatus = Field(default_factory=PercentCautionStatus)
    weapon_energy: PercentCautionStatus = Field(default_factory=PercentCautionStatus)
    afterburner_fuel: PercentCautionStatus = Field(default_factory=PercentCautionStatus)
    ammo: PercentCautionStatus = Field(default_factory=PercentCautionStatus)
    countermeasures: CountermeasureCautionStatus = Field(default_factory=CountermeasureCautionStatus)
    subsystem: PercentCautionStatus = Field(default_factory=PercentCautionStatus)


class ThreatStatus(PublicModel):
    available: bool = False
    sector_mask: int = Field(default=0, ge=0, le=255)
    incoming_missile_count: int = Field(default=0, ge=0)
    lock_state: Literal["NONE", "ATTEMPT", "ACQUIRED"] = "NONE"


class CockpitStatus(PublicModel):
    available: bool = False
    warnings: WarningStatus = Field(default_factory=WarningStatus)
    cautions: CautionStatus = Field(default_factory=CautionStatus)
    threat: ThreatStatus = Field(default_factory=ThreatStatus)


class CanStatus(PublicModel):
    state: Literal["UNAVAILABLE"] = "UNAVAILABLE"
    interface: Literal["can0"] = "can0"
    bitrate: Literal[1000000] = 1000000


class ModuleStatus(PublicModel):
    role: Literal["WARN_CTRL", "THREAT_PROC", "SENS_PROC", "INST_PROC"]
    installed: bool
    state: Literal["UNAVAILABLE"] = "UNAVAILABLE"
    protocol_id: int | None = None
    uid: str | None = None
    firmware_version: str | None = None
    last_heartbeat_ms: int | None = None


class AvCoreStatus(PublicModel):
    schema_name: Literal["AvCoreStatusV1"] = Field(
        default="AvCoreStatusV1",
        alias="schema",
        serialization_alias="schema",
    )
    version: str
    uptime_ms: int
    configuration: ConfigurationStatus
    restart_required: bool
    telemetry: TelemetryStatus
    cockpit: CockpitStatus = Field(default_factory=CockpitStatus)
    can: CanStatus = Field(default_factory=CanStatus)
    modules: list[ModuleStatus]


class ConfigUpdateResponse(PublicModel):
    config: AvCoreConfig
    restart_required: bool
