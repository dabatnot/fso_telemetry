import { describe, expect, it } from "vitest";
import catalog from "./instrument-catalog.json";
import {
  CARGO_PHASES,
  CARGO_VALIDITY_FLAGS,
  DOCKING_PHASES,
  SUPPORT_FLAGS,
  SUPPORT_PHASES,
  cargoSubsystemName,
  decodeClosed,
  decodeFlags,
  dockingNodes,
  dockingRelations,
  entityReferenceIsInvalid,
  serviceRows,
  supportReferenceInvalid,
  visibleDockingNodes
} from "./supportSemantics";
import type { DashboardSnapshot } from "./types";

function snapshot(): DashboardSnapshot {
  const identities = Array.from({ length: 10 }, (_, index) => ({
    entity_id: String(index + 1),
    ship_class_id: index === 2 ? 8 : 7,
    internal_name: index === 0 ? "Joueur" : index === 1 ? "Support Alpha" : `Docké ${index + 1}`
  }));
  const lifecycles = identities.map((identity) => ({
    entity_id: identity.entity_id,
    lifecycle_phase: 1
  }));
  const docking = identities.map((identity, index) => {
    const relations: Array<Record<string, unknown>> = [];
    if (index === 0) {
      for (let remote = 2; remote <= 10; remote += 1) {
        relations.push({
          item_version: 1,
          item_size: 32,
          remote_entity_id: String(remote),
          local_dockpoint: remote,
          remote_dockpoint: 1,
          local_dock_bay_name: `Port ${remote}`,
          remote_dock_bay_name: "Port principal"
        });
      }
    } else {
      relations.push({
        item_version: 1,
        item_size: 32,
        remote_entity_id: "1",
        local_dockpoint: 1,
        remote_dockpoint: index + 1,
        local_dock_bay_name: "Port principal",
        remote_dock_bay_name: `Port ${index + 1}`
      });
    }
    return {
      entity_id: identity.entity_id,
      presence: "0",
      producer_sample_time_us: "1000000",
      phase: index === 0 ? 3 : 0,
      group_leader_entity_id: "1",
      relations
    };
  });
  return {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live",
      host: "127.0.0.1",
      port: 42042,
      sessionId: "45",
      lastLiveObservedUtc: "2026-08-03T10:00:00Z",
      staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "1",
    records: {
      SHIP_IDENTITY: identities,
      ENTITY_LIFECYCLE: lifecycles,
      SUPPORT_STATE: [{
        entity_id: "1",
        presence: "1",
        producer_sample_time_us: "1000000",
        phase: 2,
        support_flags: 1,
        support_entity_id: "2"
      }],
      DOCKING_STATE: docking,
      CARGO_SCAN_STATE: [{
        entity_id: "1",
        presence: "31",
        producer_sample_time_us: "1000000",
        scan_phase: 2,
        disclosure: 1,
        target_entity_id: "3",
        target_subsystem_id: 9,
        elapsed_us: "2500000",
        required_us: "10000000",
        validity_flags: 7,
        cargo_text: "Munitions"
      }],
      DAMAGE_STATE: [{ entity_id: "1", hull_strength: 50, dynamic_max_hull: 100 }],
      SHIELD_STATE: [{ entity_id: "1", has_shields: 1 }],
      SUBSYSTEM_STATE: [
        { entity_id: "1", subsystem_id: 4, current_hits: 20, max_hits: 100 }
      ],
      WEAPON_STATE: [{
        entity_id: "1",
        primary_banks: [{ ammunition: { current: 5, initial: 10 } }],
        secondary_banks: [{ ammunition: { current: 2, initial: 8 } }],
        tertiary: { ammunition_current: 0, ammunition_initial: 4 },
        countermeasure: { current: 3, maximum: 6 }
      }]
    },
    recordInstances: {},
    manifest: {
      id: 4,
      records: {
        "CLASS_MANIFEST/class_id=8": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 8,
            subsystem_definitions: [{
              subsystem_id: 9,
              internal_name: "Conteneur central"
            }]
          }
        }
      }
    },
    derived: {
      "entities.1.hull_ratio": { available: true, value: 0.5 },
      "entities.1.shield_ratio": { available: true, value: 0.75 },
      "entities.1.subsystems.4.integrity_ratio": { available: true, value: 0.2 },
      "entities.1.primary_banks[0].ammo_ratio": { available: true, value: 0.5 },
      "entities.1.secondary_banks[0].ammo_ratio": { available: true, value: 0.25 },
      "entities.1.tertiary.ammo_ratio": { available: true, value: 0 },
      "entities.1.countermeasure.quantity_ratio": { available: true, value: 0.5 },
      "entities.1.cargo.progress_ratio": { available: true, value: 0.25 },
      "entities.1.cargo.remaining_us": { available: true, value: 7500000 },
      "entities.1.support.distance": { available: true, value: 500 },
      "entities.1.support.relative_speed": { available: true, value: 50 },
      "entities.1.support.closing_speed": { available: true, value: 42 }
    },
    transport: { synchronized: true, baseline: 4, deltaSequence: 8, manifestId: 4 },
    quality: {
      packets: 10,
      transportGapCount: 0,
      decodeErrorCount: 0,
      resyncCount: 0,
      channels: []
    },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
}

describe("support, docking and cargo semantics", () => {
  it("decodes every closed registry and rejects invalid values", () => {
    expect(Object.values(SUPPORT_PHASES)).toHaveLength(8);
    expect(Object.values(DOCKING_PHASES)).toHaveLength(5);
    expect(Object.values(CARGO_PHASES)).toHaveLength(4);
    expect(decodeClosed(4, SUPPORT_PHASES)).toBe("RÉPARATION");
    expect(decodeClosed(9, SUPPORT_PHASES)).toBeNull();
    expect(decodeFlags(0x05, SUPPORT_FLAGS)).toEqual([
      "ATTENTE RÉPARATION",
      "INTERVIENT SUR UN AUTRE"
    ]);
    expect(decodeFlags(0x07, CARGO_VALIDITY_FLAGS)).toHaveLength(3);
    expect(decodeFlags(Number.NaN, SUPPORT_FLAGS)).toBeNull();
  });

  it("resolves cargo target subsystem names from the target class", () => {
    expect(cargoSubsystemName(snapshot(), "3", 9)).toBe("Conteneur central");
    expect(cargoSubsystemName(snapshot(), "3", undefined)).toBe("— vaisseau complet");
  });

  it("reconstructs reciprocal docking and keeps player, support and direct nodes", () => {
    const value = snapshot();
    const relations = dockingRelations(value);
    expect(relations).toHaveLength(18);
    expect(relations.every((relation) => relation.reciprocal)).toBe(true);
    const visible = visibleDockingNodes(value);
    expect(visible).toHaveLength(8);
    expect(visible[0].player).toBe(true);
    expect(visible[1].support).toBe(true);
    expect(dockingNodes(value)).toHaveLength(10);
  });

  it("fails closed when a synchronized entity reference cannot be resolved", () => {
    const value = snapshot();
    expect(entityReferenceIsInvalid(value, "2")).toBe(false);
    value.records.SHIP_IDENTITY = value.records.SHIP_IDENTITY.filter(
      (identity) => String(identity.entity_id) !== "2"
    );
    expect(entityReferenceIsInvalid(value, "2")).toBe(true);
    expect(supportReferenceInvalid(value)).toBe(true);
  });

  it("keeps service resources separate and preserves a real zero", () => {
    const rows = serviceRows(snapshot());
    expect(rows.map((row) => row.id)).toEqual([
      "hull", "shield", "subsystems", "primary", "secondary", "tertiary", "countermeasure"
    ]);
    expect(rows.find((row) => row.id === "tertiary")?.ratio).toBe(0);
    expect(rows.some((row) => row.id.includes("global"))).toBe(false);
  });

  it("declares exhaustive support coverage and keeps future data ND", () => {
    const definitions = catalog.filter((definition) => definition.tab === "support");
    expect(definitions.map((definition) => definition.id)).toEqual([
      "support-overview",
      "support-service",
      "support-approach",
      "support-docking",
      "support-cargo",
      "support-future"
    ]);
    const covered = new Set(definitions.flatMap((definition) => [
      ...(definition.consumedFields ?? []),
      ...(definition.detailFields ?? []),
      ...(definition.reservedFields ?? [])
    ]));
    for (const field of [
      "SUPPORT_STATE.phase",
      "SUPPORT_STATE.support_flags",
      "SUPPORT_STATE.support_entity_id",
      "SUPPORT_STATE.reserved",
      "DOCKING_STATE.group_leader_entity_id",
      "DOCKING_STATE.relations[].remote_entity_id",
      "DOCKING_STATE.relations[].local_dock_bay_name",
      "DOCKING_STATE.relations[].item_version",
      "CARGO_SCAN_STATE.disclosure",
      "CARGO_SCAN_STATE.target_entity_id",
      "CARGO_SCAN_STATE.required_us",
      "CARGO_SCAN_STATE.validity_flags",
      "CARGO_SCAN_STATE.cargo_text"
    ]) expect(covered.has(field), field).toBe(true);
    expect(definitions.at(-1)?.availability).toBe("nd");
  });
});
