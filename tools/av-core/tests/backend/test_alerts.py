from __future__ import annotations

import sys
import unittest
from pathlib import Path


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from av_core.alerts import AlertEngine  # noqa: E402
from av_core.models import AlertsConfig  # noqa: E402


def derived(player: str = "1", **ratios: float) -> dict[str, object]:
    names = {
        "engine": "engine_integrity_ratio",
        "shield": "shield_ratio",
        "hull": "hull_ratio",
        "weapon": "weapon_energy_ratio",
        "fuel": "fuel_ratio",
    }
    return {
        f"entities.{player}.{names[name]}": {"available": True, "reason": None, "value": value}
        for name, value in ratios.items()
    }


def base_records() -> dict[str, list[dict[str, object]]]:
    return {
        "FLIGHT_STATE": [{"entity_id": "1"}],
        "HUD_ALERT_STATE": [{
            "entity_id": "1", "presence": 0, "primary_fire_threat_active": False,
            "missile_lock_state": 0, "missile_direction_sector_mask": 0,
        }],
        "THREAT_STATE": [{"entity_id": "1", "incoming_missiles": []}],
        "RADAR_STATE": [{"entity_id": "1", "sensor_state": 2}],
        "WEAPON_STATE": [{"entity_id": "1"}],
        "SUBSYSTEM_STATE": [],
    }


class WarningAndCautionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.engine = AlertEngine()
        self.config = AlertsConfig()

    def evaluate(self, records: dict[str, list[dict[str, object]]], values: dict[str, object] | None = None):
        return self.engine.evaluate(
            player_entity_id="1", records=records, derived=values or {}, config=self.config
        )

    def test_warning_sources_and_master_are_not_latched(self) -> None:
        records = base_records()
        hud = records["HUD_ALERT_STATE"][0]
        threat = records["THREAT_STATE"][0]
        hud["primary_fire_threat_active"] = True
        threat["incoming_missiles"] = [{"entity_id": "10"}]
        hud.update({"presence": 1, "warning_kind": 4, "warning_remaining_us": 1000})
        result = self.evaluate(records)
        self.assertTrue(result.warnings.master)
        self.assertTrue(result.warnings.fire)
        self.assertTrue(result.warnings.missile)
        self.assertTrue(result.warnings.blast)

        hud.update({"primary_fire_threat_active": False, "presence": 0})
        threat["incoming_missiles"] = []
        cleared = self.evaluate(records)
        self.assertFalse(cleared.warnings.master)
        self.assertFalse(cleared.warnings.blast)

    def test_collision_emp_and_expired_warning_mapping(self) -> None:
        records = base_records()
        hud = records["HUD_ALERT_STATE"][0]
        for kind, field in ((3, "collision"), (6, "emp")):
            hud.update({"presence": 1, "warning_kind": kind, "warning_remaining_us": 1})
            self.assertTrue(getattr(self.evaluate(records).warnings, field))
        hud["warning_remaining_us"] = 0
        self.assertFalse(self.evaluate(records).warnings.emp)

    def test_strict_hysteresis_and_reset_between_thresholds(self) -> None:
        records = base_records()
        result = self.evaluate(records, derived(engine=0.49))
        self.assertEqual("ACTIVE", result.cautions.engine.state)
        self.assertEqual("ACTIVE", self.evaluate(records, derived(engine=0.50)).cautions.engine.state)
        self.assertEqual("ACTIVE", self.evaluate(records, derived(engine=0.55)).cautions.engine.state)
        self.assertEqual("CLEAR", self.evaluate(records, derived(engine=0.551)).cautions.engine.state)

        reset = self.engine.evaluate(
            player_entity_id="1", records=records, derived=derived(engine=0.52),
            config=self.config, reset_hysteresis=True,
        )
        self.assertEqual("CLEAR", reset.cautions.engine.state)

    def test_selected_ammunition_countermeasures_sensor_and_subsystems(self) -> None:
        records = base_records()
        records["RADAR_STATE"][0]["sensor_state"] = 1
        records["WEAPON_STATE"][0].update({
            "current_primary_bank_id": 2,
            "primary_banks": [
                {"bank_id": 1, "ammunition": {"current": 1, "initial": 100}},
                {"bank_id": 2, "ammunition": {"current": 10, "initial": 100}},
                {"bank_id": 3},
            ],
            "countermeasure": {"current": 3, "maximum": 20},
        })
        records["SUBSYSTEM_STATE"] = [
            {"entity_id": "1", "type": 1, "current_hits": 1, "max_hits": 100},
            {"entity_id": "1", "type": 3, "current_hits": 1, "max_hits": 100},
            {"entity_id": "1", "type": 7, "current_hits": 1, "max_hits": 100},
            {"entity_id": "1", "type": 5, "current_hits": 30, "max_hits": 100},
            {"entity_id": "2", "type": 5, "current_hits": 1, "max_hits": 100},
        ]
        result = self.evaluate(records)
        self.assertEqual("ACTIVE", result.cautions.sensor.state)
        self.assertEqual("ACTIVE", result.cautions.ammo.state)
        self.assertEqual(10.0, result.cautions.ammo.value_percent)
        self.assertEqual("ACTIVE", result.cautions.countermeasures.state)
        self.assertEqual(3, result.cautions.countermeasures.value_count)
        self.assertEqual("ACTIVE", result.cautions.subsystem.state)
        self.assertTrue(result.cautions.master)

    def test_ammunition_uses_the_lowest_selected_bank_and_ignores_energy_weapons(self) -> None:
        records = base_records()
        records["WEAPON_STATE"][0].update({
            "current_primary_bank_id": 1,
            "current_secondary_bank_id": 3,
            "current_tertiary_bank_id": 4,
            "primary_banks": [
                {"bank_id": 1},
                {"bank_id": 2, "ammunition": {"current": 1, "initial": 100}},
            ],
            "secondary_banks": [
                {"bank_id": 3, "ammunition": {"current": 30, "initial": 100}},
            ],
            "tertiary": {
                "bank_id": 4,
                "ammunition_current": 15,
                "ammunition_initial": 100,
            },
        })
        result = self.evaluate(records)
        self.assertEqual("ACTIVE", result.cautions.ammo.state)
        self.assertEqual(15.0, result.cautions.ammo.value_percent)

    def test_missing_values_are_unavailable_not_zero(self) -> None:
        result = self.evaluate(base_records())
        self.assertEqual("UNAVAILABLE", result.cautions.engine.state)
        self.assertIsNone(result.cautions.engine.value_percent)
        self.assertEqual("UNAVAILABLE", result.cautions.ammo.state)


class ThreatSectorTest(unittest.TestCase):
    def evaluate(self, mask: int, missile_count: int = 1, lock: int = 0):
        records = base_records()
        records["HUD_ALERT_STATE"][0]["missile_lock_state"] = lock
        records["HUD_ALERT_STATE"][0]["missile_direction_sector_mask"] = mask
        missiles = [{"entity_id": str(index + 10)} for index in range(missile_count)]
        records["THREAT_STATE"][0]["incoming_missiles"] = missiles
        values = {
            "entities.1.missiles.10.relative_position_local": {
                "available": True, "reason": None, "value": [999, 0, -999]
            }
        }
        return AlertEngine().evaluate(
            player_entity_id="1", records=records, derived=values, config=AlertsConfig()
        ).threat

    def test_uses_the_hud_sector_mask_verbatim(self) -> None:
        self.assertEqual(0xFF, self.evaluate(0xFF, missile_count=8).sector_mask)
        self.assertEqual(0x85, self.evaluate(0x85, missile_count=3).sector_mask)

    def test_count_stays_independent_and_derived_geometry_is_ignored(self) -> None:
        result = self.evaluate(0x01, missile_count=4)
        self.assertEqual(0x01, result.sector_mask)
        self.assertEqual(4, result.incoming_missile_count)

    def test_invalid_masks_clear_and_lock_states_are_closed(self) -> None:
        self.assertEqual(0, self.evaluate(256, lock=1).sector_mask)
        self.assertEqual("ATTEMPT", self.evaluate(1, lock=1).lock_state)
        self.assertEqual("ACQUIRED", self.evaluate(1, lock=2).lock_state)
        self.assertEqual("NONE", self.evaluate(1, lock=99).lock_state)


if __name__ == "__main__":
    unittest.main()
