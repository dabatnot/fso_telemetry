import type { DashboardSnapshot, InstrumentDefinition, InstrumentValue } from "./types";
import { attitudeFromQuaternion } from "./flightMath";
import { playerClassManifest } from "./energySemantics";

export function dashboardSourcePresentation(
  snapshot: DashboardSnapshot | null,
  socketOnline: boolean
): { label: string; statusClass: string } {
  if (!socketOnline) return { label: "BRIDGE HORS LIGNE", statusClass: "disconnected" };
  if (snapshot?.mode === "replay") {
    return {
      label: snapshot.replay.playing ? "REPLAY · LECTURE" : "REPLAY · PAUSE",
      statusClass: "replay"
    };
  }
  const status = snapshot?.connection.status ?? "Synchronizing";
	if (status === "Live" && Boolean(snapshot?.mission.paused)) {
		return { label: "PAUSE", statusClass: "paused" };
	}
	if (status === "Ready") {
		return { label: "PRÊT", statusClass: "ready" };
	}
  return { label: status, statusClass: status.toLowerCase() };
}

function pathValue(value: unknown, path: string): unknown {
  return path.split(".").filter(Boolean).reduce<unknown>((current, part) => {
    if (current === null || current === undefined || typeof current !== "object") return undefined;
    return (current as Record<string, unknown>)[part];
  }, value);
}

function qualityFor(snapshot: DashboardSnapshot, recordName?: string) {
  if (!recordName) return undefined;
  return snapshot.quality.channels.find(
    (channel) =>
      channel.recordName === recordName &&
      (!snapshot.playerEntityId || channel.identity.includes(`entity_id=${snapshot.playerEntityId}`))
  );
}

function recordValue(snapshot: DashboardSnapshot, name: string, field: string): {
  value: unknown;
  record?: Record<string, unknown>;
} {
  const records = snapshot.records[name] ?? [];
  const record =
    records.find((candidate) => String(candidate.entity_id ?? "") === String(snapshot.playerEntityId ?? "")) ??
    records[0];
  return { value: pathValue(record, field), record };
}

function futureValue(snapshot: DashboardSnapshot, source: string): unknown {
  const aliases: Record<string, [string, string?]> = {
    RCS_STATE: ["PROPULSION_STATE", "rcs_intensity"],
    INCOMING_MISSILES: ["THREAT_STATE", "incoming_missiles"]
  };
  const [recordName, field] = aliases[source] ?? [source, undefined];
  const records = snapshot.records[recordName] ?? [];
  if (!records.length) return undefined;
  if (!field) return records;
  const playerRecord =
    records.find((candidate) => String(candidate.entity_id ?? "") === String(snapshot.playerEntityId ?? "")) ??
    records[0];
  return pathValue(playerRecord, field);
}

export function resolveInstrument(
  definition: InstrumentDefinition,
  snapshot: DashboardSnapshot | null
): InstrumentValue {
  if (definition.availability === "nd") {
    const sourceName = definition.source.startsWith("future:")
      ? definition.source.slice("future:".length)
      : "";
    const promoted = snapshot && sourceName ? futureValue(snapshot, sourceName) : undefined;
    if (promoted !== undefined) {
      return {
        state: snapshot?.connection.status === "Stale" ? "stale" : "live",
        value: promoted,
        source: definition.source
      };
    }
    return {
      state: "nd",
      value: null,
      reason: definition.reason ?? "source non produite actuellement",
      source: definition.source
    };
  }
  if (!snapshot || snapshot.connection.status === "Ready" || snapshot.connection.status === "Synchronizing" || snapshot.connection.status === "Disconnected") {
    return {
      state: "waiting",
      value: null,
      reason:
        snapshot?.connection.status === "Disconnected"
          ? "producteur FS2Open indisponible"
          : "synchronisation FSTL en attente",
      source: definition.source
    };
  }

  let value: unknown;
  let sampleTimeUs: string | undefined;
  let recordName: string | undefined;
  let rawRecord: Record<string, unknown> | undefined;
  const [kind, remainder = ""] = definition.source.split(":", 2);
  if (kind === "record") {
    const [name, ...fieldParts] = remainder.split(".");
    recordName = name;
    const resolved = recordValue(snapshot, name, fieldParts.join("."));
    value = resolved.value;
    rawRecord = resolved.record;
    sampleTimeUs =
      resolved.record?.producer_sample_time_us === undefined
        ? undefined
        : String(resolved.record.producer_sample_time_us);
  } else if (kind === "records") {
    recordName = remainder;
    value = snapshot.records[remainder] ?? [];
  } else if (kind === "derived") {
    const key = remainder.replace("$player", String(snapshot.playerEntityId ?? ""));
    const item = snapshot.derived[key];
    if (item && !item.available) {
      return {
        state: "not_applicable",
        value: null,
        reason: item.reason ?? "sans objet",
        source: definition.source
      };
    }
    value = item?.value;
  } else if (kind === "manifest-class") {
    rawRecord = playerClassManifest(snapshot) ?? undefined;
    value = pathValue(rawRecord, remainder);
  } else {
    value = pathValue(snapshot, `${kind}.${remainder}`);
  }

  const quality = qualityFor(snapshot, recordName);
  if (value === undefined || value === null) {
    return {
      state: "not_applicable",
      value: null,
      reason: definition.reason ?? "champ optionnel absent pour l’entité courante",
      source: definition.source,
      sampleTimeUs,
      rawRecord,
      ageUs: quality?.ageUs ?? undefined,
      observedHz: quality?.observedHz ?? undefined
    };
  }
  if (
    (typeof value === "number" && !Number.isFinite(value)) ||
    (Array.isArray(value) && value.some((item) => typeof item === "number" && !Number.isFinite(item)))
  ) {
    return {
      state: "invalid",
      value,
      reason: "valeur non finie",
      source: definition.source,
      sampleTimeUs,
      rawRecord,
      ageUs: quality?.ageUs ?? undefined,
      observedHz: quality?.observedHz ?? undefined
    };
  }
  if (definition.component === "attitude" && attitudeFromQuaternion(value) === null) {
    return {
      state: "invalid",
      value,
      reason: "quaternion local-vers-monde invalide",
      source: definition.source,
      sampleTimeUs,
      rawRecord,
      ageUs: quality?.ageUs ?? undefined,
      observedHz: quality?.observedHz ?? undefined
    };
  }
  return {
    state: snapshot.connection.status === "Stale" ? "stale" : "live",
    value,
    source: definition.source,
    sampleTimeUs,
    ageUs: quality?.ageUs ?? undefined,
    observedHz: quality?.observedHz ?? undefined,
    rawRecord
  };
}

export function resolveDetailFields(
  definition: InstrumentDefinition,
  snapshot: DashboardSnapshot | null
): Array<{ field: string; value: unknown }> {
  if (!snapshot) return [];
  const currentClass = playerClassManifest(snapshot);
  return (definition.detailFields ?? []).map((field) => {
    const [root, ...parts] = field.split(".");
    const path = parts.join(".");
    let value: unknown;
    if (root === "CLASS_MANIFEST") {
      value = pathValue(currentClass, path);
    } else if (root === "derived") {
      const key = path.replace("$player", String(snapshot.playerEntityId ?? ""));
      value = snapshot.derived[key]?.value;
    } else {
      value = recordValue(snapshot, root, path).value;
    }
    return { field, value };
  });
}

export function formatValue(value: unknown): string {
  if (typeof value === "boolean") return value ? "ACTIF" : "INACTIF";
  if (typeof value === "number") {
    const normalized = Object.is(value, -0) ? 0 : value;
    const magnitude = Math.abs(normalized);
    return normalized.toLocaleString("fr-FR", {
      maximumFractionDigits: magnitude < 10 ? 2 : magnitude < 100 ? 1 : 0
    });
  }
  if (typeof value === "string") return value;
  if (Array.isArray(value)) return value.map(formatValue).join(" · ");
  if (value && typeof value === "object") return JSON.stringify(value);
  return "—";
}
