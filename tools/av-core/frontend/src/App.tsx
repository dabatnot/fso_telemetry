import { useCallback, useEffect, useMemo, useState } from "react";
import { loadConfig, loadStatus, saveConfig } from "./api";
import { initialLanguage, LANGUAGE_OPTIONS, translate, type Language, type TranslationKey } from "./i18n";
import type {
  AvCoreConfig,
  AvCoreStatus,
  CautionState,
  ModuleKey,
  ModuleRole,
  RgbColor,
  ThresholdKey
} from "./types";
import { validateConfig } from "./validation";

type Page = "modules" | "alerts" | "lighting" | "system";
type Translate = (key: TranslationKey, variables?: Record<string, string>) => string;

const PAGES: Array<[Page, TranslationKey]> = [
  ["modules", "modules"],
  ["alerts", "alerts"],
  ["lighting", "lighting"],
  ["system", "system"]
];

const MODULES: Array<[ModuleKey, ModuleRole, string]> = [
  ["warnCtrl", "WARN_CTRL", "WARN CTRL"],
  ["threatProc", "THREAT_PROC", "THREAT PROC"],
  ["sensProc", "SENS_PROC", "SENS PROC"],
  ["instProc", "INST_PROC", "INST PROC"]
];

const THRESHOLDS: Array<[ThresholdKey, string]> = [
  ["engine", "ENG"],
  ["shield", "SHIELD"],
  ["hull", "HULL"],
  ["weaponEnergy", "WEP EN"],
  ["afterburnerFuel", "AB FUEL"],
  ["ammo", "AMMO"],
  ["subsystem", "SUBSYS"]
];

function currentPage(): Page {
  const candidate = window.location.hash.replace(/^#\/?/, "") as Page;
  return PAGES.some(([id]) => id === candidate) ? candidate : "modules";
}

function numberValue(event: React.ChangeEvent<HTMLInputElement>): number {
  return Number(event.target.value);
}

function rgbToHex(color: RgbColor): string {
  return `#${[color.r, color.g, color.b].map((component) => component.toString(16).padStart(2, "0")).join("")}`;
}

function hexToRgb(value: string): RgbColor {
  return {
    r: Number.parseInt(value.slice(1, 3), 16),
    g: Number.parseInt(value.slice(3, 5), 16),
    b: Number.parseInt(value.slice(5, 7), 16)
  };
}

function StatusPill({ label, state, t }: { label: string; state: string; t: Translate }) {
  return <div className={`status-pill state-${state.toLowerCase()}`}><span>{label}</span><strong>{displayState(state, t)}</strong></div>;
}

function displayState(state: string, t: Translate): string {
  const labels: Record<string, TranslationKey> = {
    UNAVAILABLE: "unavailable", LIVE: "live", STALE: "staleState", DISCONNECTED: "disconnected",
    ACTIVE: "activeState", CLEAR: "clearState", ONLINE: "online", DEGRADED: "degraded", OFFLINE: "offline",
    NONE: "noneState", ATTEMPT: "attempt", ACQUIRED: "acquired"
  };
  return labels[state] ? t(labels[state]) : state;
}

function SaveBar({ saving, errors, onSave, t }: { saving: boolean; errors: string[]; onSave: () => void; t: Translate }) {
  return (
    <div className="save-bar">
      <div>{errors.length > 0 && <span className="form-error">{errors[0]}</span>}</div>
      <button className="primary" onClick={onSave} disabled={saving}>{saving ? t("saving") : t("save")}</button>
    </div>
  );
}

function ModulesPage({ draft, status, setDraft, onSave, saving, errors, t }: PageProps) {
  return (
    <>
      <section className="module-grid">
        {MODULES.map(([key, role, label]) => {
          const live = status?.modules.find((module) => module.role === role);
          return (
            <article className="module-card" key={key}>
              <div className="card-heading"><div><span>{t("calculator")}</span><h2>{label}</h2></div><i className="state-dot" /></div>
              <dl>
                <div><dt>{t("state")}</dt><dd>{displayState(live?.state ?? "UNAVAILABLE", t)}</dd></div>
                <div><dt>{t("identifier")}</dt><dd>{t("defineLot3")}</dd></div>
                <div><dt>UID ESP32</dt><dd>{live?.uid ?? "—"}</dd></div>
                <div><dt>{t("firmware")}</dt><dd>{live?.firmwareVersion ?? "—"}</dd></div>
                <div><dt>{t("heartbeat")}</dt><dd>{live?.lastHeartbeatMs == null ? "—" : `${live.lastHeartbeatMs} ms`}</dd></div>
              </dl>
              <label className="toggle"><input type="checkbox" checked={draft.modules[key].installed} onChange={(event) => setDraft((previous) => ({ ...previous, modules: { ...previous.modules, [key]: { installed: event.target.checked } } }))} /><span />{t("installedCockpit")}</label>
            </article>
          );
        })}
      </section>
      <SaveBar saving={saving} errors={errors} onSave={onSave} t={t} />
    </>
  );
}

interface PageProps {
  draft: AvCoreConfig;
  status: AvCoreStatus | null;
  setDraft: (update: (previous: AvCoreConfig) => AvCoreConfig) => void;
  onSave: () => void;
  saving: boolean;
  errors: string[];
  t: Translate;
}

function CautionPreview({ state, value, t }: { state: CautionState; value: string | null; t: Translate }) {
  return <span className={`preview-state state-${state.toLowerCase()}`}><b>{displayState(state, t)}</b>{value && <em>{value}</em>}</span>;
}

function AlertsPage({ draft, status, setDraft, onSave, saving, errors, t }: PageProps) {
  const setThreshold = (key: ThresholdKey, field: "activateBelowPercent" | "clearAbovePercent", value: number) => {
    setDraft((previous) => ({ ...previous, alerts: { ...previous.alerts, [key]: { ...previous.alerts[key], [field]: value } } }));
  };
  const cockpit = status?.cockpit;
  const cautions = cockpit?.cautions;
  const cautionFor = (key: ThresholdKey) => cautions?.[key];
  const percentText = (value: number | null | undefined) => value == null ? null : `${value.toFixed(1)} %`;
  const warnings = cockpit?.warnings;
  const threat = cockpit?.threat;
  const warningItems = [
    ["FIRE", warnings?.fire], ["MISSILE", warnings?.missile], ["BLAST", warnings?.blast],
    ["COLLISION", warnings?.collision], ["EMP", warnings?.emp]
  ] as const;
  const sectors = [
    [7, "north-west", "↖"], [0, "north", "↑"], [1, "north-east", "↗"],
    [6, "west", "←"], [2, "east", "→"],
    [5, "south-west", "↙"], [4, "south", "↓"], [3, "south-east", "↘"]
  ] as const;
  return (
    <>
      <section className={`alert-preview-grid ${cockpit?.available ? "" : "is-unavailable"}`}>
        <article className="panel warning-preview">
          <div className="panel-heading"><div><span>{t("warnings")}</span><h2>MASTER WARNING</h2></div><span className={`master-lamp ${warnings?.master ? "active" : ""}`}>{warnings?.master ? t("activeState") : t("clearState")}</span></div>
          {!cockpit?.available && <p className="cockpit-unavailable">{t("cockpitUnavailable")}</p>}
          <div className="lamp-row">{warningItems.map(([label, active]) => <span key={label} className={`preview-lamp ${active ? "active" : ""}`}>{label}</span>)}</div>
          <div className="master-caution"><strong>MASTER CAUTION</strong><span className={cautions?.master ? "active" : ""}>{cautions?.master ? t("activeState") : t("clearState")}</span></div>
        </article>
        <article className="panel threat-preview">
          <div className="panel-heading"><div><span>{t("threatIndicator")}</span><h2>{t("incomingMissiles")} · {threat?.incomingMissileCount ?? 0}</h2></div></div>
          <div className="threat-grid" aria-label={t("threatIndicator")}>
            {sectors.map(([bit, position, arrow]) => <span key={bit} className={`${position} ${(threat?.sectorMask ?? 0) & (1 << bit) ? "active" : ""}`}>{arrow}</span>)}
            <strong className={`lock lock-${threat?.lockState.toLowerCase() ?? "none"}`}>LOCK<br /><small>{displayState(threat?.lockState ?? "NONE", t)}</small></strong>
          </div>
        </article>
      </section>
      <section className="panel">
        <div className="panel-heading"><div><span>{t("cautionPanel")}</span><h2>{t("thresholds")}</h2></div><p>{t("cockpitPreview")}</p></div>
        <div className="threshold-table" role="table">
          <div className="threshold-row table-head" role="row"><strong>{t("light")}</strong><strong>{t("activationBelow")}</strong><strong>{t("clearAbove")}</strong><strong>{t("preview")}</strong></div>
          {THRESHOLDS.map(([key, label]) => <div className="threshold-row" role="row" key={key}>
            <strong>{label}</strong>
            <label><span className="sr-only">{t("activation")} {label}</span><input aria-label={`${t("activation")} ${label}`} type="number" min="0" max="100" value={draft.alerts[key].activateBelowPercent} onChange={(event) => setThreshold(key, "activateBelowPercent", numberValue(event))} /><em>%</em></label>
            <label><span className="sr-only">{t("clear")} {label}</span><input aria-label={`${t("clear")} ${label}`} type="number" min="0" max="100" value={draft.alerts[key].clearAbovePercent} onChange={(event) => setThreshold(key, "clearAbovePercent", numberValue(event))} /><em>%</em></label>
            <CautionPreview state={cautionFor(key)?.state ?? "UNAVAILABLE"} value={percentText(cautionFor(key)?.valuePercent)} t={t} />
          </div>)}
          <div className="threshold-row" role="row">
            <strong>CM LOW</strong>
            <label><span className="sr-only">{t("absoluteThreshold")} CM LOW</span><input aria-label={`${t("absoluteThreshold")} CM LOW`} type="number" min="0" max="255" value={draft.alerts.countermeasures.activateAtOrBelowCount} onChange={(event) => setDraft((previous) => ({ ...previous, alerts: { ...previous.alerts, countermeasures: { ...previous.alerts.countermeasures, activateAtOrBelowCount: numberValue(event) } } }))} /><em>{t("units")}</em></label>
            <label><span className="sr-only">{t("relativeThreshold")} CM LOW</span><input aria-label={`${t("relativeThreshold")} CM LOW`} type="number" min="0" max="100" value={draft.alerts.countermeasures.activateAtOrBelowPercent} onChange={(event) => setDraft((previous) => ({ ...previous, alerts: { ...previous.alerts, countermeasures: { ...previous.alerts.countermeasures, activateAtOrBelowPercent: numberValue(event) } } }))} /><em>%</em></label>
            <CautionPreview state={cautions?.countermeasures.state ?? "UNAVAILABLE"} value={cautions?.countermeasures.valueCount == null ? null : `${cautions.countermeasures.valueCount} · ${percentText(cautions.countermeasures.valuePercent)}`} t={t} />
          </div>
          <div className="threshold-row readonly-row" role="row"><strong>SENS</strong><span>{t("sensorState")}</span><span>{t("notConfigurable")}</span><CautionPreview state={cautions?.sensor.state ?? "UNAVAILABLE"} value={cautions?.sensor.sensorState ? displayState(cautions.sensor.sensorState, t) : null} t={t} /></div>
        </div>
      </section>
      <SaveBar saving={saving} errors={errors} onSave={onSave} t={t} />
    </>
  );
}

function LightingPage({ draft, setDraft, onSave, saving, errors, t }: PageProps) {
  return (
    <>
      <section className="settings-grid">
        <article className="panel">
          <div className="panel-heading"><div><span>{t("colors")}</span><h2>{t("signaling")}</h2></div></div>
          <label className="field"><span>{t("warnings")}</span><div className="color-field"><input aria-label={t("warnings")} type="color" value={rgbToHex(draft.lighting.warningColor)} onChange={(event) => setDraft((previous) => ({ ...previous, lighting: { ...previous.lighting, warningColor: hexToRgb(event.target.value) } }))} /><code>{rgbToHex(draft.lighting.warningColor).toUpperCase()}</code></div></label>
          <label className="field"><span>{t("cautions")}</span><div className="color-field"><input aria-label={t("cautions")} type="color" value={rgbToHex(draft.lighting.cautionColor)} onChange={(event) => setDraft((previous) => ({ ...previous, lighting: { ...previous.lighting, cautionColor: hexToRgb(event.target.value) } }))} /><code>{rgbToHex(draft.lighting.cautionColor).toUpperCase()}</code></div></label>
          <label className="field"><span>{t("maxBrightness")}</span><div><input aria-label={t("maxBrightness")} type="number" min="0" max="100" value={draft.lighting.maxBrightnessPercent} onChange={(event) => setDraft((previous) => ({ ...previous, lighting: { ...previous.lighting, maxBrightnessPercent: numberValue(event) } }))} /><em>%</em></div></label>
        </article>
        <article className="panel">
          <div className="panel-heading"><div><span>{t("rates")}</span><h2>{t("flashes")}</h2></div></div>
          <label className="field"><span>{t("slow")}</span><div><input aria-label={t("slow")} type="number" min="0.25" max="10" step="0.25" value={draft.lighting.slowFlashHz} onChange={(event) => setDraft((previous) => ({ ...previous, lighting: { ...previous.lighting, slowFlashHz: numberValue(event) } }))} /><em>Hz</em></div></label>
          <label className="field"><span>{t("fast")}</span><div><input aria-label={t("fast")} type="number" min="0.25" max="10" step="0.25" value={draft.lighting.fastFlashHz} onChange={(event) => setDraft((previous) => ({ ...previous, lighting: { ...previous.lighting, fastFlashHz: numberValue(event) } }))} /><em>Hz</em></div></label>
        </article>
        <article className="panel test-panel">
          <div className="panel-heading"><div><span>{t("diagnostic")}</span><h2>{t("lampTest")}</h2></div><span className="unavailable">{t("canUnavailable")}</span></div>
          <div className="test-actions"><button disabled>{t("testAll")}</button><button disabled>{t("testWarn")}</button><button disabled>{t("testThreat")}</button></div>
          <p>{t("lampTestLot3")}</p>
        </article>
      </section>
      <SaveBar saving={saving} errors={errors} onSave={onSave} t={t} />
    </>
  );
}

function SystemPage({ draft, status, setDraft, onSave, saving, errors, t }: PageProps) {
  return (
    <>
      <section className="settings-grid">
        <article className="panel">
          <div className="panel-heading"><div><span>{t("fstlUdp")}</span><h2>{t("producer")}</h2></div><span className={`telemetry-state state-${status?.telemetry.state.toLowerCase() ?? "disconnected"}`}>{displayState(status?.telemetry.state ?? "DISCONNECTED", t)}</span></div>
          <dl className="system-list telemetry-details">
            <div><dt>{t("session")}</dt><dd>{status?.telemetry.sessionId ?? "—"}</dd></div>
            <div><dt>{t("lastData")}</dt><dd>{status?.telemetry.lastLiveAgeMs == null ? "—" : `${status.telemetry.lastLiveAgeMs} ms`}</dd></div>
            {status?.telemetry.error && <div className="connection-error"><dt>{t("connectionError")}</dt><dd>{status.telemetry.error}</dd></div>}
          </dl>
          <label className="field"><span>{t("host")}</span><input aria-label={`${t("host")} FS2Open`} type="text" value={draft.telemetry.host} onChange={(event) => setDraft((previous) => ({ ...previous, telemetry: { ...previous.telemetry, host: event.target.value } }))} /></label>
          <label className="field"><span>{t("port")}</span><input aria-label={`${t("port")} FS2Open`} type="number" min="1" max="65535" value={draft.telemetry.port} onChange={(event) => setDraft((previous) => ({ ...previous, telemetry: { ...previous.telemetry, port: numberValue(event) } }))} /></label>
          <label className="field"><span>{t("stale")}</span><div><input aria-label={t("stale")} type="number" min="1" max="60000" value={draft.telemetry.staleAfterMs} onChange={(event) => setDraft((previous) => ({ ...previous, telemetry: { ...previous.telemetry, staleAfterMs: numberValue(event) } }))} /><em>ms</em></div></label>
        </article>
        <article className="panel">
          <div className="panel-heading"><div><span>{t("avionicsBus")}</span><h2>SocketCAN</h2></div><span className="unavailable">{t("unavailable")}</span></div>
          <dl className="system-list"><div><dt>{t("interface")}</dt><dd>{status?.can.interface ?? "can0"}</dd></div><div><dt>{t("fixedBitrate")}</dt><dd>1 Mbit/s</dd></div><div><dt>{t("state")}</dt><dd>{displayState(status?.can.state ?? "UNAVAILABLE", t)}</dd></div></dl>
          <label className="field"><span>{t("moduleAbsence")}</span><div><input aria-label={t("moduleAbsence")} type="number" min="1" max="60000" value={draft.can.nodeTimeoutMs} onChange={(event) => setDraft((previous) => ({ ...previous, can: { ...previous.can, nodeTimeoutMs: numberValue(event) } }))} /><em>ms</em></div></label>
        </article>
        <article className="panel">
          <div className="panel-heading"><div><span>{t("localService")}</span><h2>AV CORE</h2></div><span className="version">v{status?.version ?? "—"}</span></div>
          <label className="field"><span>{t("httpPort")}</span><input aria-label={t("httpPort")} type="number" min="1" max="65535" value={draft.web.port} onChange={(event) => setDraft((previous) => ({ ...previous, web: { port: numberValue(event) } }))} /></label>
          <p>{t("portRestartHint")}</p>
        </article>
      </section>
      <SaveBar saving={saving} errors={errors} onSave={onSave} t={t} />
    </>
  );
}

export default function App() {
  const [language, setLanguage] = useState<Language>(initialLanguage);
  const [page, setPage] = useState<Page>(currentPage);
  const [draft, setDraft] = useState<AvCoreConfig | null>(null);
  const [status, setStatus] = useState<AvCoreStatus | null>(null);
  const [errors, setErrors] = useState<string[]>([]);
  const [notice, setNotice] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const t = useCallback<Translate>((key, variables) => translate(language, key, variables), [language]);

  useEffect(() => {
    localStorage.setItem("av-core-language", language);
    document.documentElement.lang = language;
  }, [language]);

  useEffect(() => {
    Promise.all([loadConfig(), loadStatus()])
      .then(([config, nextStatus]) => { setDraft(config); setStatus(nextStatus); })
      .catch((error) => setErrors([`${translate(language, "loadFailed")} : ${error instanceof Error ? error.message : String(error)}`]));
    const events = new EventSource("/api/events");
    events.addEventListener("status", (event) => setStatus(JSON.parse((event as MessageEvent).data) as AvCoreStatus));
    events.onerror = () => setNotice(translate(language, "streamInterrupted"));
    return () => events.close();
  }, []);

  useEffect(() => {
    const onHash = () => setPage(currentPage());
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);

  const moduleState = useMemo(() => Object.fromEntries(status?.modules.map((module) => [module.role, module.state]) ?? []), [status]);
  const updateDraft = useCallback((update: (previous: AvCoreConfig) => AvCoreConfig) => {
    setDraft((previous) => previous === null ? null : update(previous));
  }, []);

  const navigate = (next: Page) => {
    window.location.hash = `/${next}`;
    setPage(next);
  };

  const save = async () => {
    if (!draft) return;
    const validation = validateConfig(draft, language);
    setErrors(validation);
    if (validation.length) return;
    setSaving(true);
    try {
      const response = await saveConfig(draft);
      setDraft(response.config);
      setNotice(response.restartRequired ? t("savedRestart") : t("saved"));
    } catch (error) {
      setErrors([`${t("saveFailed")} : ${error instanceof Error ? error.message : String(error)}`]);
    } finally {
      setSaving(false);
    }
  };

  if (!draft) {
    return <div className="loading"><div className="brand-mark"><i /><i /><i /></div><strong>AV CORE</strong><span>{errors[0] ?? t("initialization")}</span></div>;
  }

  const changeLanguage = (next: Language) => {
    setLanguage(next);
    setErrors([]);
    setNotice(null);
  };
  const pageProps: PageProps = { draft, status, setDraft: updateDraft, onSave: save, saving, errors, t };
  return (
    <div className="app-shell">
      <header>
        <div className="brand"><div className="brand-mark"><i /><i /><i /></div><div><strong>AV CORE</strong><span>FSO // SIMPIT CONFIGURATION</span></div></div>
        <div className="status-strip">
          <StatusPill label={t("telemetry")} state={status?.telemetry.state ?? "DISCONNECTED"} t={t} />
          <StatusPill label="CAN" state={status?.can.state ?? "UNAVAILABLE"} t={t} />
          <StatusPill label="WARN CTRL" state={moduleState.WARN_CTRL ?? "UNAVAILABLE"} t={t} />
          <StatusPill label="THREAT PROC" state={moduleState.THREAT_PROC ?? "UNAVAILABLE"} t={t} />
        </div>
        <label className="language-selector">
          <span>{t("language")}</span>
          <select aria-label={t("language")} value={language} onChange={(event) => changeLanguage(event.target.value as Language)}>
            {LANGUAGE_OPTIONS.map((option) => <option key={option.code} value={option.code} lang={option.code}>{option.nativeName}</option>)}
          </select>
        </label>
      </header>
      {(status?.configuration.state === "ERROR" || status?.restartRequired) && <div className="warning-banner"><strong>{status.configuration.state === "ERROR" ? t("configError") : t("restartRequired")}</strong><span>{status.configuration.message ?? t("restartMessage")}</span></div>}
      {notice && <div className="notice" role="status" onClick={() => setNotice(null)}>{notice}</div>}
      <div className="body-grid">
        <nav aria-label="Configuration AV CORE">
          <span>{t("configuration")}</span>
          {PAGES.map(([id, label], index) => <button key={id} onClick={() => navigate(id)} className={page === id ? "active" : ""} aria-current={page === id ? "page" : undefined}><i>{String(index + 1).padStart(2, "0")}</i>{t(label)}</button>)}
        </nav>
        <main>
          <div className="page-heading"><div><span>{t("subsystem")}</span><h1>{t(PAGES.find(([id]) => id === page)?.[1] ?? "modules")}</h1></div><div className="config-state"><span>{t("configuration")}</span><strong>{status?.configuration.state ?? "—"}</strong></div></div>
          {page === "modules" && <ModulesPage {...pageProps} />}
          {page === "alerts" && <AlertsPage {...pageProps} />}
          {page === "lighting" && <LightingPage {...pageProps} />}
          {page === "system" && <SystemPage {...pageProps} />}
        </main>
      </div>
    </div>
  );
}
