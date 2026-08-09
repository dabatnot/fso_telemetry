import { describe, expect, it } from "vitest";
import catalogData from "./instrument-catalog.json";
import { resolveInstrument } from "./data";
import {
  decodePropulsionFlags,
  etsModeLabel,
  forwardComponent,
  playerClassManifest
} from "./energySemantics";
import type { DashboardSnapshot, InstrumentDefinition } from "./types";

const snapshot: DashboardSnapshot = {
  schema: "DashboardSnapshotV1",
  publishedAtUtc: "2026-01-01T00:00:00Z",
  mode: "live",
  connection: {
    status: "Live",
    host: "127.0.0.1",
    port: 42042,
    sessionId: "1",
    lastLiveObservedUtc: null,
    staleReason: null
  },
  session: {},
  mission: {},
  playerEntityId: "7",
  records: {
    SHIP_IDENTITY: [{ entity_id: "7", ship_class_id: 3 }],
    ENERGY_STATE: [{ entity_id: "7", ets_mode: 1 }],
    PROPULSION_STATE: [{ entity_id: "7", propulsion_flags: 0x25, fuel_current: 0, fuel_max: 100 }]
  },
  recordInstances: {},
  manifest: {
    id: 1,
    records: {
      "CLASS_MANIFEST/class_id=3": {
        recordName: "CLASS_MANIFEST",
        fields: {
          class_id: 3,
          internal_name: "GTF Test",
          max_velocity: [0, 0, 80],
          afterburner_max_velocity: [0, 0, 140]
        }
      }
    }
  },
  derived: {
    "entities.7.fuel_ratio": { available: true, value: 0 }
  },
  transport: { synchronized: true, baseline: 1, deltaSequence: 1, manifestId: 1 },
  quality: {
    packets: 1,
    transportGapCount: 0,
    decodeErrorCount: 0,
    resyncCount: 0,
    channels: []
  },
  capture: { active: false, path: null },
  replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
};

describe("energy display semantics", () => {
  it("decodes ETS modes and closed propulsion flags", () => {
    expect([0, 1, 2].map(etsModeLabel)).toEqual(["ABSENT", "DISPONIBLE", "VERROUILLÉ"]);
    expect(decodePropulsionFlags(0x25)).toEqual([
      "AFTERBURNER DISPONIBLE", "AFTERBURNER ACTIF", "GLIDE ACTIF"
    ]);
    expect(decodePropulsionFlags(Number.NaN)).toBeNull();
  });

  it("resolves the player's current class and forward nominal values", () => {
    expect(playerClassManifest(snapshot)?.internal_name).toBe("GTF Test");
    expect(forwardComponent([0, 0, 140])).toBe(140);
    expect(forwardComponent([0, 1])).toBeNull();
  });

  it("supports manifest-class sources and preserves a real zero fuel ratio", () => {
    const manifestResult = resolveInstrument({
      id: "performance",
      tab: "energy",
      label: "Performance",
      source: "manifest-class:max_velocity",
      component: "vector"
    }, snapshot);
    expect(manifestResult.state).toBe("live");
    expect(manifestResult.value).toEqual([0, 0, 80]);
    const fuelResult = resolveInstrument({
      id: "fuel",
      tab: "energy",
      label: "Carburant",
      source: "derived:entities.$player.fuel_ratio",
      component: "radial"
    }, snapshot);
    expect(fuelResult.state).toBe("live");
    expect(fuelResult.value).toBe(0);
  });
});

describe("energy catalog coverage", () => {
  it("classifies every known ENERGY_STATE and PROPULSION_STATE field", () => {
    const expected = [
      "ENERGY_STATE.entity_id", "ENERGY_STATE.presence", "ENERGY_STATE.producer_sample_time_us",
      "ENERGY_STATE.reserved", "ENERGY_STATE.ets_mode", "ENERGY_STATE.ets_shields_index",
      "ENERGY_STATE.ets_weapons_index", "ENERGY_STATE.ets_engines_index",
      "ENERGY_STATE.weapon_energy_current", "ENERGY_STATE.weapon_energy_max",
      "ENERGY_STATE.weapon_regeneration_per_s", "ENERGY_STATE.shield_regeneration_per_s",
      "ENERGY_STATE.deferred_to_weapons", "ENERGY_STATE.deferred_to_shields",
      "ENERGY_STATE.resulting_engine_power", "ENERGY_STATE.resulting_max_speed",
      "ENERGY_STATE.power_output", "ENERGY_STATE.aggregate_engine_current_hits",
      "ENERGY_STATE.aggregate_engine_max_hits",
      "PROPULSION_STATE.entity_id", "PROPULSION_STATE.presence",
      "PROPULSION_STATE.producer_sample_time_us", "PROPULSION_STATE.reserved",
      "PROPULSION_STATE.propulsion_flags", "PROPULSION_STATE.fuel_current",
      "PROPULSION_STATE.fuel_max", "PROPULSION_STATE.consumption_per_s",
      "PROPULSION_STATE.recovery_per_s", "PROPULSION_STATE.minimum_to_engage",
      "PROPULSION_STATE.cooldown_remaining_us", "PROPULSION_STATE.time_since_last_stop_us",
      "PROPULSION_STATE.fuel_at_last_engagement", "PROPULSION_STATE.forward_accel_time_const_s",
      "PROPULSION_STATE.afterburner_max_velocity_local",
      "PROPULSION_STATE.engine_wash_intensity", "PROPULSION_STATE.rcs_intensity"
    ];
    const definitions = (catalogData as InstrumentDefinition[]).filter((item) => item.tab === "energy");
    const classified = new Set(definitions.flatMap((item) => [
      ...(item.consumedFields ?? []),
      ...(item.detailFields ?? []),
      ...(item.reservedFields ?? [])
    ]));
    expect(expected.filter((field) => !classified.has(field))).toEqual([]);
  });
});
