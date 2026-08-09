export interface FlagDefinition {
  mask: number;
  label: string;
}

export const PHYSICS_MODE_FLAGS: FlagDefinition[] = [
  { mask: 0x0001, label: "AFTERBURNER" },
  { mask: 0x0002, label: "BOOSTER" },
  { mask: 0x0004, label: "GLIDE" },
  { mask: 0x0008, label: "GLIDE FORCÉ" },
  { mask: 0x0010, label: "AMORT. NEWTONIEN" },
  { mask: 0x0020, label: "TRANSLATION" },
  { mask: 0x0040, label: "WARP ENTRANT" },
  { mask: 0x0080, label: "WARP SORTANT" },
  { mask: 0x0100, label: "SCRIPTÉ" },
  { mask: 0x0200, label: "ONDE DE CHOC" },
  { mask: 0x0400, label: "IMMOBILE" },
  { mask: 0x0800, label: "ORIENTATION VERROUILLÉE" }
];

export const CONTROL_FLAGS: FlagDefinition[] = [
  { mask: 0x01, label: "MATCH SPEED" },
  { mask: 0x02, label: "AUTO TARGET" },
  { mask: 0x04, label: "AUTO MATCH" },
  { mask: 0x08, label: "PRIMAIRE LIÉ" },
  { mask: 0x10, label: "SECONDAIRE DOUBLE" },
  { mask: 0x20, label: "AFTERBURNER DEMANDÉ" }
];

export const CONTROL_MODES: Record<number, string> = {
  0: "INCONNU",
  1: "VAISSEAU",
  2: "VUE",
  3: "FLIGHT CURSOR",
  4: "AUTOPILOTE"
};

export function decodeFlags(value: unknown, definitions: FlagDefinition[]): string[] | null {
  const numeric = Number(value);
  if (!Number.isInteger(numeric) || numeric < 0) return null;
  return definitions.filter((definition) => (numeric & definition.mask) !== 0).map((definition) => definition.label);
}

export function controlModeLabel(value: unknown): string {
  const numeric = Number(value);
  return Number.isInteger(numeric) && CONTROL_MODES[numeric] ? CONTROL_MODES[numeric] : "INCONNU";
}
