import { describe, expect, it } from "vitest";
import catalogData from "./instrument-catalog.json";
import { resolveInstrument } from "./data";
import {
  decodeFlags,
  LIFECYCLE_FLAGS,
  playerRecords,
  PROTECTION_FLAGS,
  shieldArcGeometry,
  shieldQuadrantLabel,
  SUBSYSTEM_FLAGS,
  subsystemTypeLabel,
  subsystemViews
} from "./integritySemantics";
import type { DashboardSnapshot, InstrumentDefinition } from "./types";

function makeSnapshot(): DashboardSnapshot {
  return {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live",
      host: "127.0.0.1",
      port: 42042,
      sessionId: "42",
      lastLiveObservedUtc: null,
      staleReason: null
    },
    session: {},
    mission: {},
    playerEntityId: "7",
    records: {
      SHIP_IDENTITY: [{ entity_id: "7", ship_class_id: 3 }],
      DAMAGE_STATE: [{
        entity_id: "7",
        hull_strength: 0,
        dynamic_max_hull: 100,
        protection_flags: 0x0005
      }],
      SHIELD_STATE: [{
        entity_id: "7",
        has_shields: 1,
        segment_current_hits: [20, 40],
        segment_max_hits: [100, 50]
      }],
      SUBSYSTEM_STATE: [
        { entity_id: "7", subsystem_id: 1, canonical_index: 0, type: 1, current_hits: 80, max_hits: 100, subsystem_flags: 0 },
        { entity_id: "7", subsystem_id: 2, canonical_index: 1, type: 2, current_hits: 0, max_hits: 100, subsystem_flags: 0, turret: { cooldown_remaining_us: 0 } },
        { entity_id: "7", subsystem_id: 3, canonical_index: 2, type: 7, current_hits: 90, max_hits: 100, subsystem_flags: 0x21 },
        { entity_id: "7", subsystem_id: 4, canonical_index: 3, type: 8, current_hits: 0, max_hits: 0, subsystem_flags: 0 },
        { entity_id: "99", subsystem_id: 5, canonical_index: 0, type: 1, current_hits: 0, max_hits: 100, subsystem_flags: 1 }
      ]
    },
    recordInstances: {},
    manifest: {
      id: 1,
      records: {
        "CLASS_MANIFEST/class_id=3": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 3,
            subsystem_definitions: [
              { subsystem_id: 1, canonical_index: 0, internal_name: "Engine", type: 1 },
              { subsystem_id: 2, canonical_index: 1, hud_name: "Tourelle avant", type: 2 },
              { subsystem_id: 3, canonical_index: 2, alternate_name: "Senseurs", type: 7 },
              { subsystem_id: 4, canonical_index: 3, internal_name: "Reactor", type: 8 }
            ]
          }
        },
        "CLASS_MANIFEST/class_id=4": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 4,
            subsystem_definitions: [
              { subsystem_id: 1, canonical_index: 0, internal_name: "Moteur classe B", type: 1 }
            ]
          }
        }
      }
    },
    derived: {
      "entities.7.hull_ratio": { available: true, value: 0 },
      "entities.7.shield_ratio": { available: false, reason: "shields-absent", value: null }
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
}

describe("integrity display semantics", () => {
  it("decodes closed protection, lifecycle, subsystem flags and types", () => {
    expect(decodeFlags(0x0005, PROTECTION_FLAGS)).toEqual(["INVULNÉRABLE", "GUARDIAN"]);
    expect(decodeFlags(0x0003, LIFECYCLE_FLAGS)).toEqual(["DYING", "DISABLED"]);
    expect(decodeFlags(0x21, SUBSYSTEM_FLAGS)).toEqual(["PERTURBÉ", "MOUVEMENT VERROUILLÉ"]);
    expect(decodeFlags(Number.NaN, SUBSYSTEM_FLAGS)).toBeNull();
    expect(subsystemTypeLabel(2)).toBe("TOURELLE");
    expect(subsystemTypeLabel(99)).toBe("INCONNU");
    expect([0, 1, 2, 3].map(shieldQuadrantLabel)).toEqual([
      "DROITE", "AVANT", "ARRIÈRE", "GAUCHE"
    ]);
    expect(shieldQuadrantLabel(4)).toBe("INCONNU");
  });

  it("centers shield quadrants and keeps partial energy symmetric", () => {
    expect(shieldArcGeometry(270, 1)).toEqual({
      trackRotation: 234,
      valueRotation: 234,
      valueLength: 80
    });
    expect(shieldArcGeometry(270, 0.5)).toEqual({
      trackRotation: 234,
      valueRotation: 252,
      valueLength: 40
    });
    expect(shieldArcGeometry(270, 0)).toEqual({
      trackRotation: 234,
      valueRotation: 270,
      valueLength: 0
    });
    expect(shieldArcGeometry(90, 2).valueRotation).toBe(54);
    expect(shieldArcGeometry(90, -1).valueRotation).toBe(90);
  });

  it("filters the player, resolves manifest names and sorts alerts before ordinary damage", () => {
    const snapshot = makeSnapshot();
    expect(playerRecords(snapshot, "SUBSYSTEM_STATE")).toHaveLength(4);
    const systems = subsystemViews(snapshot);
    expect(systems.map((system) => system.subsystemId)).toEqual(["2", "3", "1", "4"]);
    expect(systems.map((system) => system.name)).toEqual([
      "Tourelle avant", "Senseurs", "Engine", "Reactor"
    ]);
    expect(systems[0].destroyed).toBe(true);
    expect(systems[0].cooldownUs).toBe(0);
    expect(systems[1].alert).toBe(true);
    expect(systems[3].ratio).toBeNull();
    expect(systems[3].destroyed).toBe(false);
  });

  it("follows a class change when resolving subsystem definitions", () => {
    const snapshot = makeSnapshot();
    snapshot.records.SHIP_IDENTITY[0].ship_class_id = 4;
    expect(subsystemViews(snapshot).find((system) => system.subsystemId === "1")?.name)
      .toBe("Moteur classe B");
  });

  it("preserves zero, not-applicable, ND, stale, invalid and waiting", () => {
    const snapshot = makeSnapshot();
    const hull: InstrumentDefinition = {
      id: "hull", tab: "integrity", label: "Coque",
      source: "record:DAMAGE_STATE.hull_strength", component: "bar"
    };
    expect(resolveInstrument(hull, snapshot)).toMatchObject({ state: "live", value: 0 });
    expect(resolveInstrument({
      ...hull,
      id: "ratio",
      source: "derived:entities.$player.shield_ratio"
    }, snapshot).state).toBe("not_applicable");
    expect(resolveInstrument({
      ...hull,
      id: "future",
      source: "future:DAMAGE_DIRECTION",
      availability: "nd"
    }, snapshot).state).toBe("nd");
    snapshot.connection.status = "Stale";
    expect(resolveInstrument(hull, snapshot).state).toBe("stale");
    snapshot.connection.status = "Live";
    snapshot.records.DAMAGE_STATE[0].hull_strength = Number.NaN;
    expect(resolveInstrument(hull, snapshot).state).toBe("invalid");
    expect(resolveInstrument(hull, null).state).toBe("waiting");
  });
});

describe("integrity catalog coverage", () => {
  it("classifies every known damage, shield and subsystem field", () => {
    const expected = [
      "DAMAGE_STATE.entity_id", "DAMAGE_STATE.presence", "DAMAGE_STATE.producer_sample_time_us",
      "DAMAGE_STATE.hull_strength", "DAMAGE_STATE.dynamic_max_hull",
      "DAMAGE_STATE.protection_flags", "DAMAGE_STATE.sim_hull_strength",
      "DAMAGE_STATE.armor_id", "DAMAGE_STATE.guardian_threshold",
      "DAMAGE_STATE.cumulative_damage", "DAMAGE_STATE.last_damage_source_entity_id",
      "DAMAGE_STATE.last_damage_weapon_class_id", "DAMAGE_STATE.contributors",
      "SHIELD_STATE.entity_id", "SHIELD_STATE.presence", "SHIELD_STATE.producer_sample_time_us",
      "SHIELD_STATE.has_shields", "SHIELD_STATE.segment_count", "SHIELD_STATE.reserved",
      "SHIELD_STATE.segment_current_hits", "SHIELD_STATE.segment_max_hits",
      "SHIELD_STATE.recharge_max", "SHIELD_STATE.regeneration_per_s",
      "SHIELD_STATE.deferred_energy_transfer",
      "SUBSYSTEM_STATE.entity_id", "SUBSYSTEM_STATE.subsystem_id", "SUBSYSTEM_STATE.presence",
      "SUBSYSTEM_STATE.producer_sample_time_us", "SUBSYSTEM_STATE.canonical_index",
      "SUBSYSTEM_STATE.type", "SUBSYSTEM_STATE.current_hits", "SUBSYSTEM_STATE.max_hits",
      "SUBSYSTEM_STATE.subsystem_flags", "SUBSYSTEM_STATE.internal_name_override",
      "SUBSYSTEM_STATE.alternate_name_override", "SUBSYSTEM_STATE.hud_name_override",
      "SUBSYSTEM_STATE.armor_id", "SUBSYSTEM_STATE.perturbation_remaining_us",
      "SUBSYSTEM_STATE.animated_translation_local", "SUBSYSTEM_STATE.animated_orientation_local",
      "SUBSYSTEM_STATE.animations", "SUBSYSTEM_STATE.cargo_disclosure",
      "SUBSYSTEM_STATE.cargo_text", "SUBSYSTEM_STATE.aggregate_current_hits",
      "SUBSYSTEM_STATE.aggregate_max_hits", "SUBSYSTEM_STATE.turret"
    ];
    const definitions = (catalogData as InstrumentDefinition[]).filter(
      (item) => item.tab === "integrity"
    );
    const classified = new Set(definitions.flatMap((item) => [
      ...(item.consumedFields ?? []),
      ...(item.detailFields ?? []),
      ...(item.reservedFields ?? [])
    ]));
    expect(expected.filter((field) => !classified.has(field))).toEqual([]);
  });
});
