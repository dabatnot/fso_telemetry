from __future__ import annotations

import math
from typing import Any

from .models import (
    AlertsConfig,
    CockpitStatus,
    CountermeasureCautionStatus,
    CautionStatus,
    PercentCautionStatus,
    PercentThreshold,
    SensorCautionStatus,
    ThreatStatus,
    WarningStatus,
)


def _finite(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _integer(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    try:
        result = int(value)
    except (TypeError, ValueError):
        return None
    return result if result >= 0 else None


def _ratio(current: Any, maximum: Any) -> float | None:
    numerator = _finite(current)
    denominator = _finite(maximum)
    if numerator is None or denominator is None or denominator <= 0:
        return None
    return min(1.0, max(0.0, numerator / denominator))


def _derived_ratio(derived: dict[str, Any], path: str) -> float | None:
    item = derived.get(path)
    if not isinstance(item, dict) or item.get("available") is not True:
        return None
    value = _finite(item.get("value"))
    return min(1.0, max(0.0, value)) if value is not None else None


def _player_record(records: dict[str, list[dict[str, Any]]], name: str, player: str) -> dict[str, Any] | None:
    return next((record for record in records.get(name, []) if str(record.get("entity_id")) == player), None)


class AlertEngine:
    def __init__(self) -> None:
        self._latches: dict[str, bool] = {}

    def reset(self) -> None:
        self._latches.clear()

    def unavailable(self) -> CockpitStatus:
        self.reset()
        return CockpitStatus()

    def evaluate(
        self,
        *,
        player_entity_id: str | int | None,
        records: dict[str, list[dict[str, Any]]],
        derived: dict[str, Any],
        config: AlertsConfig,
        reset_hysteresis: bool = False,
    ) -> CockpitStatus:
        if reset_hysteresis:
            self.reset()
        if player_entity_id is None:
            return self.unavailable()
        player = str(player_entity_id)
        hud = _player_record(records, "HUD_ALERT_STATE", player)
        threat_record = _player_record(records, "THREAT_STATE", player)
        flight = _player_record(records, "FLIGHT_STATE", player)
        warning = self._warnings(hud, threat_record)
        threat = self._threat(hud, threat_record, flight)
        cautions = self._cautions(player, records, derived, config)
        return CockpitStatus(
            available=True,
            warnings=warning,
            cautions=cautions,
            threat=threat,
        )

    @staticmethod
    def _warnings(hud: dict[str, Any] | None, threat: dict[str, Any] | None) -> WarningStatus:
        incoming = threat.get("incoming_missiles", []) if threat else []
        incoming = incoming if isinstance(incoming, list) else []
        fire = bool(hud and hud.get("primary_fire_threat_active") is True)
        missile = bool(incoming)
        warning_kind = None
        if hud and (_integer(hud.get("presence")) or 0) & 0x01:
            remaining = _integer(hud.get("warning_remaining_us"))
            if remaining is not None and remaining > 0:
                warning_kind = _integer(hud.get("warning_kind"))
        collision = warning_kind == 3
        blast = warning_kind == 4
        emp = warning_kind == 6
        return WarningStatus(
            available=hud is not None and threat is not None,
            master=fire or missile or collision or blast or emp,
            fire=fire,
            missile=missile,
            blast=blast,
            collision=collision,
            emp=emp,
        )

    def _percent_caution(
        self,
        key: str,
        ratio: float | None,
        threshold: PercentThreshold,
    ) -> PercentCautionStatus:
        if ratio is None:
            self._latches.pop(key, None)
            return PercentCautionStatus()
        percent = ratio * 100.0
        active = self._latches.get(key, False)
        if active and ratio > threshold.clear_above_percent / 100.0:
            active = False
        elif not active and ratio < threshold.activate_below_percent / 100.0:
            active = True
        self._latches[key] = active
        return PercentCautionStatus(state="ACTIVE" if active else "CLEAR", value_percent=percent)

    def _cautions(
        self,
        player: str,
        records: dict[str, list[dict[str, Any]]],
        derived: dict[str, Any],
        config: AlertsConfig,
    ) -> CautionStatus:
        engine = self._percent_caution(
            "engine", _derived_ratio(derived, f"entities.{player}.engine_integrity_ratio"), config.engine
        )
        shield = self._percent_caution(
            "shield", _derived_ratio(derived, f"entities.{player}.shield_ratio"), config.shield
        )
        hull = self._percent_caution(
            "hull", _derived_ratio(derived, f"entities.{player}.hull_ratio"), config.hull
        )
        weapon_energy = self._percent_caution(
            "weapon_energy", _derived_ratio(derived, f"entities.{player}.weapon_energy_ratio"), config.weapon_energy
        )
        afterburner = self._percent_caution(
            "afterburner_fuel", _derived_ratio(derived, f"entities.{player}.fuel_ratio"), config.afterburner_fuel
        )
        ammo = self._percent_caution(
            "ammo", self._selected_ammo_ratio(player, records), config.ammo
        )
        subsystem = self._percent_caution(
            "subsystem", self._lowest_subsystem_ratio(player, records), config.subsystem
        )
        sensor = self._sensor_caution(player, records)
        countermeasures = self._countermeasure_caution(player, records, config)
        statuses = [engine, shield, hull, weapon_energy, afterburner, ammo, subsystem, sensor, countermeasures]
        return CautionStatus(
            master=any(status.state == "ACTIVE" for status in statuses),
            engine=engine,
            sensor=sensor,
            shield=shield,
            hull=hull,
            weapon_energy=weapon_energy,
            afterburner_fuel=afterburner,
            ammo=ammo,
            countermeasures=countermeasures,
            subsystem=subsystem,
        )

    @staticmethod
    def _sensor_caution(player: str, records: dict[str, list[dict[str, Any]]]) -> SensorCautionStatus:
        radar = _player_record(records, "RADAR_STATE", player)
        value = _integer(radar.get("sensor_state")) if radar else None
        labels = {0: "OFFLINE", 1: "DEGRADED", 2: "ONLINE"}
        label = labels.get(value)
        if label is None:
            return SensorCautionStatus()
        return SensorCautionStatus(
            state="CLEAR" if label == "ONLINE" else "ACTIVE",
            sensor_state=label,
        )

    @staticmethod
    def _selected_ammo_ratio(player: str, records: dict[str, list[dict[str, Any]]]) -> float | None:
        weapon = _player_record(records, "WEAPON_STATE", player)
        if weapon is None:
            return None
        ratios: list[float] = []
        for family, selected_key in (
            ("primary_banks", "current_primary_bank_id"),
            ("secondary_banks", "current_secondary_bank_id"),
        ):
            selected = _integer(weapon.get(selected_key))
            banks = weapon.get(family, [])
            if not selected or not isinstance(banks, list):
                continue
            bank = next((item for item in banks if _integer(item.get("bank_id")) == selected), None)
            ammunition = bank.get("ammunition") if isinstance(bank, dict) else None
            if isinstance(ammunition, dict):
                ratio = _ratio(ammunition.get("current"), ammunition.get("initial"))
                if ratio is not None:
                    ratios.append(ratio)
        tertiary = weapon.get("tertiary")
        selected_tertiary = _integer(weapon.get("current_tertiary_bank_id"))
        if (
            selected_tertiary
            and isinstance(tertiary, dict)
            and _integer(tertiary.get("bank_id")) == selected_tertiary
        ):
            ratio = _ratio(tertiary.get("ammunition_current"), tertiary.get("ammunition_initial"))
            if ratio is not None:
                ratios.append(ratio)
        return min(ratios) if ratios else None

    @staticmethod
    def _countermeasure_caution(
        player: str,
        records: dict[str, list[dict[str, Any]]],
        config: AlertsConfig,
    ) -> CountermeasureCautionStatus:
        weapon = _player_record(records, "WEAPON_STATE", player)
        countermeasure = weapon.get("countermeasure") if weapon else None
        if not isinstance(countermeasure, dict):
            return CountermeasureCautionStatus()
        current = _integer(countermeasure.get("current"))
        maximum = _integer(countermeasure.get("maximum"))
        if current is None or maximum is None or maximum <= 0:
            return CountermeasureCautionStatus()
        threshold = max(
            float(config.countermeasures.activate_at_or_below_count),
            maximum * config.countermeasures.activate_at_or_below_percent / 100.0,
        )
        return CountermeasureCautionStatus(
            state="ACTIVE" if current <= threshold else "CLEAR",
            value_percent=min(100.0, max(0.0, current * 100.0 / maximum)),
            value_count=current,
        )

    @staticmethod
    def _lowest_subsystem_ratio(player: str, records: dict[str, list[dict[str, Any]]]) -> float | None:
        ratios: list[float] = []
        for subsystem in records.get("SUBSYSTEM_STATE", []):
            if str(subsystem.get("entity_id")) != player:
                continue
            if _integer(subsystem.get("type")) in {1, 3, 7}:
                continue
            ratio = _ratio(subsystem.get("current_hits"), subsystem.get("max_hits"))
            if ratio is not None:
                ratios.append(ratio)
        return min(ratios) if ratios else None

    @staticmethod
    def _threat(
        hud: dict[str, Any] | None,
        threat: dict[str, Any] | None,
        flight: dict[str, Any] | None,
    ) -> ThreatStatus:
        incoming = threat.get("incoming_missiles", []) if threat else []
        incoming = incoming if isinstance(incoming, list) else []
        mask = _integer(hud.get("missile_direction_sector_mask")) if hud else None
        mask = mask if mask is not None and 0 <= mask <= 0xFF else 0
        lock = _integer(hud.get("missile_lock_state")) if hud else None
        lock_state = "ATTEMPT" if lock == 1 else "ACQUIRED" if lock == 2 else "NONE"
        return ThreatStatus(
            available=threat is not None and hud is not None and flight is not None,
            sector_mask=mask,
            incoming_missile_count=len(incoming),
            lock_state=lock_state,
        )
