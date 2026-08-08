import { useEffect, useMemo, useState } from "react";
import { formatValue } from "./data";
import {
  bankViews,
  FIRING_PATTERNS,
  GUIDANCE_TYPES,
  turretViews,
  type WeaponFamily
} from "./weaponSemantics";
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

function familyLabel(family: WeaponFamily): string {
  return family === "primary" ? "PRIMAIRE" : "SECONDAIRE";
}

export function WeaponInspection({ target, snapshot, onSelect }: Props) {
  const { t } = useI18n();
  const [filter, setFilter] = useState("");
  useEffect(() => setFilter(""), [target.kind, target.family]);
  const family = target.family === "secondary" ? "secondary" : "primary";
  const banks = useMemo(() => bankViews(snapshot, family), [snapshot, family]);
  const turrets = useMemo(() => turretViews(snapshot), [snapshot]);
  const normalized = filter.trim().toLocaleLowerCase("fr");

  if (target.kind === "turret-list") {
    const filtered = normalized
      ? turrets.filter((turret) => turret.name.toLocaleLowerCase("fr").includes(normalized))
      : turrets;
    return (
      <>
        <span className="eyebrow">{t("INSPECTION EXHAUSTIVE")}</span>
        <h2>{t("Tourelles du joueur")}</h2>
        <div className="detail-state state-live">{turrets.length} {t("TOURELLES")}</div>
        <label className="subsystem-filter">
          <span>{t("FILTRER PAR NOM")}</span>
          <input
            type="search"
            value={filter}
            onChange={(event) => setFilter(event.currentTarget.value)}
            placeholder="tourelle…"
          />
        </label>
        <div className="subsystem-detail-list">
          {filtered.map((turret) => (
            <button
              key={turret.subsystemId}
              onClick={() => onSelect({
                definition: target.definition,
                kind: "subsystem",
                record: turret.record,
                subsystemId: turret.subsystemId,
                title: turret.name
              })}
            >
              <span><strong>{turret.name}</strong><small>{t("TOURELLE")}</small></span>
              <b>{turret.locked ? "LOCK" : turret.cooldownUs > 0 ? `${(turret.cooldownUs / 1_000_000).toFixed(2)} s` : t("LIBRE")}</b>
            </button>
          ))}
          {!filtered.length ? <p className="detail-empty">{t("AUCUN RÉSULTAT")}</p> : null}
        </div>
      </>
    );
  }

  if (target.kind === "weapon-bank-list") {
    const filtered = normalized
      ? banks.filter((bank) =>
          `${bank.name} ${bank.subtype} ${bank.flags.join(" ")}`
            .toLocaleLowerCase("fr")
            .includes(normalized)
        )
      : banks;
    return (
      <>
        <span className="eyebrow">{t("INSPECTION EXHAUSTIVE")}</span>
        <h2>{t("Banques")} {t(family === "primary" ? "primaires" : "secondaires")}</h2>
        <div className="detail-state state-live">
          {banks.length} {t("BANQUES")} · {banks.filter((bank) => bank.selected).length} {t("SÉLECTIONNÉE")}
        </div>
        <label className="subsystem-filter">
          <span>{t("FILTRER PAR NOM, TYPE OU CAPACITÉ")}</span>
          <input
            type="search"
            value={filter}
            onChange={(event) => setFilter(event.currentTarget.value)}
            placeholder="missile, beam, guidée…"
          />
        </label>
        <div className="subsystem-detail-list">
          {filtered.map((bank) => (
            <button
              key={bank.bankId}
              onClick={() => onSelect({
                definition: target.definition,
                kind: "weapon-bank",
                family,
                bankId: bank.bankId,
                record: bank.record,
                title: bank.name
              })}
            >
              <span><strong>{bank.name}</strong><small>{bank.subtype} · ID {bank.bankId}</small></span>
              <b>{t(bank.selected ? "SÉLECTIONNÉE" : bank.state)}</b>
            </button>
          ))}
          {!filtered.length ? <p className="detail-empty">{t("AUCUN RÉSULTAT")}</p> : null}
        </div>
      </>
    );
  }

  const bank = banks.find((candidate) => candidate.bankId === String(target.bankId));
  if (!bank) {
    return (
      <>
        <span className="eyebrow">{t("INSPECTION BANQUE")}</span>
        <h2>{target.title ?? t("Banque d’arme")}</h2>
        <div className="detail-state state-invalid">ERR</div>
        <p className="detail-empty">{t("Banque absente du snapshot courant.")}</p>
      </>
    );
  }
  const quality = snapshot?.quality.channels.find(
    (channel) =>
      channel.recordName === "WEAPON_STATE" &&
      channel.identity.includes(`entity_id=${snapshot.playerEntityId}`)
  );
  const manifest = bank.weaponClass;
  const fire = manifest?.fire as Record<string, unknown> | undefined;
  const damage = manifest?.damage as Record<string, unknown> | undefined;
  const guidance = manifest?.guidance as Record<string, unknown> | undefined;
  const state = snapshot?.connection.status === "Stale" ? "stale" : "live";
  return (
    <>
      <button
        className="detail-back"
        onClick={() => onSelect({
          definition: target.definition,
          kind: "weapon-bank-list",
          family
        })}
      >
        ← {t("LISTE")} {t(familyLabel(family))}
      </button>
      <span className="eyebrow">{t("INSPECTION BANQUE")}</span>
      <h2>{bank.name}</h2>
      <div className={`detail-state state-${state}`}>{state.toUpperCase()}</div>
      <dl>
        <dt>{t("Famille")}</dt><dd>{t(familyLabel(family))}</dd>
        <dt>{t("Banque")}</dt><dd>ID {bank.bankId} · index {bank.index}</dd>
        <dt>{t("Sélectionnée")}</dt><dd>{t(bank.selected ? "OUI" : "NON")}</dd>
        <dt>{t("État temporel")}</dt><dd>{t(bank.state)}</dd>
        <dt>{t("Classe")}</dt><dd>{formatValue(bank.record.weapon_class_id)}</dd>
        <dt>{t("Type")}</dt><dd>{t(bank.subtype)}</dd>
        <dt>{t("Capacités")}</dt><dd>{bank.flags.map(t).join(", ") || t("aucune")}</dd>
        <dt>{t("Munitions")}</dt><dd>{bank.ammoCurrent === null ? `— ${t("arme énergétique")}` : `${bank.ammoCurrent} / ${bank.ammoInitial}`}</dd>
        <dt>Cooldown</dt><dd>{bank.cooldownS === null ? "—" : `${bank.cooldownS.toFixed(3)} s`}</dd>
        <dt>{t("Réarmement")}</dt><dd>{bank.rearmS === null ? "—" : `${bank.rearmS.toFixed(3)} s`}</dd>
        <dt>{t("Cadence nominale")}</dt><dd>{bank.nominalRateHz === null ? "—" : `${bank.nominalRateHz.toFixed(2)} ${t("tir/s")}`}</dd>
        <dt>{t("Coût énergétique")}</dt><dd>{formatValue(fire?.energy_consumed)}</dd>
        <dt>{t("Dégâts nominaux")}</dt><dd>{formatValue(damage?.amount)}</dd>
        <dt>{t("Guidage")}</dt><dd>{guidance ? t(GUIDANCE_TYPES[Number(guidance.type)] ?? "INCONNU") : "—"}</dd>
        <dt>Pattern</dt><dd>{t(FIRING_PATTERNS[Number(bank.record.pattern_id)] ?? "—")}</dd>
        <dt>{t("Points de tir")}</dt><dd>{Array.isArray(bank.definition?.fire_points) ? bank.definition.fire_points.length : "—"}</dd>
        <dt>{t("Entité")}</dt><dd>{snapshot?.playerEntityId ?? "—"}</dd>
        <dt>{t("Échantillon")}</dt><dd>{formatValue(weaponStateSample(snapshot))}</dd>
        <dt>{t("Âge estimé")}</dt><dd>{quality?.ageUs === null || quality?.ageUs === undefined ? "—" : `${quality.ageUs} µs`}</dd>
        <dt>{t("Cadence observée")}</dt><dd>{quality?.observedHz === null || quality?.observedHz === undefined ? "—" : `${quality.observedHz.toFixed(2)} Hz`}</dd>
      </dl>
      <RawValues title="État runtime brut" values={bank.record} />
      <RawValues title="Manifeste de l’arme" values={manifest} />
      <RawValues title="Définition de banque et géométrie" values={bank.definition} />
    </>
  );
}

function weaponStateSample(snapshot: DashboardSnapshot | null): unknown {
  const record = (snapshot?.records.WEAPON_STATE ?? []).find(
    (candidate) => String(candidate.entity_id) === String(snapshot?.playerEntityId)
  );
  return record?.producer_sample_time_us;
}
