import { useEffect, useMemo, useState } from "react";
import { formatValue } from "./data";
import { subsystemViews } from "./integritySemantics";
import { useI18n } from "./i18n";
import type { DashboardSnapshot, InspectionTarget } from "./types";

interface Props {
  target: InspectionTarget;
  snapshot: DashboardSnapshot | null;
  onSelect: (target: InspectionTarget) => void;
}

function RawValues({
  title,
  values
}: {
  title: string;
  values: Record<string, unknown> | null | undefined;
}) {
  const { t } = useI18n();
  if (!values) return null;
  return (
    <div className="detail-values">
      <h3>{t(title)}</h3>
      {Object.entries(values).map(([field, value]) => (
        <div key={field}>
          <span>{field}</span>
          <code>{value === undefined ? "—" : JSON.stringify(value)}</code>
        </div>
      ))}
    </div>
  );
}

export function IntegrityInspection({ target, snapshot, onSelect }: Props) {
  const { t } = useI18n();
  const [filter, setFilter] = useState("");
  useEffect(() => setFilter(""), [target.kind]);
  const systems = useMemo(() => subsystemViews(snapshot), [snapshot]);
  const normalizedFilter = filter.trim().toLocaleLowerCase("fr");
  const filtered = normalizedFilter
    ? systems.filter((system) =>
        `${system.name} ${system.typeLabel} ${system.flags.join(" ")}`
          .toLocaleLowerCase("fr")
          .includes(normalizedFilter)
      )
    : systems;

  if (target.kind === "subsystem-list") {
    return (
      <>
        <span className="eyebrow">{t("INSPECTION EXHAUSTIVE")}</span>
        <h2>{t("Sous-systèmes du joueur")}</h2>
        <div className="detail-state state-live">
          {systems.length} {t("SYSTÈMES")} · {systems.filter((system) => system.destroyed).length} {t("DÉTRUITS")}
        </div>
        <label className="subsystem-filter">
          <span>{t("FILTRER PAR NOM, TYPE OU ÉTAT")}</span>
          <input
            type="search"
            value={filter}
            onChange={(event) => setFilter(event.currentTarget.value)}
            placeholder="moteur, perturbé…"
          />
        </label>
        <div className="subsystem-detail-list">
          {filtered.map((system) => (
            <button
              key={`${system.entityId}-${system.subsystemId}`}
              onClick={() => onSelect({
                definition: target.definition,
                kind: "subsystem",
                record: system.record,
                title: system.name
              })}
            >
              <span><strong>{t(system.name)}</strong><small>{t(system.typeLabel)}</small></span>
              <b>{system.ratio === null ? "—" : `${(system.ratio * 100).toFixed(0)}%`}</b>
            </button>
          ))}
          {!filtered.length ? <p className="detail-empty">{t("AUCUN RÉSULTAT")}</p> : null}
        </div>
      </>
    );
  }

  const system = systems.find(
    (candidate) =>
      String(candidate.record.entity_id) === String(target.record?.entity_id) &&
      String(candidate.record.subsystem_id) === String(target.record?.subsystem_id)
  );
  if (!system) {
    return (
      <>
        <span className="eyebrow">{t("INSPECTION SOUS-SYSTÈME")}</span>
        <h2>{target.title ?? t("Sous-système")}</h2>
        <div className="detail-state state-invalid">ERR</div>
        <p className="detail-empty">{t("Sous-système absent du snapshot courant.")}</p>
      </>
    );
  }

  const quality = snapshot?.quality.channels.find(
    (channel) =>
      channel.recordName === "SUBSYSTEM_STATE" &&
      channel.identity.includes(`entity_id=${system.entityId}`) &&
      channel.identity.includes(`subsystem_id=${system.subsystemId}`)
  );
  const derivedPrefix = `entities.${system.entityId}.subsystems.${system.subsystemId}`;
  const state = snapshot?.connection.status === "Stale" ? "stale" : "live";
  const turret = system.record.turret && typeof system.record.turret === "object"
    ? system.record.turret as Record<string, unknown>
    : null;

  return (
    <>
      <button
        className="detail-back"
        onClick={() => onSelect({
          definition: target.definition,
          kind: target.definition.id === "weapon-turrets" ? "turret-list" : "subsystem-list"
        })}
      >
        ← {t("LISTE COMPLÈTE")}
      </button>
      <span className="eyebrow">{t("INSPECTION SOUS-SYSTÈME")}</span>
      <h2>{t(system.name)}</h2>
      <div className={`detail-state state-${state}`}>{state.toUpperCase()}</div>
      <dl>
        <dt>{t("Type")}</dt><dd>{t(system.typeLabel)}</dd>
        <dt>{t("Entité")}</dt><dd>{system.entityId}</dd>
        <dt>{t("ID sous-système")}</dt><dd>{system.subsystemId}</dd>
        <dt>{t("Index canonique")}</dt><dd>{formatValue(system.record.canonical_index)}</dd>
        <dt>{t("Intégrité")}</dt><dd>{system.ratio === null ? `— ${t("sans réserve de HP")}` : `${(system.ratio * 100).toFixed(1)} %`}</dd>
        <dt>HP</dt><dd>{formatValue(system.record.current_hits)} / {formatValue(system.record.max_hits)}</dd>
        <dt>{t("HP manquants")}</dt><dd>{formatValue(snapshot?.derived[`${derivedPrefix}.missing_hits`]?.value)}</dd>
        <dt>{t("Détruit")}</dt><dd>{t(system.destroyed ? "OUI" : "NON")}</dd>
        <dt>{t("États")}</dt><dd>{system.flags.map(t).join(", ") || t("aucun")}</dd>
        <dt>{t("Armure")}</dt><dd>{system.record.armor_id === undefined ? "—" : `ID ${system.record.armor_id}`}</dd>
        <dt>{t("Perturbation")}</dt><dd>{system.record.perturbation_remaining_us === undefined ? "—" : `${formatValue(system.record.perturbation_remaining_us)} µs`}</dd>
        <dt>{t("Cooldown tourelle")}</dt><dd>{turret?.cooldown_remaining_us === undefined ? "—" : `${formatValue(turret.cooldown_remaining_us)} µs`}</dd>
        <dt>{t("Échantillon")}</dt><dd>{formatValue(system.record.producer_sample_time_us)}</dd>
        <dt>{t("Âge estimé")}</dt><dd>{quality?.ageUs === null || quality?.ageUs === undefined ? "—" : `${quality.ageUs} µs`}</dd>
        <dt>{t("Cadence")}</dt><dd>{quality?.observedHz === null || quality?.observedHz === undefined ? "—" : `${quality.observedHz.toFixed(2)} Hz`}</dd>
      </dl>
      <RawValues title="Valeurs brutes" values={system.record} />
      <RawValues title="Définition de classe" values={system.definition} />
      <RawValues title="Résumé mécanique de tourelle" values={turret} />
    </>
  );
}
