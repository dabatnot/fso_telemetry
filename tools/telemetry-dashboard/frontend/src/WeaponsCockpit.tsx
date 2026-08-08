import type { ReactNode } from "react";
import { formatValue, resolveInstrument } from "./data";
import {
  bankViews,
  countermeasureState,
  decodeBitmap,
  GUIDANCE_TYPES,
  selectedBank,
  selectedBankIsInvalid,
  turretViews,
  visibleBanks,
  WEAPON_GLOBAL_FLAGS,
  weaponDisplayName,
  weaponManifest,
  weaponState,
  type WeaponBankView,
  type WeaponFamily
} from "./weaponSemantics";
import { playerRecord } from "./integritySemantics";
import { useI18n } from "./i18n";
import type {
  DashboardSnapshot,
  InspectionTarget,
  InstrumentDefinition
} from "./types";

interface Props {
  definitions: InstrumentDefinition[];
  snapshot: DashboardSnapshot | null;
  onInspect: (target: InspectionTarget) => void;
}

function definitionById(definitions: InstrumentDefinition[], id: string) {
  const found = definitions.find((definition) => definition.id === id);
  if (!found) throw new Error(`instrument manquant: ${id}`);
  return found;
}

function number(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function seconds(value: unknown): string {
  const numeric = number(value);
  return numeric === null ? "—" : `${(numeric / 1_000_000).toFixed(numeric < 100_000 ? 2 : 1)} s`;
}

function Metric({ label, value, unit }: { label: string; value: unknown; unit?: string }) {
  const { t } = useI18n();
  return (
    <div className="weapon-metric">
      <span>{t(label)}</span>
      <strong>{value === undefined || value === null ? "—" : t(formatValue(value))}</strong>
      {unit ? <small>{t(unit)}</small> : null}
    </div>
  );
}

function CockpitPanel({
  title,
  definition,
  snapshot,
  children,
  className = ""
}: {
  title: string;
  definition: InstrumentDefinition;
  snapshot: DashboardSnapshot | null;
  children: ReactNode;
  className?: string;
}) {
  const { t } = useI18n();
  const resolved = resolveInstrument(definition, snapshot);
  const unavailable = !["live", "stale"].includes(resolved.state);
  const labels: Record<string, string> = {
    nd: "ND", not_applicable: "—", invalid: "ERR", waiting: "EN ATTENTE"
  };
  return (
    <section className={`weapon-panel state-${resolved.state} ${className}`}>
      <header>
        <span>{t(title)}</span>
        <i>{resolved.state === "live" ? "LIVE" : resolved.state === "stale" ? "STALE" : t(labels[resolved.state])}</i>
      </header>
      <div className="weapon-panel-body">
        {unavailable ? (
          <div className="weapon-unavailable">
            <strong>{t(labels[resolved.state])}</strong>
            <span>{t(resolved.reason ?? "")}</span>
          </div>
        ) : children}
      </div>
    </section>
  );
}

function BankRack({
  family,
  banks,
  definition,
  onInspect
}: {
  family: WeaponFamily;
  banks: WeaponBankView[];
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const visible = visibleBanks(banks);
  return (
    <div className="weapon-rack">
      {!banks.length ? <div className="weapon-not-applicable">— {t("AUCUNE BANQUE")}</div> : null}
      {visible.map((bank) => (
        <button
          key={bank.bankId}
          className={`weapon-rack-row ${bank.selected ? "selected" : ""} state-${bank.state.toLocaleLowerCase("fr")}`}
          onClick={() => onInspect({
            definition,
            kind: "weapon-bank",
            family,
            bankId: bank.bankId,
            record: bank.record,
            title: bank.name
          })}
          aria-label={`${t("Inspecter")} ${bank.name}`}
        >
          <span>
            <b>{String(bank.index + 1).padStart(2, "0")}</b>
            <strong>{bank.name}</strong>
            <small>{bank.subtype}</small>
          </span>
          <em>{t(bank.state)}</em>
          {bank.ammoRatio !== null ? (
            <div className="weapon-ammo-line">
              <i style={{ width: `${bank.ammoRatio * 100}%` }} />
            </div>
          ) : (
            <div className="weapon-cooldown-line">
              {bank.cooldownS === null ? t("ÉNERGIE") : `${bank.cooldownS.toFixed(2)} s`}
            </div>
          )}
        </button>
      ))}
      {banks.length > visible.length ? (
        <button
          className="weapon-more"
          onClick={() => onInspect({
            definition,
            kind: "weapon-bank-list",
            family,
            title: t(family === "primary" ? "Banques primaires" : "Banques secondaires")
          })}
        >
          + {banks.length - visible.length} {t("AUTRES · LISTE COMPLÈTE")}
        </button>
      ) : null}
    </div>
  );
}

function SelectedWeapon({
  family,
  snapshot,
  definition,
  onInspect
}: {
  family: WeaponFamily;
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const state = weaponState(snapshot);
  const bank = selectedBank(snapshot, family);
  const count = Number(state?.[`${family}_bank_count`] ?? 0);
  const invalid = selectedBankIsInvalid(snapshot, family);
  if (count === 0) {
    return (
      <article className="selected-weapon selected-empty">
        <header>{t(family === "primary" ? "PRIMAIRE SÉLECTIONNÉE" : "SECONDAIRE SÉLECTIONNÉE")}</header>
        <strong>—</strong><span>{t("aucune banque installée")}</span>
      </article>
    );
  }
  if (!bank || invalid) {
    return (
      <article className="selected-weapon selected-invalid">
        <header>{t(family === "primary" ? "PRIMAIRE SÉLECTIONNÉE" : "SECONDAIRE SÉLECTIONNÉE")}</header>
        <strong>ERR</strong><span>{t("sélecteur incohérent avec les banques reçues")}</span>
      </article>
    );
  }
  const manifest = bank.weaponClass;
  const ranges = manifest?.ranges as Record<string, unknown> | undefined;
  const fire = manifest?.fire as Record<string, unknown> | undefined;
  const damage = manifest?.damage as Record<string, unknown> | undefined;
  const guidance = manifest?.guidance as Record<string, unknown> | undefined;
  const lock = manifest?.lock as Record<string, unknown> | undefined;
  const burst = manifest?.burst as Record<string, unknown> | undefined;
  const swarm = manifest?.swarm as Record<string, unknown> | undefined;
  const cooldown = bank.cooldownS;
  return (
    <button
      className={`selected-weapon state-${bank.state.toLocaleLowerCase("fr")}`}
      onClick={() => onInspect({
        definition,
        kind: "weapon-bank",
        family,
        bankId: bank.bankId,
        record: bank.record,
        title: bank.name
      })}
      aria-label={`${t("Inspecter")} ${bank.name}`}
    >
      <header>
        <span>{t(family === "primary" ? "PRIMAIRE SÉLECTIONNÉE" : "SECONDAIRE SÉLECTIONNÉE")}</span>
        <i>{t(bank.state)}</i>
      </header>
      <div className="selected-weapon-title">
        <strong>{bank.name}</strong>
        <span>{t(bank.subtype)} · {t("BANQUE")} {bank.index + 1}</span>
      </div>
      <div className="weapon-badges">
        {bank.flags.slice(0, 5).map((flag) => <span key={flag}>{t(flag)}</span>)}
        {!bank.flags.length ? <span>{t("STANDARD")}</span> : null}
      </div>
      <div className="selected-weapon-grid">
        <Metric
          label={bank.ammoCurrent === null ? "COÛT ÉNERGIE" : "MUNITIONS"}
          value={bank.ammoCurrent === null ? fire?.energy_consumed : `${bank.ammoCurrent} / ${bank.ammoInitial}`}
          unit={bank.ammoCurrent === null ? "énergie" : undefined}
        />
        <Metric label="COOLDOWN" value={cooldown} unit="s" />
        <Metric label="DÉGÂTS NOMINAUX" value={damage?.amount} />
        <Metric label="CADENCE NOMINALE" value={bank.nominalRateHz} unit="tir/s" />
        <Metric label="PORTÉE OPTIMALE" value={ranges?.optimal} unit="m" />
        <Metric label="PORTÉE MAXIMALE" value={ranges?.maximum} unit="m" />
      </div>
      <div className="weapon-combat-strip">
        <span>{t("GUIDAGE")} <b>{guidance ? t(GUIDANCE_TYPES[Number(guidance.type)] ?? "INCONNU") : "—"}</b></span>
        <span>{t("LOCK NOMINAL")} <b>{lock ? seconds(lock.time_us) : "—"}</b></span>
        <span>BURST <b>{burst ? `${formatValue(burst.count)} × ${seconds(burst.interval_us)}` : "—"}</b></span>
        <span>SWARM <b>{swarm ? `${formatValue(swarm.count)} × ${formatValue(swarm.shots_per_trigger)}` : "—"}</b></span>
      </div>
    </button>
  );
}

function GlobalModes({ snapshot }: { snapshot: DashboardSnapshot | null }) {
  const { t } = useI18n();
  const state = weaponState(snapshot);
  const flags = decodeBitmap(state?.weapon_flags, WEAPON_GLOBAL_FLAGS) ?? [];
  const primaryHeld = flags.includes("GÂCHETTE PRIMAIRE");
  const secondaryHeld = flags.includes("GÂCHETTE SECONDAIRE");
  const important = [
    ["PRIMAIRES", flags.includes("PRIMAIRES LIÉES") ? "LIÉES" : "SÉPARÉES"],
    ["SECONDAIRE", flags.includes("SECONDAIRE DOUBLE") ? "DOUBLE" : "SIMPLE"],
    ["GÂCHETTE P", primaryHeld ? "DEMANDÉE" : "REPOS"],
    ["GÂCHETTE S", secondaryHeld ? "DEMANDÉE" : "REPOS"],
    ["LOCK P", flags.includes("PRIMAIRES VERROUILLÉES") ? "VERROUILLÉ" : "LIBRE"],
    ["LOCK S", flags.includes("SECONDAIRES VERROUILLÉES") ? "VERROUILLÉ" : "LIBRE"],
    ["LASER", flags.includes("LASER DE CIBLAGE") ? "ACTIF" : "INACTIF"],
    ["DÉTONATEURS", flags.includes("DÉTONATEURS DISTANTS") ? "ACTIFS" : "INACTIFS"]
  ];
  return (
    <div className="weapon-mode-strip">
      {important.map(([label, value]) => (
        <div key={label} className={value === "ACTIF" || value === "ACTIFS" || value === "DEMANDÉE" ? "active" : ""}>
          <span>{t(label)}</span><strong>{t(value)}</strong>
        </div>
      ))}
    </div>
  );
}

function Reserves({ snapshot }: { snapshot: DashboardSnapshot | null }) {
  const { t } = useI18n();
  const state = weaponState(snapshot);
  const energy = playerRecord(snapshot, "ENERGY_STATE") ?? {};
  const control = playerRecord(snapshot, "CONTROL_STATE") ?? {};
  const tertiary = state?.tertiary as Record<string, unknown> | undefined;
  const countermeasure = state?.countermeasure as Record<string, unknown> | undefined;
  const swarm = state?.swarm as Record<string, unknown> | undefined;
  const remote = state?.remote_detonation_remaining_us;
  const tertiaryState = snapshot?.playerEntityId
    ? snapshot.derived[`entities.${snapshot.playerEntityId}.tertiary.state`]
    : undefined;
  const countermeasureDerivedState = snapshot?.playerEntityId
    ? snapshot.derived[`entities.${snapshot.playerEntityId}.countermeasure.state`]
    : undefined;
  const countermeasureManifest = countermeasure
    ? weaponManifest(snapshot, countermeasure.weapon_class_id)
    : null;
  return (
    <div className="weapon-reserves">
      <div className="weapon-reserve-block energy">
        <span>{t("ÉNERGIE ARMES")}</span>
        <strong>{formatValue(energy.weapon_energy_current)} / {formatValue(energy.weapon_energy_max)}</strong>
        <small>{t("réserve contextuelle · pas une autorisation de tir")}</small>
      </div>
      <div className="weapon-reserve-block">
        <span>{t("TERTIAIRE")}</span>
        {tertiary ? (
          <>
            <strong>{formatValue(tertiary.ammunition_current)} / {formatValue(tertiary.ammunition_initial)}</strong>
            <small>{tertiaryState?.available ? String(tertiaryState.value) : "ÉTAT —"} · cooldown {seconds(tertiary.cooldown_remaining_us)}</small>
          </>
        ) : <><strong>—</strong><small>{t("aucune banque tertiaire")}</small></>}
      </div>
      <div className="weapon-reserve-block">
        <span>{t("CONTRE-MESURES")}</span>
        {countermeasure ? (
          <>
            <strong>{formatValue(countermeasure.current)} / {formatValue(countermeasure.maximum)}</strong>
            <small>{weaponDisplayName(countermeasureManifest)} · {t(countermeasureDerivedState?.available ? String(countermeasureDerivedState.value) : countermeasureState(countermeasure))}</small>
          </>
        ) : <><strong>—</strong><small>{t("aucune contre-mesure")}</small></>}
      </div>
      <div className="weapon-reserve-block">
        <span>{t("SALVE / DÉTONATION")}</span>
        <strong>{swarm ? `${formatValue(swarm.remaining)} ${t("RESTANTS")}` : "—"}</strong>
        <small>{remote === undefined ? t("aucun détonateur actif") : `${t("détonation")} ${seconds(remote)}`}</small>
      </div>
      <div className="weapon-reserve-block requests">
        <span>{t("DEMANDES DE COMMANDE")}</span>
        <strong>
          P {formatValue(control.fire_primary_count)} · S {formatValue(control.fire_secondary_count)} · CM {formatValue(control.fire_countermeasure_count)}
        </strong>
        <small>{t("intentions · jamais des tirs confirmés")}</small>
      </div>
    </div>
  );
}

function TurretSummary({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const turrets = turretViews(snapshot);
  if (!turrets.length) return null;
  return (
    <div className="weapon-turrets">
      <header><span>{t("TOURELLES DU JOUEUR")}</span><b>{turrets.length}</b></header>
      <div>
        {turrets.slice(0, 4).map((turret) => (
          <button
            key={turret.subsystemId}
            onClick={() => onInspect({
              definition,
              kind: "subsystem",
              record: turret.record,
              subsystemId: turret.subsystemId,
              title: turret.name
            })}
          >
            <span>{turret.name}</span>
            <strong>{turret.locked ? t("VERROUILLÉE") : turret.cooldownUs > 0 ? seconds(turret.cooldownUs) : turret.targetActive ? t("CIBLE") : t("DISPONIBLE")}</strong>
          </button>
        ))}
      </div>
      {turrets.length > 4 ? (
        <button
          className="weapon-more"
          onClick={() => onInspect({ definition, kind: "turret-list", title: t("Tourelles du joueur") })}
        >
          + {turrets.length - 4} {t("AUTRES")}
        </button>
      ) : null}
    </div>
  );
}

export function WeaponsCockpit({ definitions, snapshot, onInspect }: Props) {
  const { t } = useI18n();
  const modes = definitionById(definitions, "weapon-modes");
  const primary = definitionById(definitions, "weapon-primary-rack");
  const selected = definitionById(definitions, "weapon-selected");
  const secondary = definitionById(definitions, "weapon-secondary-rack");
  const reserves = definitionById(definitions, "weapon-reserves");
  const turrets = definitionById(definitions, "weapon-turrets");
  const future = definitionById(definitions, "weapon-future");
  const primaryBanks = bankViews(snapshot, "primary");
  const secondaryBanks = bankViews(snapshot, "secondary");
  const hasTurrets = turretViews(snapshot).length > 0;
  return (
    <section className="weapons-cockpit" aria-label={t("Cockpit d’armement")}>
      <div className="weapon-zone weapon-modes-zone">
        <CockpitPanel title="Modes et autorisations" definition={modes} snapshot={snapshot}>
          <GlobalModes snapshot={snapshot} />
        </CockpitPanel>
      </div>
      <div className="weapon-zone weapon-primary-zone">
        <CockpitPanel title="Racks primaires" definition={primary} snapshot={snapshot}>
          <BankRack family="primary" banks={primaryBanks} definition={primary} onInspect={onInspect} />
        </CockpitPanel>
      </div>
      <div className="weapon-zone weapon-selected-zone">
        <CockpitPanel title="Systèmes sélectionnés" definition={selected} snapshot={snapshot}>
          <div className="selected-weapons">
            <SelectedWeapon family="primary" snapshot={snapshot} definition={selected} onInspect={onInspect} />
            <SelectedWeapon family="secondary" snapshot={snapshot} definition={selected} onInspect={onInspect} />
          </div>
        </CockpitPanel>
      </div>
      <div className="weapon-zone weapon-secondary-zone">
        <CockpitPanel title="Racks secondaires" definition={secondary} snapshot={snapshot}>
          <BankRack family="secondary" banks={secondaryBanks} definition={secondary} onInspect={onInspect} />
        </CockpitPanel>
      </div>
      <div className="weapon-zone weapon-reserves-zone">
        <CockpitPanel title="Réserves et activité" definition={reserves} snapshot={snapshot}>
          <div className={`weapon-lower-strip ${hasTurrets ? "with-turrets" : ""}`}>
            <Reserves snapshot={snapshot} />
            <TurretSummary snapshot={snapshot} definition={turrets} onInspect={onInspect} />
          </div>
        </CockpitPanel>
      </div>
      <div className="weapon-zone weapon-future-zone">
        <CockpitPanel title="Extensions tactiques" definition={future} snapshot={snapshot}>
          <span />
        </CockpitPanel>
        <div className="weapon-future-list" aria-label={t("Données d’armement futures non disponibles")}>
          {[
            "PROGRESSION LOCK",
            "CIBLE · DISTANCE · LEAD",
            "SOLUTION DE TIR",
            "TIRS / IMPACTS CONFIRMÉS",
            "ANIMATION DE BANQUE"
          ].map((label) => <span key={label}><b>ND</b>{t(label)}</span>)}
        </div>
      </div>
    </section>
  );
}
