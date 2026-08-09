import { playerClassManifest } from "./energySemantics";
import type { DashboardSnapshot } from "./types";

export interface FlagDefinition {
  mask: number;
  label: string;
}

export const PROTECTION_FLAGS: FlagDefinition[] = [
  { mask: 0x0001, label: "INVULNÉRABLE" },
  { mask: 0x0002, label: "PROTÉGÉ" },
  { mask: 0x0004, label: "GUARDIAN" }
];

export const LIFECYCLE_FLAGS: FlagDefinition[] = [
  { mask: 0x0001, label: "DYING" },
  { mask: 0x0002, label: "DISABLED" },
  { mask: 0x0004, label: "EXPLODED" },
  { mask: 0x0008, label: "SHOULD BE DEAD" },
  { mask: 0x0010, label: "BOMB" }
];

export const SUBSYSTEM_FLAGS: FlagDefinition[] = [
  { mask: 0x00000001, label: "PERTURBÉ" },
  { mask: 0x00000002, label: "CIBLABLE" },
  { mask: 0x00000004, label: "VISIBLE" },
  { mask: 0x00000008, label: "RÉVÉLÉ" },
  { mask: 0x00000010, label: "GUARDIAN" },
  { mask: 0x00000020, label: "MOUVEMENT VERROUILLÉ" },
  { mask: 0x00000040, label: "BEAM LIBRE" },
  { mask: 0x00000080, label: "BEAM VERROUILLÉ" }
];

export const SUBSYSTEM_TYPES: Record<number, string> = {
  0: "INCONNU",
  1: "MOTEUR",
  2: "TOURELLE",
  3: "RADAR",
  4: "NAVIGATION",
  5: "COMMUNICATION",
  6: "ARMES",
  7: "CAPTEURS",
  8: "RÉACTEUR",
  9: "MANŒUVRE",
  10: "HANGAR",
  11: "CARGO",
  12: "AWACS",
  13: "AUTRE"
};

export const STANDARD_SHIELD_QUADRANTS: Record<number, string> = {
  0: "DROITE",
  1: "AVANT",
  2: "ARRIÈRE",
  3: "GAUCHE"
};

export interface SubsystemView {
  record: Record<string, unknown>;
  definition: Record<string, unknown> | null;
  entityId: string;
  subsystemId: string;
  canonicalIndex: number;
  name: string;
  typeLabel: string;
  flags: string[];
  ratio: number | null;
  destroyed: boolean;
  alert: boolean;
  cooldownUs: number | null;
}

export function decodeFlags(value: unknown, definitions: FlagDefinition[]): string[] | null {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 0) return null;
  return definitions
    .filter((definition) => (numeric & definition.mask) !== 0)
    .map((definition) => definition.label);
}

export function subsystemTypeLabel(value: unknown): string {
  const numeric = Number(value);
  return Number.isInteger(numeric) && SUBSYSTEM_TYPES[numeric]
    ? SUBSYSTEM_TYPES[numeric]
    : "INCONNU";
}

export function shieldQuadrantLabel(value: unknown): string {
  const numeric = Number(value);
  return Number.isInteger(numeric) && STANDARD_SHIELD_QUADRANTS[numeric]
    ? STANDARD_SHIELD_QUADRANTS[numeric]
    : "INCONNU";
}

export const SHIELD_ARC_PATH_LENGTH = 400;
export const SHIELD_ARC_SPAN = 80;

export function shieldArcGeometry(centerRotation: number, ratio: number) {
  const boundedRatio = Number.isFinite(ratio)
    ? Math.max(0, Math.min(1, ratio))
    : 0;
  const halfSpanDegrees = (SHIELD_ARC_SPAN / SHIELD_ARC_PATH_LENGTH) * 180;

  return {
    trackRotation: centerRotation - halfSpanDegrees,
    valueRotation: centerRotation - halfSpanDegrees * boundedRatio,
    valueLength: SHIELD_ARC_SPAN * boundedRatio
  };
}

export function playerRecord(
  snapshot: DashboardSnapshot | null,
  recordName: string
): Record<string, unknown> | null {
  if (!snapshot?.playerEntityId) return null;
  return (snapshot.records[recordName] ?? []).find(
    (record) => String(record.entity_id) === String(snapshot.playerEntityId)
  ) ?? null;
}

export function playerRecords(
  snapshot: DashboardSnapshot | null,
  recordName: string
): Array<Record<string, unknown>> {
  if (!snapshot?.playerEntityId) return [];
  return (snapshot.records[recordName] ?? []).filter(
    (record) => String(record.entity_id) === String(snapshot.playerEntityId)
  );
}

export function classSubsystemDefinitions(
  snapshot: DashboardSnapshot | null
): Array<Record<string, unknown>> {
  const shipClass = playerClassManifest(snapshot);
  return Array.isArray(shipClass?.subsystem_definitions)
    ? shipClass.subsystem_definitions.filter(
        (item): item is Record<string, unknown> => Boolean(item) && typeof item === "object"
      )
    : [];
}

export function subsystemDefinition(
  snapshot: DashboardSnapshot | null,
  record: Record<string, unknown>
): Record<string, unknown> | null {
  const definitions = classSubsystemDefinitions(snapshot);
  return definitions.find(
    (definition) => String(definition.subsystem_id) === String(record.subsystem_id)
  ) ?? definitions.find(
    (definition) => Number(definition.canonical_index) === Number(record.canonical_index)
  ) ?? null;
}

function finiteNumber(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function subsystemName(
  record: Record<string, unknown>,
  definition: Record<string, unknown> | null
): string {
  for (const value of [
    record.hud_name_override,
    record.alternate_name_override,
    record.internal_name_override,
    definition?.hud_name,
    definition?.alternate_name,
    definition?.internal_name
  ]) {
    if (typeof value === "string" && value.trim()) return value;
  }
  return `SOUS-SYSTÈME ${String(record.subsystem_id ?? "?")}`;
}

export function subsystemViews(snapshot: DashboardSnapshot | null): SubsystemView[] {
  return playerRecords(snapshot, "SUBSYSTEM_STATE").map((record) => {
    const definition = subsystemDefinition(snapshot, record);
    const current = finiteNumber(record.current_hits);
    const maximum = finiteNumber(record.max_hits);
    const ratio = current !== null && maximum !== null && maximum > 0
      ? Math.max(0, Math.min(1, current / maximum))
      : null;
    const flags = decodeFlags(record.subsystem_flags, SUBSYSTEM_FLAGS) ?? [];
    const turret = record.turret && typeof record.turret === "object"
      ? record.turret as Record<string, unknown>
      : null;
    const cooldown = finiteNumber(turret?.cooldown_remaining_us);
    return {
      record,
      definition,
      entityId: String(record.entity_id ?? ""),
      subsystemId: String(record.subsystem_id ?? ""),
      canonicalIndex: Number.isInteger(Number(record.canonical_index))
        ? Number(record.canonical_index)
        : Number.MAX_SAFE_INTEGER,
      name: subsystemName(record, definition),
      typeLabel: subsystemTypeLabel(record.type),
      flags,
      ratio,
      destroyed: maximum !== null && maximum > 0 && current !== null && current <= 0,
      alert: flags.includes("PERTURBÉ") || flags.includes("MOUVEMENT VERROUILLÉ"),
      cooldownUs: cooldown
    };
  }).sort((left, right) => {
    if (left.destroyed !== right.destroyed) return left.destroyed ? -1 : 1;
    if (left.alert !== right.alert) return left.alert ? -1 : 1;
    const leftRatio = left.ratio ?? Number.POSITIVE_INFINITY;
    const rightRatio = right.ratio ?? Number.POSITIVE_INFINITY;
    if (leftRatio !== rightRatio) return leftRatio - rightRatio;
    return left.canonicalIndex - right.canonicalIndex;
  });
}
