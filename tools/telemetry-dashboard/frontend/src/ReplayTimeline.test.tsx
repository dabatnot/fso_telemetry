// @vitest-environment jsdom

import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "./i18n";
import { ReplayTimeline } from "./ReplayTimeline";
import type { DashboardSnapshot } from "./types";

function replaySnapshot(): DashboardSnapshot {
  return {
    schema: "DashboardSnapshotV1", publishedAtUtc: "2026-08-09T10:00:00Z", mode: "replay",
    connection: { status: "Live", host: "127.0.0.1", port: 42042, sessionId: "9", lastLiveObservedUtc: null, staleReason: null },
    session: {}, mission: {}, playerEntityId: null, records: {}, recordInstances: {},
    manifest: { id: 0, records: {} }, derived: {},
    transport: { synchronized: true, baseline: 1, deltaSequence: 2, manifestId: 0 },
    quality: { packets: 4, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: {
      path: "test.fstlcap", captureId: "capture", playing: false, speed: 1, position: 2,
      packetCount: 4, positionUs: 2_000_000, durationUs: 10_000_000,
      ranges: [{ id: "range-1", name: "Missile launch", sessionIndex: 0, startUs: 1_000_000, endUs: 3_000_000 }],
      sessionBoundaries: [{ sessionIndex: 0, sessionId: "9", startUs: 0, endUs: 10_000_000, reason: "welcome" }]
    }
  };
}

describe("ReplayTimeline", () => {
  beforeEach(() => localStorage.clear());
  afterEach(() => cleanup());

  it("previews a seek locally and commits it only on release", () => {
    const onControl = vi.fn();
    render(<LanguageProvider><ReplayTimeline snapshot={replaySnapshot()} onControl={onControl} onPreview={vi.fn()} onCreateRange={vi.fn()} onUpdateRange={vi.fn()} onDeleteRange={vi.fn()} /></LanguageProvider>);
    const slider = screen.getByRole("slider", { name: "Position du replay" });
    fireEvent.pointerDown(slider);
    fireEvent.change(slider, { target: { value: "4500000" } });
    expect(onControl).not.toHaveBeenCalled();
    expect(screen.getByText(/00:00:04\.500/)).toBeTruthy();
    fireEvent.pointerUp(slider);
    expect(onControl).toHaveBeenCalledWith({ positionUs: 4_500_000 });
  });

  it("shows named marker tooltips and filters the saved marker list", () => {
    render(<LanguageProvider><ReplayTimeline snapshot={replaySnapshot()} onControl={vi.fn()} onPreview={vi.fn()} onCreateRange={vi.fn()} onUpdateRange={vi.fn()} onDeleteRange={vi.fn()} /></LanguageProvider>);
    expect(screen.getByTitle("Missile launch")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "MARQUEURS 1" }));
    expect(screen.getByText("Missile launch")).toBeTruthy();
    fireEvent.change(screen.getByPlaceholderText("Rechercher un marqueur"), { target: { value: "unknown" } });
    expect(screen.queryByText("Missile launch")).toBeNull();
  });

  it("moves either marker boundary to the current playhead", () => {
    const onUpdateRange = vi.fn();
    render(<LanguageProvider><ReplayTimeline snapshot={replaySnapshot()} onControl={vi.fn()} onPreview={vi.fn()} onCreateRange={vi.fn()} onUpdateRange={onUpdateRange} onDeleteRange={vi.fn()} /></LanguageProvider>);
    fireEvent.click(screen.getByRole("button", { name: "MARQUEURS 1" }));

    fireEvent.click(screen.getByRole("button", { name: /POINT D’ENTRÉE.*Missile launch/ }));
    expect(onUpdateRange).toHaveBeenCalledWith(expect.objectContaining({
      id: "range-1",
      startUs: 2_000_000,
      endUs: 3_000_000
    }));

    fireEvent.click(screen.getByRole("button", { name: /POINT DE SORTIE.*Missile launch/ }));
    expect(onUpdateRange).toHaveBeenCalledWith(expect.objectContaining({
      id: "range-1",
      startUs: 1_000_000,
      endUs: 2_000_000
    }));
  });
});
