import { useEffect, useMemo, useState } from "react";
import { useI18n } from "./i18n";
import type { CaptureRange, DashboardSnapshot } from "./types";

interface ReplayTimelineProps {
  snapshot: DashboardSnapshot;
  onControl: (control: Record<string, unknown>) => void;
  onPreview: (positionUs: number) => void;
  onCreateRange: (name: string, startUs: number, endUs: number) => void;
  onUpdateRange: (range: CaptureRange) => void;
  onDeleteRange: (rangeId: string) => void;
}

function duration(valueUs: number): string {
  const totalSeconds = Math.max(0, Math.floor(valueUs / 1_000_000));
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  const micros = Math.max(0, valueUs % 1_000_000);
  return `${hours.toString().padStart(2, "0")}:${minutes.toString().padStart(2, "0")}:${seconds
    .toString()
    .padStart(2, "0")}.${Math.floor(micros / 1000).toString().padStart(3, "0")}`;
}

export function ReplayTimeline({
  snapshot,
  onControl,
  onPreview,
  onCreateRange,
  onUpdateRange,
  onDeleteRange
}: ReplayTimelineProps) {
  const { t } = useI18n();
  const replay = snapshot.replay;
  const max = Math.max(1, replay.durationUs ?? 0);
  const position = Math.min(max, replay.positionUs ?? 0);
  const ranges = replay.ranges ?? [];
  const [collapsed, setCollapsed] = useState(() => localStorage.getItem("fstl.timeline.collapsed") === "1");
  const [draftUs, setDraftUs] = useState(position);
  const [dragging, setDragging] = useState(false);
  const [drawer, setDrawer] = useState(false);
  const [query, setQuery] = useState("");
  const [inUs, setInUs] = useState<number | null>(null);
  const [outUs, setOutUs] = useState<number | null>(null);
  const [rangeName, setRangeName] = useState("");

  useEffect(() => {
    if (!dragging) setDraftUs(position);
  }, [position, dragging]);

  const filtered = useMemo(() => {
    const needle = query.trim().toLocaleLowerCase();
    return ranges.filter((range) => !needle || range.name.toLocaleLowerCase().includes(needle));
  }, [query, ranges]);

  const commitSeek = () => {
    setDragging(false);
    onControl({ positionUs: draftUs });
  };

  const toggleCollapsed = () => {
    setCollapsed((current) => {
      localStorage.setItem("fstl.timeline.collapsed", current ? "0" : "1");
      return !current;
    });
  };

  const createRange = () => {
    if (!rangeName.trim() || inUs === null || outUs === null || inUs >= outUs) return;
    onCreateRange(rangeName.trim(), inUs, outUs);
    setRangeName("");
    setInUs(null);
    setOutUs(null);
  };

  return (
    <section className={`replay-timeline ${collapsed ? "collapsed" : ""}`} aria-label={t("Timeline du replay")}>
      <header>
        <button onClick={toggleCollapsed} aria-expanded={!collapsed}>{collapsed ? "▴" : "▾"}</button>
        <strong>{t("TIMELINE")}</strong>
        <span>{duration(draftUs)} / {duration(max)}</span>
        <button onClick={() => setDrawer((value) => !value)} aria-expanded={drawer}>{t("MARQUEURS")} {ranges.length}</button>
      </header>
      {!collapsed && (
        <div className="timeline-body">
          <div className="timeline-track-wrap">
            <div className="timeline-overlays" aria-hidden="true">
              {(replay.sessionBoundaries ?? []).map((boundary) => (
                <i
                  className="timeline-session-boundary"
                  key={`${boundary.sessionIndex}-${boundary.startUs}`}
                  style={{ left: `${(boundary.startUs / max) * 100}%` }}
                  title={`${t("Session")} ${boundary.sessionIndex + 1}`}
                />
              ))}
              {ranges.map((range) => (
                <button
                  className={`timeline-range ${replay.activeRangeId === range.id ? "active" : ""}`}
                  key={range.id}
                  style={{ left: `${(range.startUs / max) * 100}%`, width: `${((range.endUs - range.startUs) / max) * 100}%` }}
                  title={range.name}
                  aria-label={range.name}
                  onClick={() => onControl({ activeRangeId: range.id, setActiveRange: true })}
                ><i /></button>
              ))}
            </div>
            <input
              type="range"
              min={0}
              max={max}
              step={1}
              value={draftUs}
              aria-label={t("Position du replay")}
              onPointerDown={() => setDragging(true)}
              onChange={(event) => {
                const next = Number(event.currentTarget.value);
                setDraftUs(next);
                onPreview(next);
              }}
              onPointerUp={commitSeek}
              onKeyUp={(event) => {
                if (["ArrowLeft", "ArrowRight", "Home", "End", "PageUp", "PageDown", "Enter"].includes(event.key)) commitSeek();
              }}
            />
          </div>
          <div className="timeline-actions">
            <button onClick={() => onControl({ playing: !replay.playing })}>{replay.playing ? t("PAUSE") : t("LECTURE")}</button>
            <button onClick={() => setInUs(draftUs)}>{t("POINT D’ENTRÉE")}</button>
            <button onClick={() => setOutUs(draftUs)}>{t("POINT DE SORTIE")}</button>
            <span>IN {inUs === null ? "—" : duration(inUs)}</span>
            <span>OUT {outUs === null ? "—" : duration(outUs)}</span>
          </div>
        </div>
      )}
      {drawer && !collapsed && (
        <aside className="marker-drawer" aria-label={t("Marqueurs enregistrés")}>
          <div className="marker-create">
            <input value={rangeName} onChange={(event) => setRangeName(event.currentTarget.value)} placeholder={t("Nom du marqueur")} maxLength={128} />
            <button disabled={!rangeName.trim() || inUs === null || outUs === null || inUs >= outUs} onClick={createRange}>{t("AJOUTER")}</button>
          </div>
          <input type="search" value={query} onChange={(event) => setQuery(event.currentTarget.value)} placeholder={t("Rechercher un marqueur")} />
          <div className="marker-list">
            {filtered.map((range) => (
              <div key={range.id} className={replay.activeRangeId === range.id ? "active" : ""}>
                <button onClick={() => onControl({ activeRangeId: range.id, setActiveRange: true })}>
                  <strong>{range.name}</strong><span>{duration(range.startUs)} — {duration(range.endUs)}</span>
                </button>
                <button aria-label={t("Renommer")} onClick={() => {
                  const name = window.prompt(t("Nom du marqueur"), range.name)?.trim();
                  if (name) onUpdateRange({ ...range, name });
                }}>✎</button>
                <button
                  aria-label={`${t("POINT D’ENTRÉE")} · ${range.name}`}
                  disabled={draftUs >= range.endUs}
                  onClick={() => onUpdateRange({ ...range, startUs: draftUs })}
                >IN←</button>
                <button
                  aria-label={`${t("POINT DE SORTIE")} · ${range.name}`}
                  disabled={draftUs <= range.startUs}
                  onClick={() => onUpdateRange({ ...range, endUs: draftUs })}
                >OUT←</button>
                <button aria-label={t("Supprimer")} onClick={() => {
                  if (window.confirm(t("Supprimer ce marqueur ?"))) onDeleteRange(range.id);
                }}>×</button>
              </div>
            ))}
          </div>
        </aside>
      )}
    </section>
  );
}
