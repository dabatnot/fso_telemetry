import {
  contactViews,
  lockViews,
  manifestRecord,
  missileViews,
  radarState,
  targetDisplayName,
  targetState,
  threatState
} from "./tacticalSemantics";
import { useI18n } from "./i18n";
import type { DashboardSnapshot, InspectionTarget } from "./types";

interface Props {
  target: InspectionTarget;
  snapshot: DashboardSnapshot | null;
  onSelect: (target: InspectionTarget) => void;
}

function recordNameForKind(kind: InspectionTarget["kind"]): string {
  if (kind === "target") return "TARGET_STATE";
  if (kind === "radar-contact") return "RADAR_CONTACTS";
  if (kind === "lock-point" || kind === "lock-list") return "LOCK_STATE";
  if (kind === "incoming-missile" || kind === "missile-list") return "THREAT_STATE";
  return "RADAR_STATE";
}

function quality(snapshot: DashboardSnapshot | null, recordName: string) {
  return snapshot?.quality.channels.find((channel) =>
    channel.recordName === recordName &&
    (!snapshot.playerEntityId || channel.identity.includes(`entity_id=${snapshot.playerEntityId}`))
  );
}

function derivedForPrefix(snapshot: DashboardSnapshot | null, prefix: string) {
  return Object.entries(snapshot?.derived ?? {})
    .filter(([path]) => path.startsWith(prefix))
    .sort(([left], [right]) => left.localeCompare(right));
}

function ValueList({
  values
}: {
  values: Array<[string, unknown]>;
}) {
  return (
    <div className="detail-values tactical-detail-values">
      {values.map(([name, value]) => (
        <div key={name}>
          <span>{name}</span>
          <code>{value === undefined ? "—" : JSON.stringify(value)}</code>
        </div>
      ))}
    </div>
  );
}

function ContactList({ target, snapshot, onSelect }: Props) {
  const { t } = useI18n();
  const contacts = contactViews(snapshot);
  return (
    <>
      <span className="eyebrow">{t("PISTES AUTORISÉES")}</span>
      <h2>{t("Contacts radar")} · {contacts.length}</h2>
      <div className="inspection-choice-list">
        {contacts.map((contact) => (
          <button
            key={contact.id}
            onClick={() => onSelect({
              ...target,
              kind: "radar-contact",
              entityId: contact.id,
              record: contact.record,
              title: contact.name
            })}
          >
            <span>{contact.name}</span>
            <strong>{contact.invalid ? "ERR" : t(contact.category)}</strong>
            <small>{t(contact.visibility)} · {contact.distance === null ? "—" : `${contact.distance.toFixed(0)} u`}</small>
          </button>
        ))}
      </div>
    </>
  );
}

function LockList({ target, snapshot, onSelect }: Props) {
  const { t } = useI18n();
  const locks = lockViews(snapshot);
  return (
    <>
      <span className="eyebrow">{t("LISTE EXHAUSTIVE")}</span>
      <h2>{t("Verrouillages")} · {locks.length}</h2>
      <div className="inspection-choice-list">
        {locks.map((lock) => (
          <button
            key={`${lock.targetId}-${lock.index}`}
            onClick={() => onSelect({
              ...target,
              kind: "lock-point",
              entityId: lock.targetId,
              record: lock.record,
              title: lock.targetName
            })}
          >
            <span>{lock.targetName}</span>
            <strong>{lock.invalid ? "ERR" : t(lock.locked ? "ACQUIS" : lock.attempt ? "EN COURS" : "AUCUNE TENTATIVE")}</strong>
            <small>{lock.remainingS === null ? "—" : `${lock.remainingS.toFixed(1)} s`}</small>
          </button>
        ))}
      </div>
    </>
  );
}

function MissileList({ target, snapshot, onSelect }: Props) {
  const { t } = useI18n();
  const missiles = missileViews(snapshot);
  return (
    <>
      <span className="eyebrow">{t("LISTE EXHAUSTIVE")}</span>
      <h2>{t("Missiles entrants")} · {missiles.length}</h2>
      <div className="inspection-choice-list">
        {missiles.map((missile) => (
          <button
            key={missile.id}
            onClick={() => onSelect({
              ...target,
              kind: "incoming-missile",
              entityId: missile.id,
              record: missile.record,
              title: missile.name
            })}
          >
            <span>{missile.name}</span>
            <strong>{missile.invalid ? "ERR" : t(missile.guidance)}</strong>
            <small>{missile.ttcS === null ? "TTC —" : `TTC ${missile.ttcS.toFixed(1)} s · EST.`}</small>
          </button>
        ))}
      </div>
    </>
  );
}

export function TacticalInspection(props: Props) {
  const { t } = useI18n();
  const { target, snapshot } = props;
  if (target.kind === "radar-contact" && !target.entityId) {
    return <ContactList {...props} />;
  }
  if (target.kind === "lock-list") return <LockList {...props} />;
  if (target.kind === "missile-list") return <MissileList {...props} />;

  const recordName = recordNameForKind(target.kind);
  const channel = quality(snapshot, recordName);
  let record = target.record ?? null;
  let title = target.title ?? target.definition.label;
  let derivedPrefix = "";
  let manifest: Record<string, unknown> | null = null;

  if (target.kind === "target") {
    record = targetState(snapshot);
    title = targetDisplayName(snapshot);
    derivedPrefix = `entities.${snapshot?.playerEntityId ?? ""}.target`;
    const identity = record?.revealed_identity;
    const classId = identity && typeof identity === "object"
      ? (identity as Record<string, unknown>).class_id
      : undefined;
    manifest = manifestRecord(snapshot, "CLASS_MANIFEST", "class_id", classId);
  } else if (target.kind === "radar-contact") {
    const contact = contactViews(snapshot).find((item) => item.id === target.entityId);
    record = contact?.record ?? record;
    title = contact?.name ?? title;
    derivedPrefix = `entities.${snapshot?.playerEntityId ?? ""}.tracks.${target.entityId ?? ""}`;
    manifest = manifestRecord(
      snapshot,
      "CLASS_MANIFEST",
      "class_id",
      record?.revealed_class_id
    );
  } else if (target.kind === "lock-point") {
    const lock = lockViews(snapshot).find((item) =>
      item.record === record ||
      (item.targetId === target.entityId &&
        String(item.record.subsystem_id ?? "") === String(record?.subsystem_id ?? ""))
    );
    record = lock?.record ?? record;
    title = lock?.targetName ?? title;
    derivedPrefix = `entities.${snapshot?.playerEntityId ?? ""}.locks[${lock?.index ?? 0}]`;
  } else if (target.kind === "incoming-missile") {
    const missile = missileViews(snapshot).find((item) => item.id === target.entityId);
    record = missile?.record ?? record;
    title = missile?.name ?? title;
    derivedPrefix = `entities.${snapshot?.playerEntityId ?? ""}.missiles.${target.entityId ?? ""}`;
    manifest = manifestRecord(
      snapshot,
      "WEAPON_MANIFEST",
      "weapon_class_id",
      record?.weapon_class_id
    );
  } else {
    record = radarState(snapshot) ?? threatState(snapshot);
  }

  const sampleTime = record?.producer_sample_time_us;
  const derived = derivedPrefix ? derivedForPrefix(snapshot, derivedPrefix) : [];
  return (
    <>
      <span className="eyebrow">{t("INSPECTION TACTIQUE")}</span>
      <h2>{title}</h2>
      <div className="detail-state state-live">{record ? "LIVE" : "—"}</div>
      <dl>
        <dt>Record</dt><dd><code>{recordName}</code></dd>
        <dt>{t("Entité")}</dt><dd>{target.entityId ?? String(record?.entity_id ?? "—")}</dd>
        <dt>{t("Présence")}</dt><dd>{String(record?.presence ?? "—")}</dd>
        <dt>{t("Échantillon")}</dt><dd>{String(sampleTime ?? "—")}</dd>
        <dt>{t("Âge estimé")}</dt><dd>{channel?.ageUs === null || channel?.ageUs === undefined ? "—" : `${channel.ageUs} µs`}</dd>
        <dt>{t("Cadence observée")}</dt><dd>{channel?.observedHz === null || channel?.observedHz === undefined ? "—" : `${channel.observedHz.toFixed(2)} Hz`}</dd>
      </dl>
      <h3>{t("Valeur brute")}</h3>
      <code className="inspection-json">{record ? JSON.stringify(record, null, 2) : "—"}</code>
      {derived.length > 0 && (
        <>
          <h3>{t("Dérivations et provenance")}</h3>
          <ValueList values={derived.map(([path, value]) => [path, value])} />
        </>
      )}
      {manifest && (
        <>
          <h3>{t("Manifeste autorisé")}</h3>
          <code className="inspection-json">{JSON.stringify(manifest, null, 2)}</code>
        </>
      )}
    </>
  );
}
