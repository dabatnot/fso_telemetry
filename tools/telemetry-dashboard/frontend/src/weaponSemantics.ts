import { playerRecord, subsystemViews } from "./integritySemantics";
import { playerClassManifest } from "./energySemantics";
import type { DashboardSnapshot } from "./types";

export type WeaponFamily = "primary" | "secondary";

export interface WeaponBankView {
  family: WeaponFamily;
  index: number;
  bankId: string;
  selected: boolean;
  record: Record<string, unknown>;
  definition: Record<string, unknown> | null;
  weaponClass: Record<string, unknown> | null;
  name: string;
  subtype: string;
  flags: string[];
  state: string;
  ammoCurrent: number | null;
  ammoInitial: number | null;
  ammoRatio: number | null;
  cooldownS: number | null;
  rearmS: number | null;
  nominalRateHz: number | null;
}

export interface TurretView {
  subsystemId: string;
  name: string;
  locked: boolean;
  cooldownUs: number;
  targetActive: boolean;
  record: Record<string, unknown>;
}

export const WEAPON_GLOBAL_FLAGS = [
  { mask: 0x0001, label: "PRIMAIRES LIÉES" },
  { mask: 0x0002, label: "SECONDAIRE DOUBLE" },
  { mask: 0x0004, label: "GÂCHETTE PRIMAIRE" },
  { mask: 0x0008, label: "GÂCHETTE SECONDAIRE" },
  { mask: 0x0010, label: "PRIMAIRES VERROUILLÉES" },
  { mask: 0x0020, label: "SECONDAIRES VERROUILLÉES" },
  { mask: 0x0040, label: "LASER DE CIBLAGE" },
  { mask: 0x0080, label: "DÉTONATEURS DISTANTS" },
  { mask: 0x0100, label: "BEAM LIBRE" },
  { mask: 0x0200, label: "BEAM VERROUILLÉ" }
] as const;

export const WEAPON_CLASS_FLAGS = [
  { mask: 0x0001, label: "BOMBE" },
  { mask: 0x0002, label: "BALISTIQUE" },
  { mask: 0x0004, label: "SANS MUNITIONS" },
  { mask: 0x0008, label: "BEAM" },
  { mask: 0x0010, label: "SWARM" },
  { mask: 0x0020, label: "CONTRE-MESURE" },
  { mask: 0x0040, label: "GUIDÉE" },
  { mask: 0x0080, label: "TÉLÉ-DÉTONABLE" }
] as const;

export const WEAPON_SUBTYPES: Record<number, string> = {
  0: "INCONNU",
  1: "PRIMAIRE",
  2: "MISSILE",
  3: "BEAM",
  4: "CONTRE-MESURE",
  5: "SPÉCIAL"
};

export const GUIDANCE_TYPES: Record<number, string> = {
  0: "AUCUN",
  1: "CHALEUR",
  2: "ASPECT",
  3: "HOMING",
  4: "SWARM",
  5: "SCRIPTÉ"
};

export const FIRING_PATTERNS: Record<number, string> = {
  0: "STANDARD",
  1: "CYCLE AVANT",
  2: "CYCLE ARRIÈRE",
  3: "ALÉATOIRE EXHAUSTIF",
  4: "ALÉATOIRE SANS RÉPÉTITION",
  5: "ALÉATOIRE RÉPÉTITIF"
};

function finite(value: unknown): number | null {
  const result = Number(value);
  return Number.isFinite(result) ? result : null;
}

export function decodeBitmap(
  value: unknown,
  definitions: ReadonlyArray<{ mask: number; label: string }>
): string[] | null {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 0) return null;
  return definitions
    .filter((definition) => (numeric & definition.mask) !== 0)
    .map((definition) => definition.label);
}

export function weaponState(snapshot: DashboardSnapshot | null): Record<string, unknown> | null {
  return playerRecord(snapshot, "WEAPON_STATE");
}

export function weaponManifest(
  snapshot: DashboardSnapshot | null,
  weaponClassId: unknown
): Record<string, unknown> | null {
  if (!snapshot || weaponClassId === undefined || weaponClassId === null) return null;
  for (const entry of Object.values(snapshot.manifest.records)) {
    if (!entry || typeof entry !== "object") continue;
    const record = entry as { recordName?: string; fields?: Record<string, unknown> };
    if (
      record.recordName === "WEAPON_MANIFEST" &&
      String(record.fields?.weapon_class_id) === String(weaponClassId)
    ) {
      return record.fields ?? null;
    }
  }
  return null;
}

export function weaponDisplayName(manifest: Record<string, unknown> | null): string {
  if (!manifest) return "ERR · CLASSE INTROUVABLE";
  const title = typeof manifest.title === "string" ? manifest.title.trim() : "";
  if (title) return title;
  const internal = typeof manifest.internal_name === "string"
    ? manifest.internal_name.trim()
    : "";
  return internal || "ERR · CLASSE SANS NOM";
}

export function bankState(
  family: WeaponFamily,
  weaponFlags: unknown,
  bank: Record<string, unknown>
): string {
  const flags = Number(weaponFlags);
  const cooldown = finite(bank.cooldown_remaining_us);
  if (!Number.isInteger(flags) || flags < 0 || cooldown === null) return "ERR";
  const lockedMask = family === "primary" ? 0x0010 : 0x0020;
  if ((flags & lockedMask) !== 0) return "VERROUILLÉE";
  const ammunition = bank.ammunition as Record<string, unknown> | undefined;
  const current = ammunition ? finite(ammunition.current) : null;
  if (current !== null && current <= 0) return "VIDE";
  if (cooldown > 0) return "RECHARGE";
  return "DISPONIBLE";
}

function derivedNumber(snapshot: DashboardSnapshot | null, key: string): number | null {
  const item = snapshot?.derived[key];
  return item?.available ? finite(item.value) : null;
}

export function bankViews(
  snapshot: DashboardSnapshot | null,
  family: WeaponFamily
): WeaponBankView[] {
  const state = weaponState(snapshot);
  if (!state || !snapshot?.playerEntityId) return [];
  const records = Array.isArray(state[`${family}_banks`])
    ? state[`${family}_banks`] as Array<Record<string, unknown>>
    : [];
  const selectedId = state[`current_${family}_bank_id`];
  const classRecord = playerClassManifest(snapshot);
  const definitions = Array.isArray(classRecord?.banks)
    ? classRecord.banks as Array<Record<string, unknown>>
    : [];
  return records.map((record, index) => {
    const bankId = String(record.bank_id);
    const manifest = weaponManifest(snapshot, record.weapon_class_id);
    const ammunition = record.ammunition as Record<string, unknown> | undefined;
    const prefix = `entities.${snapshot.playerEntityId}.${family}_banks[${index}]`;
    const stateValue = snapshot.derived[`${prefix}.state`];
    const nominal = manifest?.weapon_class_id === undefined
      ? null
      : derivedNumber(
          snapshot,
          `weapon_classes.${manifest.weapon_class_id}.nominal_rate_hz`
        );
    return {
      family,
      index,
      bankId,
      selected: String(selectedId) === bankId,
      record,
      definition: definitions.find((item) => String(item.bank_id) === bankId) ?? null,
      weaponClass: manifest,
      name: weaponDisplayName(manifest),
      subtype: WEAPON_SUBTYPES[Number(manifest?.subtype)] ?? "INCONNU",
      flags: decodeBitmap(manifest?.weapon_flags, WEAPON_CLASS_FLAGS) ?? [],
      state: stateValue?.available
        ? String(stateValue.value)
        : bankState(family, state.weapon_flags, record),
      ammoCurrent: ammunition ? finite(ammunition.current) : null,
      ammoInitial: ammunition ? finite(ammunition.initial) : null,
      ammoRatio: derivedNumber(snapshot, `${prefix}.ammo_ratio`),
      cooldownS: derivedNumber(snapshot, `${prefix}.cooldown_s`),
      rearmS: derivedNumber(snapshot, `${prefix}.rearm_s`),
      nominalRateHz: nominal
    };
  });
}

export function selectedBank(
  snapshot: DashboardSnapshot | null,
  family: WeaponFamily
): WeaponBankView | null {
  return bankViews(snapshot, family).find((bank) => bank.selected) ?? null;
}

export function visibleBanks(banks: WeaponBankView[], limit = 6): WeaponBankView[] {
  if (banks.length <= limit) return banks;
  const selected = banks.find((bank) => bank.selected);
  const visible = banks.slice(0, limit);
  if (selected && !visible.includes(selected)) visible[limit - 1] = selected;
  return visible.sort((left, right) => left.index - right.index);
}

export function countermeasureState(value: unknown): string {
  if (!value || typeof value !== "object") return "—";
  const countermeasure = value as Record<string, unknown>;
  const flags = Number(countermeasure.flags);
  const current = finite(countermeasure.current);
  const cooldown = finite(countermeasure.cooldown_remaining_us);
  if (!Number.isInteger(flags) || flags < 0 || current === null || cooldown === null) return "ERR";
  if ((flags & 0x0002) !== 0) return "VERROUILLÉE";
  if (current <= 0) return "VIDE";
  if (cooldown > 0) return "RECHARGE";
  return "DISPONIBLE";
}

export function turretViews(snapshot: DashboardSnapshot | null): TurretView[] {
  return subsystemViews(snapshot)
    .filter((system) => Number(system.record.type) === 2 && system.record.turret)
    .map((system) => {
      const turret = system.record.turret as Record<string, unknown>;
      const cooldown = finite(turret.cooldown_remaining_us) ?? 0;
      const target = String(turret.target_entity_id ?? "0");
      return {
        subsystemId: system.subsystemId,
        name: system.name,
        locked: system.flags.includes("MOUVEMENT VERROUILLÉ") ||
          system.flags.includes("BEAM VERROUILLÉ"),
        cooldownUs: cooldown,
        targetActive: target !== "0",
        record: system.record
      };
    })
    .sort((left, right) =>
      Number(right.locked) - Number(left.locked) ||
      Number(right.cooldownUs > 0) - Number(left.cooldownUs > 0) ||
      Number(right.targetActive) - Number(left.targetActive) ||
      Number(left.record.canonical_index) - Number(right.record.canonical_index)
    );
}

export function selectedBankIsInvalid(
  snapshot: DashboardSnapshot | null,
  family: WeaponFamily
): boolean {
  const state = weaponState(snapshot);
  if (!state) return false;
  const count = Number(state[`${family}_bank_count`] ?? 0);
  if (count === 0) return false;
  return selectedBank(snapshot, family) === null;
}
