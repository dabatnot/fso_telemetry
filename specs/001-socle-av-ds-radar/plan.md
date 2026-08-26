# Implementation Plan: Socle AV DS et Radar unique

**Branch**: `codex/phase5-communication-assets` | **Date**: 2026-08-26 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/001-socle-av-ds-radar/spec.md`

## Summary

Transformer en place le client Qt 6 situé dans `tools/radar` en application Windows
`AV DS — AV Display System`, livrée comme `av-ds.exe`. Le client FSTL, le modèle
Radar immuable, le renderer, les ressources et les réglages existants restent la base
fonctionnelle. Une coque AV DS minimale introduit une unité `MFD-L`, un catalogue
statique contenant uniquement `RADAR`, une page active et un présentateur de messages
global à l'application. Les états de liaison quittent ainsi `RadarWidget` et sont
superposés par l'unité d'affichage, sans changement du protocole ni du producteur
FS2Open.

## Technical Context

**Language/Version**: C++20, PowerShell pour le packaging Windows

**Primary Dependencies**: Qt 6.2+ (`Core`, `Gui`, `Widgets`, `Network`, `Svg`,
`Test`), bibliothèque interne `fstl_protocol`, CMake 3.22+

**Storage**: `QSettings` local Windows ; conservation du namespace historique
`FS2Open/FsoSimpitRadar` pour réutiliser hôte, port, options d'affichage et géométrie

**Testing**: Qt Test via CTest (`av_ds_shell_tests`, `application_settings_tests`,
`radar_model_tests`, `radar_widget_tests`, `radar_client_tests`,
`radar_manifest_tests`) et contrôle manuel en jeu

**Target Platform**: Windows, application desktop Qt native maximisable

**Project Type**: Application desktop CMake autonome dans le monorepo FS2Open

**Performance Goals**: Conserver les seuils existants de détection du flux périmé à
1 seconde et de reconnexion à 2 secondes de silence réseau. Aucun nouvel objectif de
débit ou de fréquence de rendu n'est introduit dans cette phase

**Constraints**: Une seule fenêtre et une seule unité `MFD-L` ; une seule page
`RADAR` ; lecture seule vis-à-vis de la simulation ; connexion directe FSTL avec le
profil `CockpitSensors` ; aucun plugin, script de page, AV CORE, matériel MFD, seconde
plateforme ou donnée `TrustedFullState`

**Scale/Scope**: Un processus, une session UDP, une unité d'affichage, une page et un
état Radar partagé immuable ; transformation limitée à `tools/radar`, ses tests, sa
documentation et son packaging Windows

## Constitution Check

*Compliance assessment evaluated before Phase 0 and re-checked after Phase 1 design.
This assessment is advisory and does not control phase eligibility or ordering.*

- **Cockpit uniquement** : ALIGNED — le design conserve le profil producteur
  `CockpitSensors` et n'ajoute aucun record ni accès à un état caché.
- **Pas de `TrustedFullState`** : ALIGNED — aucun profil omniscient ou mécanisme de
  compatibilité n'est introduit.
- **Consommateur passif et facultatif** : ALIGNED — AV DS reste une liaison directe en
  lecture seule dont l'absence ne change pas FS2Open.
- **Pas de workflow de preuve ou de blocage** : ALIGNED — la validation repose sur les
  tests automatisés existants et un contrôle manuel en jeu.
- **Ordre dirigé par l'utilisateur** : ALIGNED — les dépendances décrivent seulement
  l'ordre technique recommandé et ne bloquent pas le démarrage d'une autre phase.
- **Simplicité proportionnée** : ALIGNED — la coque contient seulement les concepts
  explicitement demandés ; aucun système dynamique de plugins ou de pages n'est prévu.

Réévaluation post-design : aucun conflit identifié. Le modèle de données et le contrat
restent internes à AV DS, et le contrat FSTL existant ne change pas.

## Project Structure

### Documentation (this feature)

```text
specs/001-socle-av-ds-radar/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── av-ds-application.md
└── tasks.md                  # créé ultérieurement par speckit-tasks
```

### Source Code (repository root)

```text
tools/radar/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp
│   ├── main_window.h/.cpp           # fenêtre AV DS et composition MFD-L
│   ├── application_settings.h/.cpp  # accès explicite au namespace historique
│   ├── display_unit.h/.cpp          # unité, catalogue et sélection de page
│   ├── av_ds_message.h/.cpp         # état global issu du statut client
│   ├── av_ds_message_overlay.h/.cpp # rendu transversal des messages
│   ├── radar_client.h/.cpp          # session FSTL conservée
│   ├── radar_image.h/.cpp           # projection Radar immuable conservée
│   ├── radar_widget.h/.cpp          # contenu de la page RADAR uniquement
│   └── settings_dialog.h/.cpp
├── tests/
│   ├── av_ds_test_support.h
│   ├── av_ds_shell_tests.cpp
│   ├── application_settings_tests.cpp
│   ├── radar_client_tests.cpp
│   ├── radar_model_tests.cpp
│   ├── radar_widget_tests.cpp
│   └── radar_manifest_tests.cpp
└── packaging/
    └── deploy-windows.ps1

code/telemetry/protocol/              # contrat FSTL réutilisé sans modification
```

**Structure Decision**: Faire évoluer le projet `tools/radar` en place évite un second
client, conserve ses composants testés et permet au packaging Windows AV DS de remplacer
le produit Radar autonome. Les nouveaux fichiers correspondent uniquement à la coque
d'affichage, à l'accès compatible aux réglages et au déplacement des messages hors de
la page Radar. La transformation de livraison concerne Windows ; hors Windows, le nom
de sortie historique `FsoSimpitRadar` est conservé afin de ne pas casser le script Linux
existant, qui n'est ni migré ni supprimé dans cette phase.

## Complexity Tracking

Aucune violation à justifier.
