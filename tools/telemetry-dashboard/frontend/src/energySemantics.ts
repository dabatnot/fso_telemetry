import type { DashboardSnapshot } from "./types";

export interface FlagDefinition {
  mask: number;
  label: string;
}

export const ETS_MODES: Record<number, string> = {
  0: "ABSENT",
  1: "DISPONIBLE",
  2: "VERROUILLÉ"
};

export const PROPULSION_FLAGS: FlagDefinition[] = [
  { mask: 0x0001, label: "AFTERBURNER DISPONIBLE" },
  { mask: 0x0002, label: "AFTERBURNER VERROUILLÉ" },
  { mask: 0x0004, label: "AFTERBURNER ACTIF" },
  { mask: 0x0008, label: "AFTERBURNER DEMANDÉ" },
  { mask: 0x0010, label: "BOOSTER ACTIF" },
  { mask: 0x0020, label: "GLIDE ACTIF" },
  { mask: 0x0040, label: "GLIDE FORCÉ" },
  { mask: 0x0080, label: "RCS ACTIF" }
];

export function etsModeLabel(value: unknown): string {
  const numeric = Number(value);
  return Number.isInteger(numeric) && ETS_MODES[numeric] ? ETS_MODES[numeric] : "INCONNU";
}

export function decodePropulsionFlags(value: unknown): string[] | null {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 0) return null;
  return PROPULSION_FLAGS
    .filter((definition) => (numeric & definition.mask) !== 0)
    .map((definition) => definition.label);
}

export function playerClassManifest(snapshot: DashboardSnapshot | null): Record<string, unknown> | null {
  if (!snapshot?.playerEntityId) return null;
  const identity = (snapshot.records.SHIP_IDENTITY ?? []).find(
    (record) => String(record.entity_id) === String(snapshot.playerEntityId)
  );
  if (!identity?.ship_class_id) return null;
  for (const entry of Object.values(snapshot.manifest.records)) {
    if (!entry || typeof entry !== "object") continue;
    const record = entry as { recordName?: string; fields?: Record<string, unknown> };
    if (
      record.recordName === "CLASS_MANIFEST" &&
      String(record.fields?.class_id) === String(identity.ship_class_id)
    ) {
      return record.fields ?? null;
    }
  }
  return null;
}

export function forwardComponent(value: unknown): number | null {
  if (!Array.isArray(value) || value.length !== 3) return null;
  const result = Number(value[2]);
  return Number.isFinite(result) ? result : null;
}
