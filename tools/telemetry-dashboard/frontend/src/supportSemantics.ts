import { playerRecord } from "./integritySemantics";
import type { DashboardSnapshot } from "./types";

export const SUPPORT_PHASES: Record<number, string> = {
  0: "AUCUN",
  1: "DEMANDÉ",
  2: "EN APPROCHE",
  3: "AMARRAGE",
  4: "RÉPARATION",
  5: "RÉARMEMENT",
  6: "OBSTRUCTION",
  7: "ABANDONNÉ"
};

export const DOCKING_PHASES: Record<number, string> = {
  0: "AUCUN",
  1: "APPROCHE",
  2: "AMARRAGE",
  3: "AMARRÉ",
  4: "SÉPARATION"
};

export const CARGO_PHASES: Record<number, string> = {
  0: "NON SCANNABLE",
  1: "EN ATTENTE",
  2: "SCAN EN COURS",
  3: "TERMINÉ"
};

export const DISCLOSURE_STATES: Record<number, string> = {
  0: "MASQUÉ",
  1: "RÉVÉLÉ"
};

export const SUPPORT_FLAGS = [
  { mask: 0x01, label: "ATTENTE RÉPARATION" },
  { mask: 0x02, label: "PRISE EN CHARGE" },
  { mask: 0x04, label: "INTERVIENT SUR UN AUTRE" }
] as const;

export const CARGO_VALIDITY_FLAGS = [
  { mask: 0x01, label: "PORTÉE" },
  { mask: 0x02, label: "ANGLE" },
  { mask: 0x04, label: "LIGNE DE VUE" }
] as const;

export interface DockingRelationView {
  subjectId: string;
  remoteId: string;
  subjectName: string;
  remoteName: string;
  localDockpoint: number | null;
  remoteDockpoint: number | null;
  localBay: string;
  remoteBay: string;
  reciprocal: boolean;
  record: Record<string, unknown>;
  relation: Record<string, unknown>;
}

export interface DockingNodeView {
  entityId: string;
  name: string;
  player: boolean;
  support: boolean;
  direct: boolean;
  leader: boolean;
  lifecycle: string;
}

export interface ServiceRow {
  id: string;
  label: string;
  ratio: number | null;
  value: string;
  reason?: string;
}

function finite(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function boundedRatio(value: unknown): number | null {
  const numeric = finite(value);
  return numeric === null ? null : Math.max(0, Math.min(1, numeric));
}

export function decodeClosed(
  value: unknown,
  labels: Record<number, string>
): string | null {
  const numeric = Number(value);
  return Number.isInteger(numeric) && labels[numeric] ? labels[numeric] : null;
}

export function decodeFlags(
  value: unknown,
  definitions: ReadonlyArray<{ mask: number; label: string }>
): string[] | null {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 0) return null;
  return definitions
    .filter((definition) => (numeric & definition.mask) !== 0)
    .map((definition) => definition.label);
}

export function supportState(
  snapshot: DashboardSnapshot | null
): Record<string, unknown> | null {
  return playerRecord(snapshot, "SUPPORT_STATE");
}

export function dockingState(
  snapshot: DashboardSnapshot | null
): Record<string, unknown> | null {
  return playerRecord(snapshot, "DOCKING_STATE");
}

export function cargoState(
  snapshot: DashboardSnapshot | null
): Record<string, unknown> | null {
  return playerRecord(snapshot, "CARGO_SCAN_STATE");
}

export function entityRecord(
  snapshot: DashboardSnapshot | null,
  recordName: string,
  entityId: unknown
): Record<string, unknown> | null {
  if (!snapshot || entityId === undefined || entityId === null) return null;
  return (snapshot.records[recordName] ?? []).find(
    (record) => String(record.entity_id) === String(entityId)
  ) ?? null;
}

export function entityName(
  snapshot: DashboardSnapshot | null,
  entityId: unknown
): string {
  if (entityId === undefined || entityId === null || String(entityId) === "0") return "—";
  const identity = entityRecord(snapshot, "SHIP_IDENTITY", entityId);
  const name = typeof identity?.internal_name === "string"
    ? identity.internal_name.trim()
    : "";
  return name || `ENTITÉ ${String(entityId)}`;
}

export function entityReferenceIsInvalid(
  snapshot: DashboardSnapshot | null,
  entityId: unknown
): boolean {
  if (!snapshot?.transport.synchronized || entityId === undefined || entityId === null) return false;
  const id = String(entityId);
  if (id === "0") return false;
  return !entityRecord(snapshot, "SHIP_IDENTITY", id) ||
    !entityRecord(snapshot, "ENTITY_LIFECYCLE", id);
}

function manifestForEntity(
  snapshot: DashboardSnapshot | null,
  entityId: unknown
): Record<string, unknown> | null {
  const identity = entityRecord(snapshot, "SHIP_IDENTITY", entityId);
  if (!snapshot || identity?.ship_class_id === undefined) return null;
  for (const entry of Object.values(snapshot.manifest.records)) {
    if (!entry || typeof entry !== "object") continue;
    const manifest = entry as { recordName?: string; fields?: Record<string, unknown> };
    if (
      manifest.recordName === "CLASS_MANIFEST" &&
      String(manifest.fields?.class_id) === String(identity.ship_class_id)
    ) return manifest.fields ?? null;
  }
  return null;
}

export function cargoSubsystemName(
  snapshot: DashboardSnapshot | null,
  entityId: unknown,
  subsystemId: unknown
): string {
  if (subsystemId === undefined || subsystemId === null) return "— vaisseau complet";
  const manifest = manifestForEntity(snapshot, entityId);
  const definitions = Array.isArray(manifest?.subsystem_definitions)
    ? manifest.subsystem_definitions as Array<Record<string, unknown>>
    : [];
  const definition = definitions.find(
    (item) => String(item.subsystem_id) === String(subsystemId)
  );
  const name = typeof definition?.internal_name === "string"
    ? definition.internal_name.trim()
    : "";
  return name || `SOUS-SYSTÈME ${String(subsystemId)}`;
}

function dockingRecords(snapshot: DashboardSnapshot | null) {
  return snapshot?.records.DOCKING_STATE ?? [];
}

function relationItems(record: Record<string, unknown>): Array<Record<string, unknown>> {
  return Array.isArray(record.relations)
    ? record.relations.filter(
        (item): item is Record<string, unknown> => Boolean(item) && typeof item === "object"
      )
    : [];
}

export function dockingRelations(
  snapshot: DashboardSnapshot | null
): DockingRelationView[] {
  const records = dockingRecords(snapshot);
  const keys = new Set<string>();
  for (const record of records) {
    const subject = String(record.entity_id ?? "");
    for (const relation of relationItems(record)) {
      keys.add(`${subject}>${String(relation.remote_entity_id ?? "")}`);
    }
  }
  const result: DockingRelationView[] = [];
  for (const record of records) {
    const subjectId = String(record.entity_id ?? "");
    for (const relation of relationItems(record)) {
      const remoteId = String(relation.remote_entity_id ?? "");
      result.push({
        subjectId,
        remoteId,
        subjectName: entityName(snapshot, subjectId),
        remoteName: entityName(snapshot, remoteId),
        localDockpoint: finite(relation.local_dockpoint),
        remoteDockpoint: finite(relation.remote_dockpoint),
        localBay: String(relation.local_dock_bay_name ?? "POINT INCONNU"),
        remoteBay: String(relation.remote_dock_bay_name ?? "POINT INCONNU"),
        reciprocal: keys.has(`${remoteId}>${subjectId}`),
        record,
        relation
      });
    }
  }
  return result;
}

export function playerDockingRelations(
  snapshot: DashboardSnapshot | null
): DockingRelationView[] {
  return dockingRelations(snapshot).filter(
    (relation) => relation.subjectId === String(snapshot?.playerEntityId ?? "")
  );
}

export function dockingNodes(snapshot: DashboardSnapshot | null): DockingNodeView[] {
  if (!snapshot?.playerEntityId) return [];
  const playerId = String(snapshot.playerEntityId);
  const supportId = String(supportState(snapshot)?.support_entity_id ?? "");
  const relations = dockingRelations(snapshot);
  const directIds = new Set(
    relations
      .filter((relation) => relation.subjectId === playerId)
      .map((relation) => relation.remoteId)
  );
  const adjacency = new Map<string, Set<string>>();
  for (const relation of relations) {
    if (!adjacency.has(relation.subjectId)) adjacency.set(relation.subjectId, new Set());
    if (!adjacency.has(relation.remoteId)) adjacency.set(relation.remoteId, new Set());
    adjacency.get(relation.subjectId)?.add(relation.remoteId);
    adjacency.get(relation.remoteId)?.add(relation.subjectId);
  }
  const visited = new Set<string>([playerId]);
  const queue = [playerId];
  while (queue.length) {
    const current = queue.shift()!;
    for (const remote of adjacency.get(current) ?? []) {
      if (!visited.has(remote)) {
        visited.add(remote);
        queue.push(remote);
      }
    }
  }
  if (supportId && supportId !== "0") visited.add(supportId);
  const leaderIds = new Set(
    dockingRecords(snapshot)
      .map((record) => String(record.group_leader_entity_id ?? "0"))
      .filter((id) => id !== "0")
  );
  return [...visited].map((entityId) => {
    const lifecycle = entityRecord(snapshot, "ENTITY_LIFECYCLE", entityId);
    return {
      entityId,
      name: entityName(snapshot, entityId),
      player: entityId === playerId,
      support: entityId === supportId,
      direct: directIds.has(entityId),
      leader: leaderIds.has(entityId),
      lifecycle: String(lifecycle?.lifecycle_phase ?? "—")
    };
  }).sort((left, right) =>
    Number(right.player) - Number(left.player) ||
    Number(right.support) - Number(left.support) ||
    Number(right.direct) - Number(left.direct) ||
    left.name.localeCompare(right.name, "fr")
  );
}

export function visibleDockingNodes(
  snapshot: DashboardSnapshot | null,
  limit = 8
): DockingNodeView[] {
  return dockingNodes(snapshot).slice(0, limit);
}

function derivedRatio(snapshot: DashboardSnapshot | null, key: string): number | null {
  const item = snapshot?.derived[key];
  return item?.available ? boundedRatio(item.value) : null;
}

export function serviceRows(snapshot: DashboardSnapshot | null): ServiceRow[] {
  const player = String(snapshot?.playerEntityId ?? "");
  if (!player) return [];
  const weapon = playerRecord(snapshot, "WEAPON_STATE");
  const subsystems = (snapshot?.records.SUBSYSTEM_STATE ?? []).filter(
    (record) => String(record.entity_id) === player
  );
  const subsystemRatios = subsystems
    .map((record) => derivedRatio(
      snapshot,
      `entities.${player}.subsystems.${String(record.subsystem_id)}.integrity_ratio`
    ))
    .filter((ratio): ratio is number => ratio !== null);
  const banks = (family: "primary_banks" | "secondary_banks") => {
    const records = Array.isArray(weapon?.[family])
      ? weapon[family] as Array<Record<string, unknown>>
      : [];
    return records
      .map((_, index) => derivedRatio(snapshot, `entities.${player}.${family}[${index}].ammo_ratio`))
      .filter((ratio): ratio is number => ratio !== null);
  };
  const primary = banks("primary_banks");
  const secondary = banks("secondary_banks");
  const row = (
    id: string,
    label: string,
    ratios: number[],
    absentReason: string
  ): ServiceRow => {
    const ratio = ratios.length ? Math.min(...ratios) : null;
    return {
      id,
      label,
      ratio,
      value: ratio === null ? "—" : `${Math.round(ratio * 100)} %`,
      reason: ratio === null ? absentReason : undefined
    };
  };
  return [
    row("hull", "COQUE", [derivedRatio(snapshot, `entities.${player}.hull_ratio`)].filter(
      (ratio): ratio is number => ratio !== null
    ), "coque indisponible"),
    row("shield", "BOUCLIERS", [derivedRatio(snapshot, `entities.${player}.shield_ratio`)].filter(
      (ratio): ratio is number => ratio !== null
    ), "aucun bouclier"),
    row("subsystems", "SOUS-SYSTÈME LE PLUS FAIBLE", subsystemRatios, "sans réserve de HP"),
    row("primary", "PRIMAIRE LA PLUS BASSE", primary, "banques énergétiques ou absentes"),
    row("secondary", "SECONDAIRE LA PLUS BASSE", secondary, "aucune secondaire à réarmer"),
    row("tertiary", "TERTIAIRE", [derivedRatio(snapshot, `entities.${player}.tertiary.ammo_ratio`)].filter(
      (ratio): ratio is number => ratio !== null
    ), "aucun tertiaire"),
    row("countermeasure", "CONTRE-MESURES", [
      derivedRatio(snapshot, `entities.${player}.countermeasure.quantity_ratio`)
    ].filter((ratio): ratio is number => ratio !== null), "aucune contre-mesure")
  ];
}

export function supportReferenceInvalid(snapshot: DashboardSnapshot | null): boolean {
  const supportId = supportState(snapshot)?.support_entity_id;
  if (entityReferenceIsInvalid(snapshot, supportId)) return true;
  return dockingRelations(snapshot).some(
    (relation) => entityReferenceIsInvalid(snapshot, relation.remoteId)
  );
}
