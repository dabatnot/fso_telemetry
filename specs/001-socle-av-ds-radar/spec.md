# Feature Specification: Socle AV DS et Radar unique

**Feature Branch**: `codex/phase5-communication-assets`

**Created**: 2026-08-26

**Status**: Draft

**Input**: User description: "Phase 1 — Socle AV DS et Radar unique"

## Clarifications

### Session 2026-08-26

- Q: Que doit afficher AV DS lorsque la configuration de connexion est absente ou invalide ? → A: AV DS affiche les messages existants du Radar au moyen d'un mécanisme commun à toutes ses unités d'affichage et pages ; la page `RADAR` ne les affiche plus elle-même.
- Q: Lorsqu'un message AV DS est actif, doit-il apparaître simultanément sur toutes les unités d'affichage, quelle que soit leur page active ? → A: Oui, tout message actif apparaît simultanément sur toutes les unités d'affichage.
- Q: AV DS doit-il remplacer le client Radar autonome dans la livraison Windows, ou les deux produits doivent-ils rester installables côte à côte ? → A: AV DS remplace le client Radar autonome tout en réutilisant sa configuration.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Utiliser le Radar sous l'identité AV DS (Priority: P1)

En tant que pilote de simpit, je lance AV DS sur un PC Windows et retrouve la page `RADAR` existante dans une fenêtre unique représentant `MFD-L`, afin de continuer à utiliser mon radar distant sous la nouvelle identité du produit.

**Why this priority**: Cette transformation utilisable constitue le livrable autonome de la phase. Sans elle, il n'existe pas encore de socle AV DS sur lequel ajouter de futures pages ou unités d'affichage.

**Independent Test**: Installer ou lancer l'application sur Windows, vérifier son identité AV DS, ouvrir une mission et comparer la vue affichée au comportement de référence du client Radar avant transformation.

**Acceptance Scenarios**:

1. **Given** une installation fonctionnelle de l'ancien client Radar et des paramètres de connexion valides, **When** le pilote lance le produit transformé, **Then** l'application et sa fenêtre s'identifient comme AV DS et non comme un produit Radar autonome.
2. **Given** AV DS connecté à une mission en cours, **When** des contacts autorisés apparaissent ou évoluent dans le cockpit, **Then** `MFD-L` affiche la page `RADAR` avec les mêmes contacts, icônes, superpositions et états visuels que le client de référence.
3. **Given** le lancement d'AV DS avec sa configuration existante, **When** la fenêtre devient disponible, **Then** `MFD-L` est l'unique unité d'affichage et `RADAR` est sa page active.

---

### User Story 2 - Conserver la continuité de la liaison Radar (Priority: P2)

En tant que pilote, je veux qu'AV DS conserve les comportements d'attente, de pause, de perte de télémétrie et de reconnexion du client Radar existant, afin que le changement d'identité du produit ne dégrade pas son usage en mission.

**Why this priority**: Une vue Radar correcte au premier affichage ne suffit pas ; le pilote doit pouvoir poursuivre une session malgré les transitions ordinaires du jeu et du réseau local.

**Independent Test**: Démarrer AV DS avant puis après FS2Open, mettre une mission en pause, interrompre la télémétrie et la rétablir, puis changer de mission en observant les états présentés.

**Acceptance Scenarios**:

1. **Given** AV DS lancé avant qu'un état cockpit soit disponible, **When** aucune mission exploitable n'est reçue, **Then** la page indique son état d'attente sans présenter d'anciens contacts comme actuels.
2. **Given** une mission affichée, **When** le jeu est mis en pause, **Then** AV DS conserve le comportement de disponibilité du client Radar existant et n'invente aucune évolution des contacts.
3. **Given** une perte de télémétrie, **When** les données deviennent périmées, **Then** AV DS présente l'état de liaison correspondant et ne maintient pas les contacts périmés comme s'ils étaient actuels.
4. **Given** une liaison perdue, **When** la connexion et un état complet valide sont rétablis, **Then** la page `RADAR` converge de nouveau vers l'état cockpit courant sans redémarrage manuel d'AV DS.
5. **Given** une mission terminée ou remplacée, **When** la nouvelle mission devient active, **Then** aucun contact de l'ancienne mission ne subsiste dans la vue courante.
6. **Given** une configuration absente ou invalide, **When** AV DS présente le message correspondant déjà utilisé par le client Radar, **Then** ce message est affiché par AV DS indépendamment de la page `RADAR` et aucun contact n'est présenté comme courant.

---

### User Story 3 - Préserver l'autonomie de FS2Open (Priority: P3)

En tant que pilote, je veux que FS2Open et son HUD restent complets et jouables qu'AV DS soit lancé, absent, fermé ou déconnecté, afin que l'affichage externe demeure une aide cockpit facultative.

**Why this priority**: AV DS est un consommateur d'informations cockpit ; il ne doit devenir ni une dépendance du jeu ni une voie de commande implicite.

**Independent Test**: Exécuter la même mission avec AV DS connecté, déconnecté et non lancé, puis comparer le HUD et les commandes de jeu.

**Acceptance Scenarios**:

1. **Given** FS2Open en mission, **When** AV DS n'est pas lancé ou est fermé, **Then** le HUD, les commandes et le déroulement de la mission restent disponibles et inchangés.
2. **Given** AV DS en fonctionnement, **When** le pilote utilise ou observe la page `RADAR`, **Then** aucune action métier ne modifie la portée, la cible, les contacts ou tout autre état de la simulation.
3. **Given** un contact caché à la projection cockpit autorisée, **When** AV DS affiche sa vue Radar, **Then** ce contact n'apparaît jamais dans la page.

### Edge Cases

- AV DS est lancé alors que FS2Open est arrêté, au menu ou entre deux missions.
- La configuration de connexion existante est absente, invalide ou désigne un producteur indisponible.
- La télémétrie est interrompue au milieu d'une mise à jour Radar, puis revient avec un nouvel état complet.
- Une mission change alors que des contacts étaient encore visibles.
- L'identité du joueur observé change au cours d'une session.
- Aucun contact Radar n'est actuellement autorisé ou visible.
- La fenêtre unique est redimensionnée ou maximisée sous Windows.
- AV DS est fermé brutalement pendant que FS2Open poursuit la mission.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Le produit MUST être présenté à l'utilisateur sous le nom `AV DS — AV Display System` dans son exécutable, sa fenêtre, ses titres visibles et sa livraison Windows, et MUST remplacer le client Radar autonome dans cette livraison.
- **FR-002**: AV DS MUST fournir exactement une unité d'affichage dans cette phase, identifiée `MFD-L` et présentée dans une fenêtre Windows normale pouvant être maximisée.
- **FR-003**: AV DS MUST disposer d'un catalogue de pages intégré contenant uniquement la page fonctionnelle `RADAR` pour cette phase.
- **FR-004**: AV DS MUST sélectionner automatiquement `RADAR` comme page active de `MFD-L` au lancement.
- **FR-005**: La page `RADAR` MUST préserver les fonctions visuelles tactiques du client Radar de référence, notamment les contacts autorisés, leurs icônes et leurs superpositions. Les états et messages de liaison MUST être préservés par AV DS au niveau global conformément à FR-016, sans appartenir à la page `RADAR`.
- **FR-006**: AV DS MUST réutiliser les paramètres de connexion acceptés par le client Radar existant afin qu'une configuration auparavant valide reste exploitable sans ressaisie obligatoire.
- **FR-007**: AV DS MUST préserver les comportements existants d'établissement de session, d'attente, de pause, de perte de télémétrie, de resynchronisation et de reconnexion.
- **FR-008**: AV DS MUST retirer ou signaler comme périmées les informations Radar qui ne sont plus confirmées par un état cockpit courant.
- **FR-009**: AV DS MUST purger les contacts appartenant à une mission ou à un joueur observé précédent avant d'afficher le nouvel état courant.
- **FR-010**: AV DS MUST afficher uniquement les informations et contacts présents dans la projection cockpit autorisée reçue de FS2Open.
- **FR-011**: AV DS MUST fonctionner sans AV CORE et MUST se connecter directement à la source de télémétrie de FS2Open.
- **FR-012**: AV DS MUST rester passif vis-à-vis de la simulation ; seuls les échanges nécessaires à la réception fiable de la télémétrie sont autorisés, sans commande métier vers FS2Open.
- **FR-013**: FS2Open MUST conserver son HUD, ses commandes et son déroulement normal lorsqu'AV DS est absent, arrêté, déconnecté ou fermé pendant une mission.
- **FR-014**: La transformation MUST conserver le fonctionnement courant dans une fenêtre unique sans exiger un second écran, un contrôleur physique ou une nouvelle configuration de page.
- **FR-015**: Le modèle de navigation MUST représenter au minimum une unité d'affichage, un catalogue de pages et une page active, sans imposer de mécanisme d'extension dynamique.
- **FR-016**: AV DS MUST prendre en charge l'affichage des messages existants du client Radar comme un état global ; tout message actif MUST apparaître simultanément sur toutes les unités d'affichage, quelle que soit leur page active, et la page `RADAR` MUST fournir son contenu Radar sans posséder ni afficher elle-même ces messages.

### Scope Boundaries

- La phase exclut une seconde fenêtre et l'unité `MFD-R`.
- La livraison Windows ne conserve pas le client Radar autonome comme application installable distincte aux côtés d'AV DS.
- La phase exclut le mode plein écran sans bordure imposé.
- La phase exclut les contrôleurs Cougar, les OSB et toute autre entrée matérielle dédiée.
- La phase exclut les nouveaux filtres ou réglages Radar ; seuls les comportements existants sont conservés.
- La phase exclut la page `COM`, les Talking Heads et toute autre page cockpit.
- La phase exclut toute commande de simulation, notamment le changement de portée ou de cible.
- La phase cible uniquement Windows et n'inclut aucun travail de portabilité vers une autre plateforme.
- La phase n'introduit ni plugins, ni scripts de pages, ni cadre générique pour des extensions hypothétiques.
- `TrustedFullState` et toute vue omnisciente sont exclus ; les données restent limitées aux besoins concrets du cockpit.

### Key Entities

- **Application AV DS**: Produit d'affichage cockpit facultatif, portant l'identité visible, les paramètres de connexion partagés et le cycle de vie de la liaison.
- **Unité d'affichage `MFD-L`**: Unique surface d'affichage de cette phase ; elle possède une fenêtre et une page active.
- **Catalogue de pages**: Ensemble intégré des pages disponibles ; il contient uniquement `RADAR` dans cette phase.
- **Page `RADAR`**: Vue cockpit qui présente l'état Radar autorisé ; les messages transversaux sont présentés par AV DS et non par la page.
- **Messages AV DS**: État global reprenant les messages existants du client Radar ; tout message actif est présenté simultanément par chaque unité d'affichage, indépendamment de sa page active.
- **État Radar**: Réplique courante des informations visibles par le cockpit, incluant les contacts autorisés et leur présentation utile, sans état de mission caché.
- **Configuration de connexion**: Paramètres existants permettant à AV DS de joindre directement la source de télémétrie de FS2Open.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Dans 100 % des lancements de validation sous Windows avec une configuration valide, l'utilisateur voit une unique fenêtre identifiée AV DS, associée à `MFD-L`, avec `RADAR` comme page active.
- **SC-002**: Pour le fixture déterministe `referenceRadarImage` conservé dans les tests Radar, chaque contact autorisé conserve son identifiant, son asset d'icône résolu, sa couleur IFF, l'ordre de ses superpositions et son état visuel attendu, aucun identifiant supplémentaire n'est rendu, et les messages de liaison correspondants conservent leurs textes et couleurs au niveau global AV DS.
- **SC-003**: Dans chacun des quatre états vérifiés — attente, mission active, télémétrie perdue et reconnexion — l'utilisateur voit l'état approprié et aucun contact périmé n'est présenté comme courant.
- **SC-004**: Après rétablissement de la télémétrie ou changement de mission, la vue rejoint le premier état complet valide reçu sans redémarrage manuel et sans conserver de contact de l'état précédent.
- **SC-005**: Dans 100 % des scénarios avec AV DS non lancé, fermé ou déconnecté, le HUD, les commandes et la jouabilité de FS2Open restent inchangés.
- **SC-006**: Aucune action ou transition de cette phase ne change un état de simulation ; toutes les observations du jeu avec et sans AV DS sont fonctionnellement identiques hors affichage externe.
- **SC-007**: Un pilote déjà utilisateur du client Radar peut lancer AV DS avec ses paramètres de connexion valides et retrouver la vue Radar sans devoir configurer un second écran, un contrôleur ou une nouvelle page.

## Assumptions

- Le client Radar autonome existant constitue la référence fonctionnelle pour le rendu, les états de liaison et la reconnexion à préserver.
- La source FS2Open publie déjà les données Radar filtrées selon la visibilité cockpit ; cette phase ne demande aucune nouvelle donnée de mission.
- Les paramètres de connexion existants sont disponibles localement et restent compatibles avec la source de télémétrie utilisée.
- La machine d'affichage exécute une version Windows supportée par le client Radar actuel et possède un écran utilisable.
- Le renommage vers AV DS peut coexister avec une migration progressive des noms internes qui ne sont pas visibles par l'utilisateur, à condition que l'identité livrée et les comportements observables soient cohérents.
- Les tests automatisés et une vérification manuelle courte en jeu suffisent pour valider cette phase ; aucun registre de preuve ni mécanisme de blocage des phases suivantes n'est requis.
