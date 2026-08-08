import {
  useEffect,
  useMemo,
  useRef,
  type CSSProperties,
  type MouseEvent,
  type ReactNode
} from "react";
import { resolveInstrument } from "./data";
import {
  lockViews,
  missileViews,
  prioritizedContacts,
  RADAR_SCOPE_GRID,
  contactVisibilityAlpha,
  hudAlertView,
  radarRangeDisplay,
  radarState,
  sensorLabels,
  subsystemNameForTarget,
  targetClassDisplayName,
  targetContact,
  targetDisplayName,
  targetHudTypeLabel,
  targetHudColor,
  targetRecordVersion,
  targetReferenceInvalid,
  targetState,
  threatState,
  type ContactView
} from "./tacticalSemantics";
import type {
  DashboardSnapshot,
  InspectionTarget,
  InstrumentDefinition
} from "./types";
import { useI18n } from "./i18n";

interface Props {
  definitions: InstrumentDefinition[];
  snapshot: DashboardSnapshot | null;
  onInspect: (target: InspectionTarget) => void;
}

function definitionById(definitions: InstrumentDefinition[], id: string) {
  const definition = definitions.find((candidate) => candidate.id === id);
  if (!definition) throw new Error(`instrument tactique manquant: ${id}`);
  return definition;
}

function finite(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function formatNumber(value: number | null, digits = 0): string {
  return value === null
    ? "—"
    : value.toLocaleString(document.documentElement.lang === "en" ? "en-US" : "fr-FR", { maximumFractionDigits: digits });
}

function formatDurationUs(value: unknown): string {
  const microseconds = finite(value);
  return microseconds === null ? "—" : `${formatNumber(microseconds / 1_000_000, 1)} s`;
}

function TacticalPanel({
  definition,
  snapshot,
  title,
  className,
  onInspect,
  children
}: {
  definition: InstrumentDefinition;
  snapshot: DashboardSnapshot | null;
  title: string;
  className: string;
  onInspect: Props["onInspect"];
  children: ReactNode;
}) {
  const { t } = useI18n();
  const resolved = resolveInstrument(definition, snapshot);
  const unavailable = !["live", "stale"].includes(resolved.state);
  const label: Record<string, string> = {
    nd: "ND",
    not_applicable: "—",
    invalid: "ERR",
    waiting: "EN ATTENTE"
  };
  return (
    <section className={`tactical-panel ${className} state-${resolved.state}`}>
      <button
        className="tactical-panel-header"
        onClick={() => onInspect({ definition })}
        aria-label={`${t("Inspecter")} ${t(title)}: ${resolved.state}`}
      >
        <span>{t(title)}</span>
        <i>{resolved.state === "live" ? "LIVE" : resolved.state === "stale" ? "STALE" : t(label[resolved.state])}</i>
      </button>
      <div className="tactical-panel-body">
        {unavailable ? (
          <div className="tactical-unavailable">
            <strong>{t(label[resolved.state])}</strong>
            <span>{t(resolved.reason ?? "")}</span>
          </div>
        ) : children}
      </div>
    </section>
  );
}

function SensorStrip({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const radar = radarState(snapshot);
  const labels = sensorLabels(snapshot);
  const player = String(snapshot?.playerEntityId ?? "");
  const ratioItem = snapshot?.derived[`entities.${player}.sensor_integrity_ratio`];
  const ratio = ratioItem?.available ? finite(ratioItem.value) : null;
  const current = finite(radar?.sensor_current_hits);
  const maximum = finite(radar?.sensor_max_hits);
  const range = radarRangeDisplay(snapshot);
  const emp = finite(radar?.emp_intensity);
  const invalid = labels.mode === null || labels.state === null || ratio === null;
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title="CAPTEURS · PORTÉE · ENVIRONNEMENT"
      className="tactical-sensors"
      onInspect={onInspect}
    >
      <div className={`sensor-strip ${invalid ? "invalid" : ""}`}>
        <div><span>{t("MODE RADAR")}</span><strong>{t(labels.mode ?? "ERR")}</strong></div>
        <div>
          <span>{t("PORTÉE")}</span>
          <strong aria-label={range.text === "∞" ? t("Portée infinie") : undefined}>
            {range.text ?? formatNumber(range.value, 0)}
          </strong>
          <small>{t(range.detail)}</small>
        </div>
        <div className="sensor-health">
          <span>{t("CAPTEURS")}</span>
          <strong>{t(labels.state ?? "ERR")}</strong>
          <div><i style={{ width: `${Math.max(0, Math.min(1, ratio ?? 0)) * 100}%` }} /></div>
          <small>{formatNumber(current, 1)} / {formatNumber(maximum, 1)} HP</small>
        </div>
        <div><span>AWACS</span><strong>{radar?.awacs_intensity === undefined ? "—" : formatNumber(finite(radar.awacs_intensity), 2)}</strong><small>{radar?.awacs_range === undefined ? t("non applicable") : `${t("PORTÉE").toLowerCase()} ${formatNumber(finite(radar.awacs_range))}`}</small></div>
        <div className={(emp ?? 0) > 0 ? "alert" : ""}>
          <span>EMP</span><strong>{radar?.emp_intensity === undefined ? "—" : formatNumber(emp, 2)}</strong>
          <small>{radar?.emp_remaining_us === undefined ? t("non applicable") : formatDurationUs(radar.emp_remaining_us)}</small>
        </div>
      </div>
    </TacticalPanel>
  );
}

type HitPoint = { contact: ContactView; x: number; y: number };

function drawContact(
  context: CanvasRenderingContext2D,
  contact: ContactView,
  x: number,
  y: number
) {
  // TARGET_STATE is sampled at flightHz and is the authority for the current
  // cockpit selection. The contact flag describes the older/newer radar sample
  // and remains available for inspection, but must not drive live emphasis.
  const target = contact.current;
  const bomb = (contact.flagBits & 0x20) !== 0;
  const threat = (contact.flagBits & 0x80) !== 0;
  const homing = (contact.flagBits & 0x40) !== 0;
  context.save();
  context.translate(x, y);
  context.globalAlpha = contactVisibilityAlpha(contact.visibilityCode);
  context.strokeStyle = contact.color.css;
  context.fillStyle = context.strokeStyle;
  context.lineWidth = target ? 2.6 : 1.5;
  context.setLineDash(contact.visibilityCode === 2 ? [3, 3] : []);
  const size = target ? 8 : threat || bomb ? 6 : 4.5;
  context.beginPath();
  if (bomb || homing) {
    context.moveTo(0, -size);
    context.lineTo(size, 0);
    context.lineTo(0, size);
    context.lineTo(-size, 0);
    context.closePath();
  } else if (contact.category === "VAISSEAU") {
    context.rect(-size, -size, size * 2, size * 2);
  } else if (contact.category === "ASTÉROÏDE" || contact.category === "DÉBRIS") {
    context.arc(0, 0, size, 0, Math.PI * 2);
  } else {
    context.moveTo(0, -size);
    context.lineTo(size, size);
    context.lineTo(-size, size);
    context.closePath();
  }
  context.stroke();
  if (target) {
    context.beginPath();
    context.moveTo(-12, -12); context.lineTo(-6, -12); context.lineTo(-12, -6);
    context.moveTo(12, -12); context.lineTo(6, -12); context.lineTo(12, -6);
    context.moveTo(-12, 12); context.lineTo(-6, 12); context.lineTo(-12, 6);
    context.moveTo(12, 12); context.lineTo(6, 12); context.lineTo(12, 6);
    context.stroke();
  }
  if (contact.elevationRad !== null && Math.abs(contact.elevationRad) > 0.04) {
    context.beginPath();
    const direction = contact.elevationRad > 0 ? -1 : 1;
    context.moveTo(-3, direction * 10);
    context.lineTo(0, direction * 13);
    context.lineTo(3, direction * 10);
    context.stroke();
  }
  if (contact.inRange === false) {
    context.fillRect(-1.5, -1.5, 3, 3);
  }
  context.restore();
}

function RadarScope({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const canvas = useRef<HTMLCanvasElement>(null);
  const hits = useRef<HitPoint[]>([]);
  const contacts = useMemo(() => prioritizedContacts(snapshot), [snapshot]);
  const visibleList = contacts.slice(0, 8);

  useEffect(() => {
    const element = canvas.current;
    if (!element) return;
    const render = () => {
      const bounds = element.getBoundingClientRect();
      const dpr = Math.max(window.devicePixelRatio || 1, 1);
      const width = Math.max(1, Math.round(bounds.width * dpr));
      const height = Math.max(1, Math.round(bounds.height * dpr));
      if (element.width !== width || element.height !== height) {
        element.width = width;
        element.height = height;
      }
      const context = element.getContext("2d");
      if (!context) return;
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.clearRect(0, 0, bounds.width, bounds.height);
      const centerX = bounds.width / 2;
      const centerY = bounds.height / 2;
      const radius = Math.max(20, Math.min(bounds.width, bounds.height) / 2 - 18);
      context.strokeStyle = "rgba(112, 228, 209, .24)";
      context.fillStyle = "rgba(7, 22, 28, .76)";
      context.lineWidth = 1;
      context.beginPath();
      context.arc(centerX, centerY, radius, 0, Math.PI * 2);
      context.fill();
      context.stroke();
      for (const fraction of RADAR_SCOPE_GRID.ringFractions) {
        context.beginPath();
        context.arc(centerX, centerY, radius * fraction, 0, Math.PI * 2);
        context.stroke();
      }
      context.save();
      context.translate(centerX, centerY);
      context.rotate(RADAR_SCOPE_GRID.axisRotationRad);
      const axisInnerRadius = radius * RADAR_SCOPE_GRID.axisInnerCutoutFraction;
      context.beginPath();
      context.moveTo(-radius, 0);
      context.lineTo(-axisInnerRadius, 0);
      context.moveTo(axisInnerRadius, 0);
      context.lineTo(radius, 0);
      context.moveTo(0, -radius);
      context.lineTo(0, -axisInnerRadius);
      context.moveTo(0, axisInnerRadius);
      context.lineTo(0, radius);
      context.stroke();
      context.restore();
      context.fillStyle = "#d5f8f1";
      context.font = "13px ui-monospace, monospace";
      context.textAlign = "center";
      // The visual axes rotate, while the cockpit directions keep their
      // stable cardinal positions so the labels remain immediately readable.
      context.fillText(t("HAUT"), centerX, centerY - radius + 12);
      context.fillText(t("BAS"), centerX, centerY + radius - 5);
      context.textAlign = "left";
      context.fillText(t("DROITE"), centerX + radius - 40, centerY - 6);
      context.textAlign = "right";
      context.fillText(t("GAUCHE"), centerX - radius + 40, centerY - 6);
      context.textAlign = "center";
      context.fillText(t("AVANT"), centerX, centerY + 20);
      context.beginPath();
      context.moveTo(centerX, centerY - 6);
      context.lineTo(centerX - 5, centerY + 5);
      context.lineTo(centerX + 5, centerY + 5);
      context.closePath();
      context.fill();
      hits.current = [];
      for (const contact of contacts) {
        if (!contact.scope || contact.invalid) continue;
        const x = centerX + contact.scope[0] * radius;
        const y = centerY + contact.scope[1] * radius;
        drawContact(context, contact, x, y);
        hits.current.push({ contact, x, y });
      }
    };
    render();
    const observer = new ResizeObserver(render);
    observer.observe(element);
    return () => observer.disconnect();
  }, [contacts, t]);

  const chooseAt = (event: MouseEvent<HTMLCanvasElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const x = event.clientX - bounds.left;
    const y = event.clientY - bounds.top;
    const nearest = hits.current
      .map((hit) => ({ hit, distance: Math.hypot(hit.x - x, hit.y - y) }))
      .filter((candidate) => candidate.distance <= 14)
      .sort((left, right) => left.distance - right.distance)[0];
    if (nearest) {
      onInspect({
        definition,
        kind: "radar-contact",
        entityId: nearest.hit.contact.id,
        record: nearest.hit.contact.record,
        title: nearest.hit.contact.name
      });
    }
  };

  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title={t(`SCOPE RADAR · ${contacts.length} PISTE${contacts.length > 1 ? "S" : ""}`)}
      className="tactical-scope"
      onInspect={onInspect}
    >
      <div className="scope-layout">
        <canvas
          ref={canvas}
          onClick={chooseAt}
          aria-label={`${t("SCOPE RADAR")} · ${contacts.length} ${t("CONTACTS")}`}
        />
        <div className="scope-contact-list" aria-label={t("Contacts radar prioritaires")}>
          {visibleList.map((contact) => (
            <button
              key={contact.id}
              className={`${contact.current ? "current" : ""} ${contact.invalid ? "invalid" : ""}`}
              data-color-provenance={contact.color.provenance}
              style={{ "--contact-color": contact.color.css } as CSSProperties}
              onClick={() => onInspect({
                definition,
                kind: "radar-contact",
                entityId: contact.id,
                record: contact.record,
                title: contact.name
              })}
            >
              <span>{contact.name}</span>
              <strong>{contact.invalid ? "ERR" : formatNumber(contact.distance)}</strong>
              {contact.typeLabel && (
                <small className="contact-type">{contact.typeLabel}</small>
              )}
              <small className="contact-status">
                {t(contact.category)} · {t(contact.visibility)}
              </small>
            </button>
          ))}
          {!contacts.length && <div className="tactical-empty">{t("AUCUN CONTACT AUTORISÉ")}</div>}
          {contacts.length > visibleList.length && (
            <button
              className="scope-more"
              onClick={() => onInspect({
                definition,
                kind: "radar-contact",
                title: t("Tous les contacts")
              })}
            >
              + {contacts.length - visibleList.length} {t("AUTRES")}
            </button>
          )}
        </div>
      </div>
    </TacticalPanel>
  );
}

const TRENDS: Record<number, string> = {
  0: "INCONNUE",
  1: "DIMINUE",
  2: "STABLE",
  3: "AUGMENTE"
};

function hudTrendSuffix(value: unknown, hide = false): string {
  if (hide) return "";
  if (Number(value) === 1) return "−";
  if (Number(value) === 3) return "+";
  return "";
}

function TargetPanel({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const target = targetState(snapshot);
  const contact = targetContact(snapshot);
  const player = String(snapshot?.playerEntityId ?? "");
  const distanceItem = snapshot?.derived[`entities.${player}.target.distance`];
  const distance = distanceItem?.available ? finite(distanceItem.value) : null;
  const hudSpeedItem = snapshot?.derived[`entities.${player}.target.hud_speed`];
  const hudSpeed = hudSpeedItem?.available ? finite(hudSpeedItem.value) : null;
  const targetVersion = targetRecordVersion(snapshot);
  const targetClass = targetClassDisplayName(snapshot);
  const targetHudLabel = targetHudTypeLabel(snapshot);
  const targetColor = targetHudColor(snapshot);
  const targetId = String(target?.current_target_entity_id ?? "0");
  const invalid = targetReferenceInvalid(snapshot);
  const targetSubsystem = subsystemNameForTarget(
    snapshot,
    target?.target_subsystem_id,
    target?.hud_target_subsystem_label
  );
  const lockSubsystem = subsystemNameForTarget(
    snapshot,
    target?.lock_subsystem_id,
    target?.hud_lock_subsystem_label
  );
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title="CIBLE SÉLECTIONNÉE"
      className="tactical-target"
      onInspect={onInspect}
    >
      {targetId === "0" ? (
        <div className="target-empty">— {t("AUCUNE CIBLE")}</div>
      ) : (
        <button
          className={`target-card ${invalid ? "invalid" : ""}`}
          data-color-provenance={targetColor.provenance}
          style={{ "--target-color": targetColor.css } as CSSProperties}
          onClick={() => onInspect({
            definition,
            kind: "target",
            entityId: targetId,
            record: target ?? undefined,
            title: targetDisplayName(snapshot)
          })}
        >
          <span className="target-kicker">{t(`ENTITÉ ${targetId}`)}</span>
          <strong>{invalid ? `ERR · ${t("RÉFÉRENCE INCONNUE")}` : targetDisplayName(snapshot)}</strong>
          {(targetClass ?? targetHudLabel) !== null && <span className="target-class">{targetClass ?? targetHudLabel}</span>}
          <div className="target-hud-readout" aria-label={t("Informations HUD FSO")}>
            <span>D : {formatNumber(distance)}{hudTrendSuffix(target?.distance_trend)}</span>
            <span>S : {formatNumber(hudSpeed)}{hudTrendSuffix(target?.speed_trend, (hudSpeed ?? 0) <= 1)}</span>
            {targetVersion !== null && targetVersion < 2 && <i>COMPAT. CAPTURE V1</i>}
          </div>
          <div className="target-primary-metrics">
            <div><span>{t("DISTANCE")}</span><strong>{formatNumber(distance)}</strong></div>
            <div><span>{t("RAPPROCHEMENT")}</span><strong>{formatNumber(contact?.closingSpeed ?? null, 1)}</strong></div>
            <div><span>{t("TEMPS CIBLE")}</span><strong>{formatDurationUs(target?.time_on_target_us)}</strong></div>
          </div>
          <div className="target-badges">
            <i>{t(contact?.category ?? "CONTACT")}</i>
            {target?.in_cone !== undefined && <i>{t(target.in_cone ? "DANS LE CÔNE" : "HORS CÔNE")}</i>}
            {target?.lead_world !== undefined && <i>LEAD · {t("BANQUE")} {String(target.lead_bank_id)}</i>}
          </div>
          <dl>
            <dt>{t("Tendance distance")}</dt><dd>{t(TRENDS[Number(target?.distance_trend)] ?? "—")}</dd>
            <dt>{t("Tendance vitesse")}</dt><dd>{t(TRENDS[Number(target?.speed_trend)] ?? "—")}</dd>
            <dt>{t("Sous-système ciblé")}</dt><dd>{targetSubsystem ?? "—"}</dd>
            <dt>{t("Sous-système lock")}</dt><dd>{lockSubsystem ?? "—"}</dd>
            <dt>{t("Cible précédente")}</dt><dd>{target?.previous_target_entity_id === undefined ? "—" : String(target.previous_target_entity_id)}</dd>
          </dl>
          {target?.last_stealth_position !== undefined && (
            <div className="stealth-memory">{t("PISTE FURTIVE MÉMORISÉE · OBSERVATION ANCIENNE")}</div>
          )}
        </button>
      )}
    </TacticalPanel>
  );
}

function LockPanel({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const locks = lockViews(snapshot);
  const visible = locks.slice(0, 8);
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title={t(`VERROUILLAGES · ${locks.length}`)}
      className="tactical-locks"
      onInspect={onInspect}
    >
      <div className="lock-list">
        {visible.map((lock) => (
          <button
            key={`${lock.targetId}-${lock.index}`}
            className={`${lock.locked ? "acquired" : lock.attempt ? "attempt" : ""} ${lock.invalid ? "invalid" : ""}`}
            onClick={() => onInspect({
              definition,
              kind: "lock-point",
              entityId: lock.targetId,
              record: lock.record,
              title: lock.targetName
            })}
          >
            <div>
              <span>{lock.targetName}</span>
              <strong>{lock.invalid ? "ERR" : t(lock.locked ? "ACQUIS" : lock.attempt ? "ACQUISITION" : "SANS TENTATIVE")}</strong>
            </div>
            <div className="lock-progress">
              <i style={{ width: `${Math.max(0, Math.min(1, lock.progress ?? 0)) * 100}%` }} />
            </div>
            <small>
              {t(lock.inCone ? "DANS LE CÔNE" : "HORS CÔNE")} ·
              {lock.remainingS === null ? ` ${t("progression")} —` : ` ${formatNumber(lock.remainingS, 1)} s`}
            </small>
          </button>
        ))}
        {!locks.length && <div className="tactical-empty">{t("AUCUN LOCK")}</div>}
        {locks.length > visible.length && (
          <button
            className="lock-more"
            onClick={() => onInspect({ definition, kind: "lock-list", title: t("Tous les verrouillages") })}
          >
            + {locks.length - visible.length} {t("AUTRES")}
          </button>
        )}
      </div>
    </TacticalPanel>
  );
}

function ThreatPanel({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const { t } = useI18n();
  const threat = threatState(snapshot);
  const alerts = hudAlertView(snapshot);
  const labels = sensorLabels(snapshot);
  const missiles = missileViews(snapshot);
  const visible = missiles.slice(0, 6);
  const level = Number(threat?.threat_level ?? 0);
  const warningKinds = [
    [1, "LAUNCH"],
    [2, "EVADED"],
    [3, "COLLISION"],
    [4, "BLAST"],
    [5, "ENGINE WASH"],
    [6, "EMP"],
    [7, "OTHER"]
  ] as const;
  const lampStyle = (period: number | null): CSSProperties =>
    period === null ? {} : ({ "--alert-period": `${period}ms` } as CSSProperties);
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title="MENACES ENTRANTES"
      className={`tactical-threats threat-level-${level}`}
      onInspect={onInspect}
    >
      <div className="threat-strip">
        <button
          className="threat-level"
          onClick={() => onInspect({ definition, record: threat ?? undefined })}
        >
          <span>{t("NIVEAU")}</span>
          <strong>{t(labels.threat ?? "ERR")}</strong>
          <small>{missiles.length} {t(missiles.length > 1 ? "MISSILES" : "MISSILE")}</small>
        </button>
        <div
          className="hud-alert-bank"
          data-alert-provenance={alerts.provenance}
          data-warning-instance={alerts.warning?.instanceId ?? "0"}
        >
          <button
            className={`hud-alert-lamp threat-lamp ${alerts.primaryFireActive ? "active" : ""}`}
            style={lampStyle(alerts.primaryBlinkMs)}
            aria-pressed={alerts.primaryFireActive}
            onClick={() => onInspect({ definition, record: alerts.record ?? undefined, title: "Menace tir primaire" })}
          >
            {t("TIR PRIMAIRE")}
          </button>
          <button
            className={`hud-alert-lamp threat-lamp lock-${alerts.missileLockState} ${alerts.missileLockState !== 0 ? "active" : ""}`}
            style={lampStyle(alerts.lockBlinkMs)}
            aria-pressed={alerts.missileLockState !== 0}
            onClick={() => onInspect({ definition, record: alerts.record ?? undefined, title: "Menace verrouillage missile" })}
          >
            {t(alerts.missileLockState === 2 ? "LOCK ACQUIS" : "TENTATIVE LOCK")}
          </button>
          <div className="hud-warning-lamps" aria-live="polite">
            {warningKinds.map(([kindCode, label]) => {
              const active = alerts.warning?.kindCode === kindCode;
              return (
                <button
                  key={label}
                  className={`hud-alert-lamp warning-lamp ${active ? "active" : ""}`}
                  aria-pressed={active}
                  title={active ? alerts.warning?.text : t(label)}
                  onClick={() => onInspect({
                    definition,
                    record: alerts.record ?? undefined,
                    title: active ? alerts.warning?.text : label
                  })}
                >
                  {t(label)}
                </button>
              );
            })}
          </div>
          <small className="hud-warning-text">
            {alerts.warning?.text ?? (alerts.provenance === "legacy-aggregated"
              ? t("CAPTURE HISTORIQUE · MENACE AGRÉGÉE")
              : alerts.provenance === "missing-authoritative"
                ? t("ALERTES HUD INDISPONIBLES")
                : t("AUCUN AVERTISSEMENT HUD"))}
          </small>
        </div>
        <div className="missile-rack">
          {visible.map((missile) => (
            <button
              key={missile.id}
              className={missile.invalid ? "invalid" : ""}
              onClick={() => onInspect({
                definition,
                kind: "incoming-missile",
                entityId: missile.id,
                record: missile.record,
                title: missile.name
              })}
            >
              <span>{missile.name}</span>
              <strong>{missile.invalid ? "ERR" : missile.ttcS === null ? "—" : `${formatNumber(missile.ttcS, 1)} s EST.`}</strong>
              <small>{t(missile.guidance)} · {formatNumber(missile.distance)} u</small>
            </button>
          ))}
          {!missiles.length && <div className="tactical-empty">{t("AUCUN MISSILE ENTRANT")}</div>}
          {missiles.length > visible.length && (
            <button
              className="missile-more"
              onClick={() => onInspect({ definition, kind: "missile-list", title: t("Tous les missiles") })}
            >
              + {missiles.length - visible.length}
            </button>
          )}
        </div>
      </div>
    </TacticalPanel>
  );
}

function FuturePanel({ definition }: { definition: InstrumentDefinition }) {
  const { t } = useI18n();
  const items = [
    "Brouillage global quantifié",
    "Historique global de visibilité",
    "Impact et probabilité autoritaires",
    "Prédiction du résultat d’un tir",
    "Coordonnées HUD projetées",
    "Vidéo de cible H.264"
  ];
  return (
    <section className="tactical-future" aria-label={t("Données tactiques non disponibles")}>
      <strong>ND</strong>
      {items.map((item) => <span key={item}>{t(item)}</span>)}
      <small>{t(definition.reason ?? "")}</small>
    </section>
  );
}

export function TacticalCockpit({ definitions, snapshot, onInspect }: Props) {
  const sensors = definitionById(definitions, "tactical-sensors");
  const scope = definitionById(definitions, "tactical-scope");
  const target = definitionById(definitions, "tactical-target");
  const locks = definitionById(definitions, "tactical-locks");
  const threats = definitionById(definitions, "tactical-threats");
  const future = definitionById(definitions, "tactical-future");
  return (
    <section className="tactical-cockpit">
      <SensorStrip snapshot={snapshot} definition={sensors} onInspect={onInspect} />
      <TargetPanel snapshot={snapshot} definition={target} onInspect={onInspect} />
      <RadarScope snapshot={snapshot} definition={scope} onInspect={onInspect} />
      <LockPanel snapshot={snapshot} definition={locks} onInspect={onInspect} />
      <ThreatPanel snapshot={snapshot} definition={threats} onInspect={onInspect} />
      <FuturePanel definition={future} />
    </section>
  );
}
