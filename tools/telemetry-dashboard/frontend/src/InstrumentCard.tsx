import type { DashboardSnapshot, InstrumentDefinition, InstrumentValue } from "./types";
import { formatValue } from "./data";
import { useI18n } from "./i18n";

interface Props {
  definition: InstrumentDefinition;
  resolved: InstrumentValue;
  snapshot: DashboardSnapshot | null;
  onInspect: () => void;
}

function numericValue(value: unknown): number | null {
  const numeric = typeof value === "number" ? value : typeof value === "string" ? Number(value) : Number.NaN;
  return Number.isFinite(numeric) ? numeric : null;
}

function ratioFor(definition: InstrumentDefinition, value: unknown): number {
  const number = numericValue(value);
  if (number === null) return 0;
  const min = definition.min ?? 0;
  const max = definition.max ?? 1;
  return Math.max(0, Math.min(1, (number - min) / Math.max(0.000001, max - min)));
}

function Radial({ definition, value }: { definition: InstrumentDefinition; value: unknown }) {
  const ratio = ratioFor(definition, value);
  const dash = 238.76 * ratio;
  return (
    <div className="radial">
      <svg viewBox="0 0 100 100" aria-hidden="true">
        <circle className="radial-track" cx="50" cy="50" r="38" />
        <circle className="radial-value" cx="50" cy="50" r="38" strokeDasharray={`${dash} 238.76`} />
        <path className="radial-ticks" d="M50 6V12 M12 50H6 M94 50H88 M23 23L18 18 M77 23L82 18" />
      </svg>
      <div className="radial-readout">
        <strong>{formatValue(value)}</strong>
        <small>{definition.unit ?? ""}</small>
      </div>
    </div>
  );
}

function Bar({ definition, value }: { definition: InstrumentDefinition; value: unknown }) {
  const ratio = ratioFor(definition, value);
  return (
    <div className="linear-gauge">
      <div className="linear-scale"><i style={{ width: `${ratio * 100}%` }} /></div>
      <div className="linear-readout">
        <strong>{formatValue(value)}</strong><span>{definition.unit ?? ""}</span>
      </div>
    </div>
  );
}

function Vector({ value, unit }: { value: unknown; unit?: string }) {
  const vector = Array.isArray(value) ? value : [];
  return (
    <div className="vector-gauge">
      {["X", "Y", "Z"].map((axis, index) => (
        <div key={axis}><span>{axis}</span><strong>{formatValue(vector[index])}</strong><small>{unit}</small></div>
      ))}
    </div>
  );
}

function Segments({ value }: { value: unknown }) {
  const { t } = useI18n();
  const values = Array.isArray(value) ? value : [];
  if (!values.length) return <div className="empty-applicable">{t("AUCUN SEGMENT")}</div>;
  const max = Math.max(...values.map((item) => Number(item) || 0), 1);
  return (
    <div className="segment-array">
      {values.map((item, index) => (
        <div key={index} title={`Segment ${index + 1}: ${formatValue(item)}`}>
          <i style={{ height: `${Math.max(4, ((Number(item) || 0) / max) * 100)}%` }} />
          <span>{index + 1}</span>
        </div>
      ))}
    </div>
  );
}

function Attitude({ value }: { value: unknown }) {
  const values = Array.isArray(value) ? value.flat(2).map(Number) : [];
  const tilt = Number.isFinite(values[0]) ? values[0] * 20 : 0;
  return (
    <div className="attitude">
      <div className="attitude-horizon" style={{ transform: `rotate(${tilt}deg)` }}>
        <i /><span />
      </div>
      <div className="attitude-craft">◇</div>
    </div>
  );
}

function ObjectList({ value }: { value: unknown }) {
  const { t } = useI18n();
  const rows = Array.isArray(value) ? value : value && typeof value === "object" ? Object.values(value) : [];
  if (!rows.length) return <div className="empty-applicable">{t("AUCUN")}</div>;
  return (
    <div className="object-list">
      {rows.slice(0, 12).map((row, index) => {
        const record = row as Record<string, unknown>;
        const name = String(
          record.internal_name_override ??
          (record.name_overrides as Record<string, unknown> | undefined)?.hud_name_override ??
          record.subsystem_id ??
          record.bank_id ??
          record.event_id ??
          `#${index + 1}`
        );
        const current = Number(record.current_hits ?? (record.ammunition as Record<string, unknown> | undefined)?.current);
        const maximum = Number(record.max_hits ?? (record.ammunition as Record<string, unknown> | undefined)?.initial);
        const ratio = Number.isFinite(current) && Number.isFinite(maximum) && maximum > 0 ? current / maximum : null;
        return (
          <div className="object-row" key={`${name}-${index}`}>
            <span>{name}</span>
            {ratio === null ? <strong>{t(formatValue(record.phase ?? record.cooldown_remaining_us ?? "ACTIF"))}</strong> :
              <div className="microbar"><i style={{ width: `${Math.max(0, Math.min(1, ratio)) * 100}%` }} /></div>}
          </div>
        );
      })}
      {rows.length > 12 && <div className="more-rows">+ {t(`${rows.length - 12} éléments`)}</div>}
    </div>
  );
}

function Diagnostic({ snapshot }: { snapshot: DashboardSnapshot | null }) {
  const { t } = useI18n();
  const channels = snapshot?.quality.channels ?? [];
  return (
    <div className="quality-table">
      <div className="quality-head"><span>{t("CANAL")}</span><span>HZ</span><span>{t("ÂGE")}</span><span>JITTER</span></div>
      {channels.slice(0, 14).map((channel) => (
        <div className="quality-row" key={channel.identity}>
          <span title={channel.identity}>{channel.recordName.replace("_STATE", "")}</span>
          <strong>{channel.observedHz?.toFixed(1) ?? "—"}<small>/{channel.configuredHz}</small></strong>
          <span>{channel.ageUs === null ? "ND" : `${(channel.ageUs / 1000).toFixed(0)} ms`}</span>
          <span>{channel.jitterUs === null ? "—" : `${(channel.jitterUs / 1000).toFixed(1)} ms`}</span>
        </div>
      ))}
      {!channels.length && <div className="empty-applicable">{t("AUCUN ÉCHANTILLON")}</div>}
    </div>
  );
}

function LiveInstrument({ definition, resolved, snapshot }: Omit<Props, "onInspect">) {
  if (definition.component === "radial") return <Radial definition={definition} value={resolved.value} />;
  if (definition.component === "bar") return <Bar definition={definition} value={resolved.value} />;
  if (definition.component === "vector") return <Vector value={resolved.value} unit={definition.unit} />;
  if (definition.component === "segments") return <Segments value={resolved.value} />;
  if (definition.component === "list" || definition.component === "timeline") return <ObjectList value={resolved.value} />;
  if (definition.component === "diagram") {
    return Array.isArray(resolved.value) && resolved.value.length >= 9
      ? <Attitude value={resolved.value} />
      : <ObjectList value={resolved.value} />;
  }
  if (definition.component === "diagnostic") return <Diagnostic snapshot={snapshot} />;
  return <div className="status-readout"><strong>{formatValue(resolved.value)}</strong><small>{definition.unit}</small></div>;
}

export function InstrumentCard({ definition, resolved, snapshot, onInspect }: Props) {
  const { t } = useI18n();
  const unavailable = resolved.state !== "live" && resolved.state !== "stale";
  const labels: Record<string, string> = {
    nd: "ND",
    not_applicable: "—",
    invalid: "ERR",
    waiting: "EN ATTENTE"
  };
  return (
    <button
      className={`instrument instrument-${definition.component} state-${resolved.state}`}
      onClick={onInspect}
      aria-label={`${t(definition.label)}: ${unavailable ? t(labels[resolved.state]) : formatValue(resolved.value)}`}
    >
      <header>
        <span>{t(definition.label)}</span>
        <i>{resolved.state === "live" ? "LIVE" : resolved.state === "stale" ? "STALE" : t(labels[resolved.state])}</i>
      </header>
      <div className="instrument-body">
        {unavailable ? (
          <div className="unavailable">
            <strong>{t(labels[resolved.state])}</strong>
            <span>{t(resolved.reason ?? "")}</span>
          </div>
        ) : (
          <LiveInstrument definition={definition} resolved={resolved} snapshot={snapshot} />
        )}
      </div>
      <footer>
        <span>{resolved.observedHz === undefined ? "— Hz" : `${resolved.observedHz.toFixed(1)} Hz`}</span>
        <span>{resolved.ageUs === undefined ? `${t("ÂGE").toLowerCase()} —` : `${(resolved.ageUs / 1000).toFixed(0)} ms`}</span>
      </footer>
    </button>
  );
}
