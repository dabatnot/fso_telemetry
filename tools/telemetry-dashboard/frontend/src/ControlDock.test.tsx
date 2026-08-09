// @vitest-environment jsdom

import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ControlDock } from "./ControlDock";
import { LanguageProvider } from "./i18n";
import type { DashboardSnapshot } from "./types";

function snapshot(mode: "live" | "replay" = "live", status = "Live"): DashboardSnapshot {
  return {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-09T10:00:00Z",
    mode,
    connection: {
      status,
      recoveryState: "idle",
      host: "127.0.0.1",
      port: 42042,
      sessionId: mode === "live" ? "42" : "0",
      lastLiveObservedUtc: "2026-08-09T09:59:59Z",
      staleReason: status === "Stale" ? "silence" : null
    },
    session: {}, mission: {}, playerEntityId: null, records: {}, recordInstances: {},
    manifest: { id: 1, records: {} }, derived: {},
    transport: { synchronized: true, baseline: 12, deltaSequence: 8, manifestId: 1 },
    quality: { packets: 3, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: true, speed: 1, position: 4, packetCount: 20 }
  };
}

function renderDock(value = snapshot()) {
  const callbacks = {
    onToggleFreeze: vi.fn(), onOpenDiagnostics: vi.fn(), onResync: vi.fn(),
    onReconnect: vi.fn(), onToggleCapture: vi.fn(), onExport: vi.fn(),
    onReplayControl: vi.fn()
  };
  render(
    <LanguageProvider>
      <ControlDock snapshot={value} renderFps={60} displayFrozen={false} {...callbacks} />
    </LanguageProvider>
  );
  return callbacks;
}

describe("ControlDock", () => {
  beforeEach(() => window.localStorage.clear());
  afterEach(() => cleanup());

  it("opens one upward action menu at a time and closes it with Escape", () => {
    renderDock();
    fireEvent.click(screen.getByRole("button", { name: "SESSION" }));
    expect(screen.getByText("ÉTAT DE SESSION")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "AFFICHAGE" }));
    expect(screen.queryByText("ÉTAT DE SESSION")).toBeNull();
    expect(screen.getByText("OPTIONS D’AFFICHAGE")).toBeTruthy();
    fireEvent.keyDown(document, { key: "Escape" });
    expect(screen.queryByText("OPTIONS D’AFFICHAGE")).toBeNull();
  });

  it("confirms a hard reconnect only while the session is Live", () => {
    const callbacks = renderDock();
    fireEvent.click(screen.getByRole("button", { name: "SESSION" }));
    fireEvent.click(screen.getByRole("menuitem", { name: "RECONNECTER" }));
    expect(callbacks.onReconnect).not.toHaveBeenCalled();
    expect(screen.getByRole("alert")).toBeTruthy();
    fireEvent.click(screen.getByRole("menuitem", { name: "CONFIRMER" }));
    expect(callbacks.onReconnect).toHaveBeenCalledTimes(1);
  });

  it("reconnects immediately from Stale and exposes the data actions", () => {
    const callbacks = renderDock(snapshot("live", "Stale"));
    fireEvent.click(screen.getByRole("button", { name: "SESSION" }));
    fireEvent.click(screen.getByRole("menuitem", { name: "RECONNECTER" }));
    expect(callbacks.onReconnect).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole("button", { name: "DONNÉES" }));
    fireEvent.click(screen.getByRole("menuitem", { name: "● CAPTURER" }));
    expect(callbacks.onToggleCapture).toHaveBeenCalledTimes(1);
  });

  it("replaces the live session menu with replay controls", () => {
    const callbacks = renderDock(snapshot("replay"));
    expect(screen.queryByRole("button", { name: "SESSION" })).toBeNull();
    fireEvent.click(screen.getByRole("button", { name: "REPLAY" }));
    fireEvent.click(screen.getByRole("menuitem", { name: "PAUSE" }));
    expect(callbacks.onReplayControl).toHaveBeenCalledWith({ playing: false });
    fireEvent.click(screen.getByRole("button", { name: "AFFICHAGE" }));
    expect(screen.getByRole("menuitem", { name: "FIGER L’AFFICHAGE" }).hasAttribute("disabled")).toBe(true);
    fireEvent.click(screen.getByRole("button", { name: "DONNÉES" }));
    expect(screen.queryByRole("menuitem", { name: "● CAPTURER" })).toBeNull();
    expect(screen.getByRole("menuitem", { name: "EXPORTER" })).toBeTruthy();
  });
});
