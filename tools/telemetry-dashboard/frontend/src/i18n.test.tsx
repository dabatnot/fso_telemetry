// @vitest-environment jsdom

import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";
import { LanguageProvider, LanguageSelector, translateText, useI18n } from "./i18n";

afterEach(() => {
  cleanup();
  window.localStorage.clear();
});

function Probe() {
  const { t } = useI18n();
  return <output>{t("CIBLE SÉLECTIONNÉE")}</output>;
}

describe("dashboard internationalization", () => {
  it("keeps French as the default and leaves authoritative unknown text untouched", () => {
    expect(translateText("CIBLE SÉLECTIONNÉE", "fr")).toBe("CIBLE SÉLECTIONNÉE");
    expect(translateText("GTF Myrmidon", "en")).toBe("GTF Myrmidon");
  });

  it("switches to English, updates the document language and persists the choice", () => {
    render(
      <LanguageProvider>
        <LanguageSelector />
        <Probe />
      </LanguageProvider>
    );
    expect(screen.getByText("CIBLE SÉLECTIONNÉE")).toBeTruthy();
    fireEvent.change(screen.getByRole("combobox"), { target: { value: "en" } });
    expect(screen.getByText("SELECTED TARGET")).toBeTruthy();
    expect(document.documentElement.lang).toBe("en");
    expect(window.localStorage.getItem("fso-telemetry-dashboard-language")).toBe("en");
  });
});
