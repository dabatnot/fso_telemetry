import { playerRecord, playerRecords } from "./integritySemantics";
import type { DashboardSnapshot } from "./types";

export const RADAR_MODES: Record<number, string> = {
  0: "COURTE",
  1: "LONGUE",
  2: "INFINIE",
  3: "PERSONNALISÉE"
};

export const SENSOR_STATES: Record<number, string> = {
  0: "HORS LIGNE",
  1: "DÉGRADÉ",
  2: "EN LIGNE"
};

export const RADAR_VISIBILITY: Record<number, string> = {
  0: "PISTE MÉMORISÉE",
  1: "VISIBLE",
  2: "DISTORDUE"
};

export const RADAR_CATEGORIES: Record<number, string> = {
  0: "INCONNU",
  1: "VAISSEAU",
  2: "ARME",
  3: "NAVIGATION",
  4: "NŒUD DE SAUT",
  5: "ASTÉROÏDE",
  6: "DÉBRIS",
  7: "AUTRE"
};

export const THREAT_LEVELS: Record<number, string> = {
  0: "AUCUNE",
  1: "DUMBFIRE",
  2: "LOCK EN COURS",
  3: "LOCK ACQUIS"
};

export const HUD_WARNING_KINDS: Record<number, string> = {
  1: "LAUNCH",
  2: "EVADED",
  3: "COLLISION",
  4: "BLAST",
  5: "ENGINE WASH",
  6: "EMP",
  7: "OTHER"
};

export type HudAlertProvenance =
  | "authoritative-v1"
  | "legacy-aggregated"
  | "missing-authoritative";

export interface HudWarningView {
  kindCode: number;
  kind: string;
  text: string;
  remainingUs: number;
  instanceId: string;
}

export interface HudAlertView {
  primaryFireActive: boolean;
  primaryBlinkMs: 180 | null;
  missileLockState: 0 | 1 | 2;
  lockBlinkMs: 180 | 90 | null;
  warning: HudWarningView | null;
  provenance: HudAlertProvenance;
  record: Record<string, unknown> | null;
}

export const GUIDANCE_TYPES: Record<number, string> = {
  0: "AUCUN",
  1: "CHALEUR",
  2: "ASPECT",
  3: "HOMING",
  4: "SWARM",
  5: "SCRIPTÉ"
};

export const CONTACT_FLAGS = [
  { mask: 0x01, label: "BRILLANT" },
  { mask: 0x02, label: "CIBLE" },
  { mask: 0x04, label: "FURTIF" },
  { mask: 0x08, label: "TAGUÉ" },
  { mask: 0x10, label: "WARP" },
  { mask: 0x20, label: "BOMBE" },
  { mask: 0x40, label: "HOMING" },
  { mask: 0x80, label: "MENACE" }
] as const;

export const RADAR_BLIP_TYPES: Record<number, string> = {
  0: "NŒUD DE SAUT",
  1: "NAVBUOY/CARGO",
  2: "BOMBE",
  3: "WARP",
  4: "TAGUÉ",
  5: "VAISSEAU NORMAL"
};

/** Static scope presentation only; FSO blip coordinates are unchanged. */
export const RADAR_SCOPE_GRID = {
  axisRotationRad: Math.PI / 4,
  ringFractions: [0.5],
  axisInnerCutoutFraction: 0.5
} as const;

export type TacticalColorProvenance =
  | "authoritative-v4"
  | "legacy-v1-v3"
  | "missing-authoritative-color";

export interface TacticalColor {
  rgba: [number, number, number, number] | null;
  css: string;
  provenance: TacticalColorProvenance;
}

export interface ContactView {
  id: string;
  record: Record<string, unknown>;
  name: string;
  typeLabel: string | null;
  category: string;
  visibility: string;
  visibilityCode: number;
  blipType: string | null;
  blipTypeCode: number | null;
  bright: boolean;
  /** Latest cockpit selection from TARGET_STATE, independent of radar cadence. */
  current: boolean;
  color: TacticalColor;
  flags: string[];
  flagBits: number;
  distance: number | null;
  relativeSpeed: number | null;
  closingSpeed: number | null;
  ttcS: number | null;
  ageUs: number | null;
  bearingRad: number | null;
  elevationRad: number | null;
  scope: [number, number] | null;
  inRange: boolean | null;
  invalid: boolean;
}

export interface LockView {
  index: number;
  record: Record<string, unknown>;
  targetId: string;
  targetName: string;
  locked: boolean;
  inCone: boolean;
  attempt: boolean;
  remainingS: number | null;
  progress: number | null;
  invalid: boolean;
}

export interface MissileView {
  id: string;
  record: Record<string, unknown>;
  name: string;
  guidance: string;
  visibility: string;
  distance: number | null;
  relativeSpeed: number | null;
  closingSpeed: number | null;
  ttcS: number | null;
  invalid: boolean;
}

function finite(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function integer(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isInteger(numeric) && numeric >= 0 ? numeric : null;
}

const LEGACY_NEUTRAL_COLOR = "#70e4d1";
const LEGACY_TARGET_COLOR = "#ffd466";
const LEGACY_THREAT_COLOR = "#ff6b55";
const RADAR_COLOR_RECORD_VERSIONS = [4] as const;
const TARGET_COLOR_RECORD_VERSIONS = [4, 5] as const;

function byte(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isInteger(numeric) && numeric >= 0 && numeric <= 0xff
    ? numeric
    : null;
}

function rgba8(value: unknown): [number, number, number, number] | null {
  if (!Array.isArray(value) || value.length !== 4) return null;
  const components = value.map(byte);
  return components.some((component) => component === null)
    ? null
    : components as [number, number, number, number];
}

function rgba8Css(value: [number, number, number, number]): string {
  return `#${value.map((component) => component.toString(16).padStart(2, "0")).join("")}`;
}

function legacyContactColor(flagBits: number): TacticalColor {
  const css = (flagBits & 0xa0) !== 0
    ? LEGACY_THREAT_COLOR
    : (flagBits & 0x02) !== 0
      ? LEGACY_TARGET_COLOR
      : LEGACY_NEUTRAL_COLOR;
  return { rgba: null, css, provenance: "legacy-v1-v3" };
}

function contactColor(
  value: unknown,
  version: number | null,
  flagBits: number
): TacticalColor {
  const authoritative = authoritativeColor(value, version, RADAR_COLOR_RECORD_VERSIONS);
  if (authoritative !== null) return authoritative;
  if (version !== null && version > 3) {
    return {
      rgba: null,
      css: LEGACY_NEUTRAL_COLOR,
      provenance: "missing-authoritative-color"
    };
  }
  return legacyContactColor(flagBits);
}

function authoritativeColor(
  value: unknown,
  version: number | null,
  supportedVersions: readonly number[]
): TacticalColor | null {
  if (version === null || !supportedVersions.includes(version)) return null;
  const rgba = rgba8(value);
  return rgba === null
    ? { rgba: null, css: LEGACY_NEUTRAL_COLOR, provenance: "missing-authoritative-color" }
    : { rgba, css: rgba8Css(rgba), provenance: "authoritative-v4" };
}

function derivedNumber(
  snapshot: DashboardSnapshot | null,
  path: string
): number | null {
  const item = snapshot?.derived[path];
  return item?.available ? finite(item.value) : null;
}

function derivedBoolean(
  snapshot: DashboardSnapshot | null,
  path: string
): boolean | null {
  const item = snapshot?.derived[path];
  return item?.available && typeof item.value === "boolean" ? item.value : null;
}

function derivedPair(
  snapshot: DashboardSnapshot | null,
  path: string
): [number, number] | null {
  const item = snapshot?.derived[path];
  if (!item?.available || !Array.isArray(item.value) || item.value.length !== 2) return null;
  const x = finite(item.value[0]);
  const y = finite(item.value[1]);
  return x === null || y === null ? null : [x, y];
}

function closedLabel(value: unknown, labels: Record<number, string>): string | null {
  const numeric = integer(value);
  return numeric === null ? null : labels[numeric] ?? null;
}

export function decodeContactFlags(value: unknown): string[] | null {
  const numeric = integer(value);
  if (numeric === null || (numeric & ~0xff) !== 0) return null;
  return CONTACT_FLAGS
    .filter((definition) => (numeric & definition.mask) !== 0)
    .map((definition) => definition.label);
}

export function contactVisibilityAlpha(visibilityCode: number): number {
  if (visibilityCode === 0) return 0.35;
  if (visibilityCode === 2) return 0.6;
  return 1;
}

export function targetState(snapshot: DashboardSnapshot | null) {
  return playerRecord(snapshot, "TARGET_STATE");
}

export function radarState(snapshot: DashboardSnapshot | null) {
  return playerRecord(snapshot, "RADAR_STATE");
}

export function radarRangeDisplay(snapshot: DashboardSnapshot | null) {
  const radar = radarState(snapshot);
  if (integer(radar?.radar_mode) === 2) {
    return {
      value: null,
      text: "∞",
      detail: "portée illimitée"
    } as const;
  }
  return {
    value: finite(radar?.selected_range),
    text: null,
    detail: "unités monde"
  } as const;
}

export function lockState(snapshot: DashboardSnapshot | null) {
  return playerRecord(snapshot, "LOCK_STATE");
}

export function threatState(snapshot: DashboardSnapshot | null) {
  return playerRecord(snapshot, "THREAT_STATE");
}

export function hudAlertState(snapshot: DashboardSnapshot | null) {
  return playerRecord(snapshot, "HUD_ALERT_STATE");
}

export function hudAlertView(snapshot: DashboardSnapshot | null): HudAlertView {
  const record = hudAlertState(snapshot);
  if (record !== null) {
    const lock = integer(record.missile_lock_state);
    const primary = record.primary_fire_threat_active === true;
    const presence = integer(record.presence) ?? 0;
    let warning: HudWarningView | null = null;
    if ((presence & 0x01) !== 0) {
      const kindCode = integer(record.warning_kind);
      const text = typeof record.warning_text === "string"
        ? record.warning_text.trim()
        : "";
      const remainingUs = integer(record.warning_remaining_us);
      const instanceId = String(record.warning_instance_id ?? "0");
      if (kindCode !== null && HUD_WARNING_KINDS[kindCode] && text &&
          remainingUs !== null && remainingUs > 0 && instanceId !== "0") {
        warning = {
          kindCode,
          kind: HUD_WARNING_KINDS[kindCode],
          text,
          remainingUs,
          instanceId
        };
      }
    }
    const lockState = lock === 1 || lock === 2 ? lock : 0;
    return {
      primaryFireActive: primary,
      primaryBlinkMs: primary ? 180 : null,
      missileLockState: lockState,
      lockBlinkMs: lockState === 1 ? 180 : lockState === 2 ? 90 : null,
      warning,
      provenance: "authoritative-v1",
      record
    };
  }

  if (snapshot?.mode === "replay" &&
      snapshot.replay.captureSchema === "FSTL-dashboard-capture-v1") {
    const level = integer(threatState(snapshot)?.threat_level) ?? 0;
    const lockState = level >= 3 ? 2 : level >= 2 ? 1 : 0;
    return {
      primaryFireActive: level === 1,
      primaryBlinkMs: level === 1 ? 180 : null,
      missileLockState: lockState,
      lockBlinkMs: lockState === 1 ? 180 : lockState === 2 ? 90 : null,
      warning: null,
      provenance: "legacy-aggregated",
      record: null
    };
  }

  return {
    primaryFireActive: false,
    primaryBlinkMs: null,
    missileLockState: 0,
    lockBlinkMs: null,
    warning: null,
    provenance: "missing-authoritative",
    record: null
  };
}

export function radarContacts(snapshot: DashboardSnapshot | null) {
  return playerRecords(snapshot, "RADAR_CONTACTS");
}

export function manifestRecord(
  snapshot: DashboardSnapshot | null,
  recordName: string,
  idField: string,
  id: unknown
): Record<string, unknown> | null {
  if (!snapshot || id === undefined || id === null) return null;
  for (const entry of Object.values(snapshot.manifest.records)) {
    if (!entry || typeof entry !== "object") continue;
    const manifest = entry as { recordName?: string; fields?: Record<string, unknown> };
    if (
      manifest.recordName === recordName &&
      String(manifest.fields?.[idField]) === String(id)
    ) return manifest.fields ?? null;
  }
  return null;
}

function revealedContactName(record: Record<string, unknown>): string {
  const revealed = typeof record.revealed_name === "string"
    ? record.revealed_name.trim()
    : "";
  return revealed || `CONTACT ${String(record.contact_entity_id)}`;
}

function radarContactRecordVersion(
  snapshot: DashboardSnapshot | null,
  contactId: string
): number | null {
  const player = String(snapshot?.playerEntityId ?? "");
  for (const envelope of Object.values(snapshot?.recordInstances ?? {})) {
    if (envelope.recordName === "RADAR_CONTACTS" &&
        String(envelope.fields.entity_id ?? "") === player &&
        String(envelope.fields.contact_entity_id ?? "") === contactId) {
      return typeof envelope.recordVersion === "number" &&
        Number.isInteger(envelope.recordVersion)
        ? envelope.recordVersion
        : null;
    }
  }
  return null;
}

function contactTypeLabel(
  snapshot: DashboardSnapshot | null,
  record: Record<string, unknown>,
  contactId: string
): string | null {
  const recordVersion = radarContactRecordVersion(snapshot, contactId);
  const authoritative = typeof record.hud_type_label === "string"
    ? record.hud_type_label.trim()
    : "";
  if (recordVersion !== null && recordVersion >= 3) return authoritative || null;
  const definition = manifestRecord(
    snapshot, "CLASS_MANIFEST", "class_id", record.revealed_class_id
  );
  const legacy = definition?.internal_name;
  return typeof legacy === "string" && legacy.trim() ? legacy.trim() : null;
}

export function contactViews(snapshot: DashboardSnapshot | null): ContactView[] {
  const player = String(snapshot?.playerEntityId ?? "");
  const currentTargetId = String(
    targetState(snapshot)?.current_target_entity_id ?? "0"
  );
  const locks = lockState(snapshot)?.locks;
  const lockTargets = new Set(
    (Array.isArray(locks) ? locks : []).map((lock) =>
      String((lock as Record<string, unknown>).target_entity_id)
    )
  );
  return radarContacts(snapshot).map((record) => {
    const id = String(record.contact_entity_id ?? "");
    const prefix = `entities.${player}.tracks.${id}`;
    const categoryCode = integer(record.category);
    const visibilityCode = integer(record.visibility);
    const flags = decodeContactFlags(record.contact_flags);
    const flagBits = integer(record.contact_flags);
    const recordVersion = radarContactRecordVersion(snapshot, id);
    const blipTypeCode = integer(record.radar_blip_type);
    const color = contactColor(record.radar_blip_color, recordVersion, flagBits ?? 0);
    const scope = derivedPair(snapshot, `${prefix}.scope_clamped_position`);
    const invalid =
      !id ||
      categoryCode === null ||
      !RADAR_CATEGORIES[categoryCode] ||
      visibilityCode === null ||
      !RADAR_VISIBILITY[visibilityCode] ||
      flags === null ||
      flagBits === null ||
      scope === null;
    const view: ContactView = {
      id,
      record,
      name: revealedContactName(record),
      typeLabel: contactTypeLabel(snapshot, record, id),
      category: categoryCode === null ? "ERR" : RADAR_CATEGORIES[categoryCode] ?? "ERR",
      visibility: visibilityCode === null ? "ERR" : RADAR_VISIBILITY[visibilityCode] ?? "ERR",
      visibilityCode: visibilityCode ?? -1,
      blipType: blipTypeCode === null ? null : RADAR_BLIP_TYPES[blipTypeCode] ?? null,
      blipTypeCode,
      bright: ((flagBits ?? 0) & 0x01) !== 0,
      current: id !== "" && id !== "0" && id === currentTargetId,
      color,
      flags: flags ?? [],
      flagBits: flagBits ?? 0,
      distance: derivedNumber(snapshot, `${prefix}.distance`),
      relativeSpeed: derivedNumber(snapshot, `${prefix}.relative_speed`),
      closingSpeed: derivedNumber(snapshot, `${prefix}.closing_speed`),
      ttcS: derivedNumber(snapshot, `${prefix}.ttc_s`),
      ageUs: derivedNumber(snapshot, `${prefix}.age_us`),
      bearingRad: derivedNumber(snapshot, `${prefix}.bearing_local_rad`),
      elevationRad: derivedNumber(snapshot, `${prefix}.elevation_local_rad`),
      scope,
      inRange: derivedBoolean(snapshot, `${prefix}.scope_in_range`),
      invalid
    };
    const priority =
      (view.current ? 1_000_000 : 0) +
      ((view.flagBits & 0xe0) ? 500_000 : 0) +
      (lockTargets.has(id) ? 250_000 : 0) +
      (view.visibilityCode === 1 ? 100_000 : view.visibilityCode === 2 ? 50_000 : 0) -
      (view.distance ?? Number.MAX_SAFE_INTEGER) / 1000;
    Object.defineProperty(view, "_priority", { value: priority, enumerable: false });
    return view;
  });
}

export function prioritizedContacts(snapshot: DashboardSnapshot | null): ContactView[] {
  return contactViews(snapshot).sort((left, right) => {
    const leftPriority = (left as ContactView & { _priority?: number })._priority ?? 0;
    const rightPriority = (right as ContactView & { _priority?: number })._priority ?? 0;
    return rightPriority - leftPriority || left.id.localeCompare(right.id);
  });
}

export function targetContact(
  snapshot: DashboardSnapshot | null
): ContactView | null {
  const target = targetState(snapshot);
  const targetId = String(target?.current_target_entity_id ?? "0");
  return targetId === "0"
    ? null
    : contactViews(snapshot).find((contact) => contact.id === targetId) ?? null;
}

export function targetDisplayName(snapshot: DashboardSnapshot | null): string {
  const target = targetState(snapshot);
  const targetId = String(target?.current_target_entity_id ?? "0");
  if (targetId === "0") return "— AUCUNE CIBLE";
  const identity = target?.revealed_identity;
  const revealed = identity && typeof identity === "object"
    ? String((identity as Record<string, unknown>).name ?? "").trim()
    : "";
  return revealed || targetContact(snapshot)?.name || `CONTACT ${targetId}`;
}

export function targetClassDisplayName(snapshot: DashboardSnapshot | null): string | null {
  const identity = targetState(snapshot)?.revealed_identity;
  const classId = identity && typeof identity === "object"
    ? (identity as Record<string, unknown>).class_id
    : undefined;
  const definition = manifestRecord(snapshot, "CLASS_MANIFEST", "class_id", classId);
  const name = definition?.internal_name;
  return typeof name === "string" && name.trim() ? name.trim() : null;
}

/** The exact second Target Box line captured by TARGET_STATE v3 and later. */
export function targetHudTypeLabel(snapshot: DashboardSnapshot | null): string | null {
  const label = targetState(snapshot)?.hud_type_label;
  return typeof label === "string" && label.trim() ? label.trim() : null;
}

export function targetRecordVersion(snapshot: DashboardSnapshot | null): number | null {
  const target = targetState(snapshot);
  if (!target) return null;
  const player = String(snapshot?.playerEntityId ?? "");
  for (const envelope of Object.values(snapshot?.recordInstances ?? {})) {
    if (envelope.recordName === "TARGET_STATE" &&
        String(envelope.fields.entity_id ?? "") === player) {
      const version = envelope.recordVersion;
      return typeof version === "number" && Number.isInteger(version) ? version : null;
    }
  }
  return null;
}

export function targetHudColor(snapshot: DashboardSnapshot | null): TacticalColor {
  const version = targetRecordVersion(snapshot);
  const color = authoritativeColor(
    targetState(snapshot)?.hud_target_color,
    version,
    TARGET_COLOR_RECORD_VERSIONS
  );
  if (color !== null) return color;
  if (version !== null && version >= 4) {
    return {
      rgba: null,
      css: LEGACY_NEUTRAL_COLOR,
      provenance: "missing-authoritative-color"
    };
  }
  return { rgba: null, css: LEGACY_TARGET_COLOR, provenance: "legacy-v1-v3" };
}

export function targetReferenceInvalid(snapshot: DashboardSnapshot | null): boolean {
  // TARGET_STATE v3 and later are independent projections of the FSO Target Box. A
  // selected object can intentionally have no RADAR_CONTACTS entry.
  return false;
}

export function lockViews(snapshot: DashboardSnapshot | null): LockView[] {
  const state = lockState(snapshot);
  const locks = Array.isArray(state?.locks)
    ? state.locks as Array<Record<string, unknown>>
    : [];
  const currentTarget = String(targetState(snapshot)?.current_target_entity_id ?? "0");
  const contacts = new Map(contactViews(snapshot).map((contact) => [contact.id, contact]));
  const player = String(snapshot?.playerEntityId ?? "");
  return locks.map((record, index) => {
    const targetId = String(record.target_entity_id ?? "");
    const remainingUs = finite(record.time_to_lock_remaining_us);
    const progress = derivedNumber(snapshot, `entities.${player}.locks[${index}].progress`);
    const locked = typeof record.locked === "boolean" ? record.locked : false;
    const inCone = typeof record.target_in_lock_cone === "boolean"
      ? record.target_in_lock_cone
      : false;
    const invalid =
      !targetId ||
      typeof record.locked !== "boolean" ||
      typeof record.target_in_lock_cone !== "boolean" ||
      (snapshot?.transport.synchronized === true &&
        !contacts.has(targetId) &&
        targetId !== currentTarget);
    return {
      index,
      record,
      targetId,
      targetName: contacts.get(targetId)?.name ?? `CONTACT ${targetId}`,
      locked,
      inCone,
      attempt: remainingUs !== null,
      remainingS: remainingUs === null ? null : Math.max(remainingUs, 0) / 1_000_000,
      progress,
      invalid
    };
  }).sort((left, right) =>
    Number(right.locked) - Number(left.locked) ||
    Number(right.attempt) - Number(left.attempt) ||
    Number(right.targetId === currentTarget) - Number(left.targetId === currentTarget) ||
    (left.remainingS ?? Number.MAX_SAFE_INTEGER) -
      (right.remainingS ?? Number.MAX_SAFE_INTEGER) ||
    left.targetId.localeCompare(right.targetId)
  );
}

function weaponName(snapshot: DashboardSnapshot | null, weaponClassId: unknown): string {
  const manifest = manifestRecord(
    snapshot,
    "WEAPON_MANIFEST",
    "weapon_class_id",
    weaponClassId
  );
  if (!manifest) return `ARME ${String(weaponClassId)}`;
  const title = typeof manifest.title === "string" ? manifest.title.trim() : "";
  const internal = typeof manifest.internal_name === "string"
    ? manifest.internal_name.trim()
    : "";
  return title || internal || `ARME ${String(weaponClassId)}`;
}

export function missileViews(snapshot: DashboardSnapshot | null): MissileView[] {
  const state = threatState(snapshot);
  const missiles = Array.isArray(state?.incoming_missiles)
    ? state.incoming_missiles as Array<Record<string, unknown>>
    : [];
  const player = String(snapshot?.playerEntityId ?? "");
  return missiles.map((record) => {
    const id = String(record.entity_id ?? "");
    const prefix = `entities.${player}.missiles.${id}`;
    const guidance = closedLabel(record.guidance_type, GUIDANCE_TYPES);
    const visibility = closedLabel(record.radar_visibility, RADAR_VISIBILITY);
    const manifest = manifestRecord(
      snapshot,
      "WEAPON_MANIFEST",
      "weapon_class_id",
      record.weapon_class_id
    );
    const invalid =
      !id ||
      guidance === null ||
      visibility === null ||
      (snapshot?.transport.synchronized === true && manifest === null);
    return {
      id,
      record,
      name: weaponName(snapshot, record.weapon_class_id),
      guidance: guidance ?? "ERR",
      visibility: visibility ?? "ERR",
      distance: derivedNumber(snapshot, `${prefix}.distance`),
      relativeSpeed: derivedNumber(snapshot, `${prefix}.relative_speed`),
      closingSpeed: derivedNumber(snapshot, `${prefix}.closing_speed`),
      ttcS: derivedNumber(snapshot, `${prefix}.ttc_s`),
      invalid
    };
  }).sort((left, right) =>
    (left.ttcS ?? Number.MAX_SAFE_INTEGER) - (right.ttcS ?? Number.MAX_SAFE_INTEGER) ||
    (right.closingSpeed ?? Number.MIN_SAFE_INTEGER) -
      (left.closingSpeed ?? Number.MIN_SAFE_INTEGER) ||
    left.id.localeCompare(right.id)
  );
}

export function sensorLabels(snapshot: DashboardSnapshot | null) {
  const radar = radarState(snapshot);
  return {
    mode: closedLabel(radar?.radar_mode, RADAR_MODES),
    state: closedLabel(radar?.sensor_state, SENSOR_STATES),
    threat: closedLabel(threatState(snapshot)?.threat_level, THREAT_LEVELS)
  };
}

export function subsystemNameForTarget(
  snapshot: DashboardSnapshot | null,
  subsystemId: unknown,
  authoritativeLabel?: unknown
): string | null {
  const version = targetRecordVersion(snapshot);
  if (version !== null && version >= 5) {
    return typeof authoritativeLabel === "string" && authoritativeLabel.trim()
      ? authoritativeLabel.trim()
      : null;
  }
  const target = targetState(snapshot);
  const identity = target?.revealed_identity;
  const classId = identity && typeof identity === "object"
    ? (identity as Record<string, unknown>).class_id
    : undefined;
  if (!subsystemId || !classId) return null;
  const manifest = manifestRecord(snapshot, "CLASS_MANIFEST", "class_id", classId);
  const definitions = Array.isArray(manifest?.subsystems)
    ? manifest.subsystems as Array<Record<string, unknown>>
    : [];
  const definition = definitions.find(
    (candidate) => String(candidate.subsystem_id) === String(subsystemId)
  );
  if (!definition) return `SOUS-SYSTÈME ${String(subsystemId)}`;
  return String(
    definition.hud_name ??
    definition.alternate_name ??
    definition.internal_name ??
    `SOUS-SYSTÈME ${String(subsystemId)}`
  );
}
