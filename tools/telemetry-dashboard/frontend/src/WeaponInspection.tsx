import { useEffect, useMemo, useState } from "react";
import { formatValue } from "./data";
import {
  bankViews,
  FIRING_PATTERNS,
  GUIDANCE_TYPES,
  turretViews,
  type WeaponFamily
} from "./weaponSemantics";
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
  if (!values) return null;
  return (
    <div className="detail-values">
      <h3>{title}</h3>
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
        <span className="eyebrow">INSPECTION EXHAUSTIVE</span>
        <h2>Tourelles du joueur</h2>
        <div className="detail-state state-live">{turrets.length} TOURELLES</div>
        <label className="subsystem-filter">
          <span>FILTRER PAR NOM</span>
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
              <span><strong>{turret.name}</strong><small>TOURELLE</small></span>
              <b>{turret.locked ? "LOCK" : turret.cooldownUs > 0 ? `${(turret.cooldownUs / 1_000_000).toFixed(2)} s` : "LIBRE"}</b>
            </button>
          ))}
          {!filtered.length ? <p className="detail-empty">AUCUN RÉSULTAT</p> : null}
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
        <span className="eyebrow">INSPECTION EXHAUSTIVE</span>
        <h2>Banques {family === "primary" ? "primaires" : "secondaires"}</h2>
        <div className="detail-state state-live">
          {banks.length} BANQUES · {banks.filter((bank) => bank.selected).length} SÉLECTIONNÉE
        </div>
        <label className="subsystem-filter">
          <span>FILTRER PAR NOM, TYPE OU CAPACITÉ</span>
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
              <b>{bank.selected ? "SÉLECTIONNÉE" : bank.state}</b>
            </button>
          ))}
          {!filtered.length ? <p className="detail-empty">AUCUN RÉSULTAT</p> : null}
        </div>
      </>
    );
  }

  const bank = banks.find((candidate) => candidate.bankId === String(target.bankId));
  if (!bank) {
    return (
      <>
        <span className="eyebrow">INSPECTION BANQUE</span>
        <h2>{target.title ?? "Banque d’arme"}</h2>
        <div className="detail-state state-invalid">ERR</div>
        <p className="detail-empty">Banque absente du snapshot courant.</p>
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
        ← LISTE {familyLabel(family)}
      </button>
      <span className="eyebrow">INSPECTION BANQUE</span>
      <h2>{bank.name}</h2>
      <div className={`detail-state state-${state}`}>{state.toUpperCase()}</div>
      <dl>
        <dt>Famille</dt><dd>{familyLabel(family)}</dd>
        <dt>Banque</dt><dd>ID {bank.bankId} · index {bank.index}</dd>
        <dt>Sélectionnée</dt><dd>{bank.selected ? "OUI" : "NON"}</dd>
        <dt>État temporel</dt><dd>{bank.state}</dd>
        <dt>Classe</dt><dd>{formatValue(bank.record.weapon_class_id)}</dd>
        <dt>Type</dt><dd>{bank.subtype}</dd>
        <dt>Capacités</dt><dd>{bank.flags.join(", ") || "aucune"}</dd>
        <dt>Munitions</dt><dd>{bank.ammoCurrent === null ? "— arme énergétique" : `${bank.ammoCurrent} / ${bank.ammoInitial}`}</dd>
        <dt>Cooldown</dt><dd>{bank.cooldownS === null ? "—" : `${bank.cooldownS.toFixed(3)} s`}</dd>
        <dt>Réarmement</dt><dd>{bank.rearmS === null ? "—" : `${bank.rearmS.toFixed(3)} s`}</dd>
        <dt>Cadence nominale</dt><dd>{bank.nominalRateHz === null ? "—" : `${bank.nominalRateHz.toFixed(2)} tir/s`}</dd>
        <dt>Coût énergétique</dt><dd>{formatValue(fire?.energy_consumed)}</dd>
        <dt>Dégâts nominaux</dt><dd>{formatValue(damage?.amount)}</dd>
        <dt>Guidage</dt><dd>{guidance ? GUIDANCE_TYPES[Number(guidance.type)] ?? "INCONNU" : "—"}</dd>
        <dt>Pattern</dt><dd>{FIRING_PATTERNS[Number(bank.record.pattern_id)] ?? "—"}</dd>
        <dt>Points de tir</dt><dd>{Array.isArray(bank.definition?.fire_points) ? bank.definition.fire_points.length : "—"}</dd>
        <dt>Entité</dt><dd>{snapshot?.playerEntityId ?? "—"}</dd>
        <dt>Échantillon</dt><dd>{formatValue(weaponStateSample(snapshot))}</dd>
        <dt>Âge estimé</dt><dd>{quality?.ageUs === null || quality?.ageUs === undefined ? "—" : `${quality.ageUs} µs`}</dd>
        <dt>Cadence observée</dt><dd>{quality?.observedHz === null || quality?.observedHz === undefined ? "—" : `${quality.observedHz.toFixed(2)} Hz`}</dd>
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
