import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode
} from "react";

export type DashboardLanguage = "fr" | "en";

const STORAGE_KEY = "fso-telemetry-dashboard-language";

const ENGLISH: Record<string, string> = {
  "VALIDATION TÉLÉMÉTRIE": "TELEMETRY VALIDATION",
  "MISSION": "MISSION",
  "SESSION": "SESSION",
  "JOUEUR": "PLAYER",
  "MODE": "MODE",
  "BRIDGE HORS LIGNE": "BRIDGE OFFLINE",
  "SYSTÈMES": "SYSTEMS",
  "MODULE ACTIF": "ACTIVE MODULE",
  "SOURCES": "SOURCES",
  "Pilotage": "Flight",
  "Propulsion & énergie": "Propulsion & energy",
  "Intégrité": "Integrity",
  "Armement": "Weapons",
  "Support · Docking · Cargo": "Support · Docking · Cargo",
  "Mission & entités": "Mission & entities",
  "Configuration": "Configuration",
  "Tactique": "Tactical",
  "Communications & effets": "Communications & effects",
  "Diagnostic télémétrie": "Telemetry diagnostics",
  "Sections du cockpit": "Cockpit sections",
  "INSPECTION INSTRUMENT": "INSTRUMENT INSPECTION",
  "Fermer": "Close",
  "Valeur": "Value",
  "Valeur brute": "Raw value",
  "Source": "Source",
  "Formule": "Formula",
  "valeur directe": "direct value",
  "Entité": "Entity",
  "Présence": "Presence",
  "Champs couverts": "Covered fields",
  "source directe": "direct source",
  "Échantillon": "Sample",
  "Âge estimé": "Estimated age",
  "Cadence observée": "Observed rate",
  "Raison": "Reason",
  "aucun écart": "no discrepancy",
  "Valeurs associées": "Related values",
  "MANIFESTE": "MANIFEST",
  "RENDU": "RENDER",
  "LECTURE": "PLAY",
  "Vitesse": "Speed",
  "Position": "Position",
  "Position du replay": "Replay position",
  "Capture arrêtée": "Capture stopped",
  "Capture démarrée": "Capture started",
  "■ ARRÊTER CAPTURE": "■ STOP CAPTURE",
  "● CAPTURER": "● CAPTURE",
  "Exports créés": "Exports created",
  "EXPORTER": "EXPORT",
  "ÉTAT DE SESSION": "SESSION STATUS",
  "ÉTAT": "STATUS",
  "BASELINE": "BASELINE",
  "DERNIER LIVE": "LAST LIVE",
  "RAISON STALE": "STALE REASON",
  "RESYNCHRONISER": "RESYNCHRONIZE",
  "RECONNECTER": "RECONNECT",
  "OUVRIR LE DIAGNOSTIC": "OPEN DIAGNOSTICS",
  "La session est Live. Confirmer la reconnexion ?": "The session is Live. Confirm reconnection?",
  "CONFIRMER": "CONFIRM",
  "ANNULER": "CANCEL",
  "REPLAY": "REPLAY",
  "CONTRÔLES DU REPLAY": "REPLAY CONTROLS",
  "PAUSE": "PAUSE",
  "DONNÉES": "DATA",
  "GESTION DES DONNÉES": "DATA MANAGEMENT",
  "AFFICHAGE": "DISPLAY",
  "OPTIONS D’AFFICHAGE": "DISPLAY OPTIONS",
  "FIGER L’AFFICHAGE": "FREEZE DISPLAY",
  "REPRENDRE L’AFFICHAGE": "RESUME DISPLAY",
  "PLEIN ÉCRAN": "FULLSCREEN",
  "QUITTER LE PLEIN ÉCRAN": "EXIT FULLSCREEN",
  "FIGÉ": "FROZEN",
  "RESYNCING": "RESYNCHRONIZING",
  "RECONNECTING": "RECONNECTING",
  "Resynchronisation demandée": "Resynchronization requested",
  "Reconnexion demandée": "Reconnection requested",
  "Erreur": "Error",
  "EN ATTENTE": "WAITING",
  "AUCUN": "NONE",
  "AUCUN SEGMENT": "NO SEGMENTS",
  "AUCUN ÉCHANTILLON": "NO SAMPLES",
  "CANAL": "CHANNEL",
  "ÂGE": "AGE",
  "ACTIF": "ACTIVE",
  "Cockpit de pilotage": "Flight cockpit",
  "CAP": "HEADING",
  "TANGAGE": "PITCH",
  "ROULIS": "ROLL",
  "DÉRIVE": "DRIFT",
  "VITESSE": "SPEED",
  "VITESSE LOCALE": "LOCAL VELOCITY",
  "VITESSE MONDE": "WORLD VELOCITY",
  "POSITION MONDE": "WORLD POSITION",
  "VITESSE ANGULAIRE": "ANGULAR VELOCITY",
  "CROISIÈRE": "CRUISE",
  "RAYON": "RADIUS",
  "DEMANDE PRIMAIRE": "PRIMARY REQUEST",
  "DEMANDE SECONDAIRE": "SECONDARY REQUEST",
  "DEMANDE CONTRE-MESURE": "COUNTERMEASURE REQUEST",
  "LACET": "YAW",
  "SENSIBILITÉ": "SENSITIVITY",
  "Vitesse quasi nulle": "Near-zero speed",
  "Données futures non disponibles": "Future data unavailable",
  "Cockpit propulsion et énergie": "Propulsion and energy cockpit",
  "MODE ETS": "ETS MODE",
  "BOUCLIERS": "SHIELDS",
  "ARMES": "WEAPONS",
  "MOTEURS": "ENGINES",
  "RÉGÉN. ARMES": "WEAPON REGEN.",
  "RÉGÉN. BOUCLIERS": "SHIELD REGEN.",
  "TRANSFERT → ARMES": "TRANSFER → WEAPONS",
  "TRANSFERT → BOUCLIERS": "TRANSFER → SHIELDS",
  "ÉNERGIE ARMES": "WEAPON ENERGY",
  "CARBURANT AB": "AFTERBURNER FUEL",
  "PRÊT DANS": "READY IN",
  "AUTONOMIE AB": "AFTERBURNER ENDURANCE",
  "RECHARGE COMPLÈTE": "FULL RECHARGE",
  "CARBURANT UTILISABLE": "USABLE FUEL",
  "ACCÉLÉRATION AB": "AFTERBURNER ACCELERATION",
  "VITESSE AB AVANT": "FORWARD AFTERBURNER SPEED",
  "CONSOMMATION": "CONSUMPTION",
  "RÉCUPÉRATION": "RECOVERY",
  "CLASSE": "CLASS",
  "VITESSE NOMINALE": "NOMINAL SPEED",
  "VITESSE AB": "AFTERBURNER SPEED",
  "VITESSE BOOSTER": "BOOSTER SPEED",
  "ACCÉL. AVANT": "FORWARD ACCEL.",
  "ACCÉL. AB": "AFTERBURNER ACCEL.",
  "DÉCÉL. AVANT": "FORWARD DECEL.",
  "CAPACITÉ AB": "AFTERBURNER CAPACITY",
  "Données énergétiques futures non disponibles": "Future energy data unavailable",
  "Cockpit d’intégrité": "Integrity cockpit",
  "COQUE": "HULL",
  "MAX DYNAMIQUE": "DYNAMIC MAX",
  "HP MANQUANTS": "MISSING HP",
  "BOUCLIER": "SHIELD",
  "MAX BOUCLIER": "MAX SHIELD",
  "CHARGE TOTALE": "TOTAL CHARGE",
  "QUADRANT LE PLUS FAIBLE": "WEAKEST QUADRANT",
  "ÉTAT VAISSEAU": "SHIP STATUS",
  "ARMURE": "ARMOR",
  "SEUIL GUARDIAN": "GUARDIAN THRESHOLD",
  "MARGE GUARDIAN": "GUARDIAN MARGIN",
  "DÉGÂTS": "DAMAGE",
  "DÉFICIT": "DEFICIT",
  "RÉGÉNÉRATION": "REGENERATION",
  "PLAFOND RECHARGEABLE": "RECHARGE CEILING",
  "TRANSFERT DIFFÉRÉ": "DEFERRED TRANSFER",
  "RECHARGE ESTIMÉE": "ESTIMATED RECHARGE",
  "Données d’intégrité futures non disponibles": "Future integrity data unavailable",
  "Inspecter": "Inspect",
  "Cockpit d’armement": "Weapons cockpit",
  "Modes et autorisations": "Modes and permissions",
  "Racks primaires": "Primary racks",
  "Systèmes sélectionnés": "Selected systems",
  "Racks secondaires": "Secondary racks",
  "Réserves et activité": "Reserves and activity",
  "Extensions tactiques": "Tactical extensions",
  "PRIMAIRE SÉLECTIONNÉE": "SELECTED PRIMARY",
  "SECONDAIRE SÉLECTIONNÉE": "SELECTED SECONDARY",
  "aucune banque installée": "no bank installed",
  "sélecteur incohérent avec les banques reçues": "selector inconsistent with received banks",
  "STANDARD": "STANDARD",
  "COÛT ÉNERGIE": "ENERGY COST",
  "MUNITIONS": "AMMUNITION",
  "DÉGÂTS NOMINAUX": "NOMINAL DAMAGE",
  "CADENCE NOMINALE": "NOMINAL RATE",
  "PORTÉE OPTIMALE": "OPTIMAL RANGE",
  "PORTÉE MAXIMALE": "MAXIMUM RANGE",
  "GUIDAGE": "GUIDANCE",
  "LOCK NOMINAL": "NOMINAL LOCK",
  "INCONNU": "UNKNOWN",
  "réserve contextuelle · pas une autorisation de tir": "contextual reserve · not a firing authorization",
  "TERTIAIRE": "TERTIARY",
  "aucune banque tertiaire": "no tertiary bank",
  "CONTRE-MESURES": "COUNTERMEASURES",
  "aucune contre-mesure": "no countermeasure",
  "SALVE / DÉTONATION": "VOLLEY / DETONATION",
  "DEMANDES DE COMMANDE": "CONTROL REQUESTS",
  "intentions · jamais des tirs confirmés": "intentions · never confirmed shots",
  "TOURELLES DU JOUEUR": "PLAYER TURRETS",
  "VERROUILLÉE": "LOCKED",
  "CIBLE": "TARGET",
  "DISPONIBLE": "AVAILABLE",
  "Données d’armement futures non disponibles": "Future weapons data unavailable",
  "Cockpit support docking cargo": "Support docking cargo cockpit",
  "Support assigné et phase": "Assigned support and phase",
  "État courant à restaurer": "Current state to restore",
  "Approche relative": "Relative approach",
  "Composante d’amarrage": "Docking component",
  "Scanner cargo": "Cargo scanner",
  "VAISSEAU DE SUPPORT": "SUPPORT SHIP",
  "PHASE ACTUELLE": "CURRENT PHASE",
  "état courant · aucun verdict de réussite": "current state · no success verdict",
  "PHASE": "PHASE",
  "RELATIONS": "RELATIONS",
  "LEADER": "LEADER",
  "CIBLE CARGO": "CARGO TARGET",
  "RESTANT": "REMAINING",
  "CONTENU DIVULGUÉ": "REVEALED CONTENT",
  "CAPTEURS · PORTÉE · ENVIRONNEMENT": "SENSORS · RANGE · ENVIRONMENT",
  "MODE RADAR": "RADAR MODE",
  "PORTÉE": "RANGE",
  "CAPTEURS": "SENSORS",
  "Portée infinie": "Infinite range",
  "portée illimitée": "unlimited range",
  "unités monde": "world units",
  "non applicable": "not applicable",
  "CIBLE SÉLECTIONNÉE": "SELECTED TARGET",
  "AUCUNE CIBLE": "NO TARGET",
  "RÉFÉRENCE INCONNUE": "UNKNOWN REFERENCE",
  "Informations HUD FSO": "FSO HUD information",
  "DISTANCE": "DISTANCE",
  "RAPPROCHEMENT": "CLOSING SPEED",
  "TEMPS CIBLE": "TIME ON TARGET",
  "DANS LE CÔNE": "IN CONE",
  "HORS CÔNE": "OUT OF CONE",
  "Tendance distance": "Distance trend",
  "Tendance vitesse": "Speed trend",
  "Sous-système ciblé": "Targeted subsystem",
  "Sous-système lock": "Locked subsystem",
  "Cible précédente": "Previous target",
  "PISTE FURTIVE MÉMORISÉE · OBSERVATION ANCIENNE": "REMEMBERED STEALTH TRACK · STALE OBSERVATION",
  "VERROUILLAGES": "LOCKS",
  "MENACES ENTRANTES": "INCOMING THREATS",
  "NIVEAU": "LEVEL",
  "AUCUN MISSILE ENTRANT": "NO INCOMING MISSILE",
  "AUCUN LOCK": "NO LOCK",
  "AUCUN CONTACT AUTORISÉ": "NO AUTHORIZED CONTACT",
  "Contacts radar prioritaires": "Priority radar contacts",
  "Données tactiques non disponibles": "Tactical data unavailable",
  "Brouillage global quantifié": "Quantified global jamming",
  "Historique global de visibilité": "Global visibility history",
  "Impact et probabilité autoritaires": "Authoritative impact and probability",
  "Prédiction du résultat d’un tir": "Shot outcome prediction",
  "Coordonnées HUD projetées": "Projected HUD coordinates",
  "Vidéo de cible H.264": "H.264 target video",
  "PISTES AUTORISÉES": "AUTHORIZED TRACKS",
  "Dérivations et provenance": "Derivations and provenance",
  "Manifeste autorisé": "Authorized manifest",
  "AUCUN RÉSULTAT": "NO RESULTS",
  "FILTRER PAR NOM, TYPE OU CAPACITÉ": "FILTER BY NAME, TYPE OR CAPABILITY",
  "Sélectionnée": "Selected",
  "OUI": "YES",
  "NON": "NO",
  "État temporel": "Temporal state",
  "Capacités": "Capabilities",
  "aucune": "none",
  "Réarmement": "Rearm time",
  "Coût énergétique": "Energy cost",
  "Dégâts nominaux": "Nominal damage",
  "État runtime brut": "Raw runtime state",
  "Définition de banque et géométrie": "Bank definition and geometry"
  ,"ACTIVITÉ & CURSEUR": "ACTIVITY & CURSOR"
  ,"ACTUEL": "CURRENT"
  ,"ALERTES": "ALERTS"
  ,"ALERTES HUD INDISPONIBLES": "HUD ALERTS UNAVAILABLE"
  ,"ATTITUDE INERTIELLE": "INERTIAL ATTITUDE"
  ,"AUCUN AVERTISSEMENT HUD": "NO HUD WARNING"
  ,"AUCUN BOUCLIER": "NO SHIELD"
  ,"AUCUN MODE ACTIF": "NO ACTIVE MODE"
  ,"AUCUN SUPPORT ASSIGNÉ": "NO SUPPORT ASSIGNED"
  ,"AUCUNE BANQUE": "NO BANK"
  ,"AUCUNE RELATION MATÉRIALISÉE": "NO MATERIALIZED RELATION"
  ,"AUTRES": "MORE"
  ,"AUTRES · COMPOSANTE COMPLÈTE": "MORE · FULL COMPONENT"
  ,"AUTRES · LISTE COMPLÈTE": "MORE · FULL LIST"
  ,"AUTRES · OUVRIR LA LISTE COMPLÈTE": "MORE · OPEN FULL LIST"
  ,"AVANT": "FRONT"
  ,"Armure": "Armor"
  ,"Aucun support assigné": "No support assigned"
  ,"BANQUE": "BANK"
  ,"BANQUES": "BANKS"
  ,"BAS": "DOWN"
  ,"BRUT": "RAW"
  ,"Banque": "Bank"
  ,"Banque absente du snapshot courant.": "Bank missing from the current snapshot."
  ,"Banque d’arme": "Weapon bank"
  ,"Banques": "Banks"
  ,"CAPTURE HISTORIQUE · MENACE AGRÉGÉE": "HISTORICAL CAPTURE · AGGREGATED THREAT"
  ,"CIBLE NON EXPOSÉE": "TARGET NOT EXPOSED"
  ,"COMMANDES": "CONTROLS"
  ,"COMPOSANTE": "COMPONENT"
  ,"CONTACTS": "CONTACTS"
  ,"Cadence": "Rate"
  ,"Cadence nominale": "Nominal rate"
  ,"Cible cargo non exposée": "Cargo target not exposed"
  ,"Classe": "Class"
  ,"Conditions": "Conditions"
  ,"Contacts radar": "Radar contacts"
  ,"Contenu": "Content"
  ,"Cooldown tourelle": "Turret cooldown"
  ,"DROITE": "RIGHT"
  ,"Demandes du tick source · pas des tirs confirmés": "Source-tick requests · not confirmed shots"
  ,"Divulgation": "Disclosure"
  ,"DÉTRUIT": "DESTROYED"
  ,"DÉTRUITS": "DESTROYED"
  ,"Détruit": "Destroyed"
  ,"ESTIMATION AU TAUX INSTANTANÉ COURANT": "ESTIMATE AT CURRENT INSTANTANEOUS RATE"
  ,"Entité distante": "Remote entity"
  ,"Entité locale": "Local entity"
  ,"FILTRER PAR NOM": "FILTER BY NAME"
  ,"FILTRER PAR NOM, TYPE OU ÉTAT": "FILTER BY NAME, TYPE OR STATUS"
  ,"Famille": "Family"
  ,"Flags support": "Support flags"
  ,"GAUCHE": "LEFT"
  ,"GESTION ETS": "ETS MANAGEMENT"
  ,"Guidage": "Guidance"
  ,"HAUT": "UP"
  ,"HP manquants": "Missing HP"
  ,"ID sous-système": "Subsystem ID"
  ,"INSPECTION BANQUE": "BANK INSPECTION"
  ,"INSPECTION ENTITÉ SUPPORT / DOCKING": "SUPPORT / DOCKING ENTITY INSPECTION"
  ,"INSPECTION EXHAUSTIVE": "FULL INSPECTION"
  ,"INSPECTION RELATION": "RELATION INSPECTION"
  ,"INSPECTION SCANNER CARGO": "CARGO SCANNER INSPECTION"
  ,"INSPECTION SOUS-SYSTÈME": "SUBSYSTEM INSPECTION"
  ,"INSPECTION TACTIQUE": "TACTICAL INSPECTION"
  ,"Index canonique": "Canonical index"
  ,"LIBRE": "FREE"
  ,"LISTE": "LIST"
  ,"LISTE COMPLÈTE": "FULL LIST"
  ,"LISTE EXHAUSTIVE": "FULL LIST"
  ,"MASQUÉ": "HIDDEN"
  ,"MESURES GÉOMÉTRIQUES · AUCUNE ETA DÉDUITE": "GEOMETRIC MEASUREMENTS · NO DERIVED ETA"
  ,"MOUVEMENT": "MOTION"
  ,"MVT VERROUILLÉ": "MOTION LOCKED"
  ,"Missiles entrants": "Incoming missiles"
  ,"Munitions": "Ammunition"
  ,"NOMINAL": "NOMINAL"
  ,"PROPULSION": "PROPULSION"
  ,"PROTECTIONS": "PROTECTIONS"
  ,"Perturbation": "Disruption"
  ,"Phase": "Phase"
  ,"Phase docking": "Docking phase"
  ,"Phase support": "Support phase"
  ,"Point distant": "Remote point"
  ,"Point local": "Local point"
  ,"Points de tir": "Fire points"
  ,"Progression": "Progress"
  ,"QUADRANTS": "QUADRANTS"
  ,"RESSOURCES": "RESOURCES"
  ,"RESTANTS": "REMAINING"
  ,"Relation absente du snapshot courant.": "Relation missing from the current snapshot."
  ,"Relation d’amarrage": "Docking relation"
  ,"Relation inverse": "Reciprocal relation"
  ,"Relations directes": "Direct relations"
  ,"Relations publiées": "Published relations"
  ,"SCOPE RADAR": "RADAR SCOPE"
  ,"SOUS-SYSTÈMES": "SUBSYSTEMS"
  ,"STRUCTURE & BOUCLIERS": "STRUCTURE & SHIELDS"
  ,"Sous-système": "Subsystem"
  ,"Sous-système absent du snapshot courant.": "Subsystem missing from the current snapshot."
  ,"Sous-systèmes du joueur": "Player subsystems"
  ,"Support assigné": "Assigned support"
  ,"SÉLECTIONNÉE": "SELECTED"
  ,"TIR PRIMAIRE": "PRIMARY FIRE"
  ,"TOTAL": "TOTAL"
  ,"TOURELLE": "TURRET"
  ,"TOURELLES": "TURRETS"
  ,"Tourelles du joueur": "Player turrets"
  ,"Tous les contacts": "All contacts"
  ,"Tous les missiles": "All missiles"
  ,"Tous les verrouillages": "All locks"
  ,"Type": "Type"
  ,"Verrouillages": "Locks"
  ,"arme énergétique": "energy weapon"
  ,"aucun": "none"
  ,"aucun bouclier": "no shield"
  ,"aucun détonateur actif": "no active detonator"
  ,"aucun sous-système": "no subsystem"
  ,"aucune affectation active": "no active assignment"
  ,"détonation": "detonation"
  ,"entités": "entities"
  ,"indisponible": "unavailable"
  ,"masqué": "hidden"
  ,"progression": "progress"
  ,"relations réciproques": "reciprocal relations"
  ,"sans réserve de HP": "without HP reserve"
  ,"segments · configuration non prise en charge": "segments · unsupported configuration"
  ,"tir/s": "shot/s"
  ,"ÉNERGIE": "ENERGY"
  ,"ÉTATS DE VOL": "FLIGHT STATES"
  ,"États": "States"
  ,"ALIGNEMENT / ETA DOCKING": "DOCKING ALIGNMENT / ETA"
  ,"ANIMATION DE BANQUE": "BANK ANIMATION"
  ,"AUCUNE PROTECTION": "NO PROTECTION"
  ,"AUCUNE TENTATIVE": "NO ATTEMPT"
  ,"CYCLE NOMINAL": "NOMINAL LIFECYCLE"
  ,"DIRECTION / POSITION IMPACT": "IMPACT DIRECTION / POSITION"
  ,"DISTANCE / ANGLE CARGO": "CARGO DISTANCE / ANGLE"
  ,"DOCKING GLOBAL": "GLOBAL DOCKING"
  ,"EN COURS": "IN PROGRESS"
  ,"ETA SUPPORT / INTERVENTION": "SUPPORT / SERVICE ETA"
  ,"LASER DE CIBLAGE": "TARGETING LASER"
  ,"LOCK ACQUIS": "LOCK ACQUIRED"
  ,"LOCK P": "PRIMARY LOCK"
  ,"LOCK S": "SECONDARY LOCK"
  ,"PROGRESSION GLOBALE": "GLOBAL PROGRESS"
  ,"PROGRESSION LOCK": "LOCK PROGRESS"
  ,"PUISSANCE MOTEUR DYNAMIQUE": "DYNAMIC ENGINE POWER"
  ,"SANS TENTATIVE": "NO ATTEMPT"
  ,"SECONDAIRE DOUBLE": "DUAL SECONDARY"
  ,"SOLUTION DE TIR": "FIRING SOLUTION"
  ,"TENTATIVE LOCK": "LOCK ATTEMPT"
  ,"VITESSE MAX DISPONIBLE": "AVAILABLE MAX SPEED"
  ,"VITESSE RELATIVE": "RELATIVE SPEED"
  ,"Afterburner, booster et glide": "Afterburner, booster and glide"
  ,"Armes sélectionnées": "Selected weapons"
  ,"Assets Talking Head": "Talking Head assets"
  ,"Axes, croisière et aides": "Axes, cruise and assists"
  ,"Baseline": "Baseline"
  ,"COMM_ASSET_MANIFEST non produit actuellement": "COMM_ASSET_MANIFEST is not currently produced"
  ,"COMM_VIEW_EVENT non produit actuellement": "COMM_VIEW_EVENT is not currently produced"
  ,"COMM_VIEW_STATE non produit actuellement": "COMM_VIEW_STATE is not currently produced"
  ,"Cadences et fraîcheur": "Rates and freshness"
  ,"Capacités négociées": "Negotiated capabilities"
  ,"Capteurs tactiques": "Tactical sensors"
  ,"Catalogue des classes": "Class catalog"
  ,"Chronologie d’événements": "Event timeline"
  ,"Cible sélectionnée": "Selected target"
  ,"Cinématique complète": "Complete kinematics"
  ,"Compression temporelle": "Time compression"
  ,"Cycle de vie joueur": "Player lifecycle"
  ,"Demandes de commande": "Control requests"
  ,"Distribution ETS": "ETS distribution"
  ,"Domaines couverts": "Covered domains"
  ,"Données tactiques futures": "Future tactical data"
  ,"EFFECT_STATE non produit actuellement": "EFFECT_STATE is not currently produced"
  ,"Effets / EMP / tags": "Effects / EMP / tags"
  ,"Erreurs de décodage": "Decode errors"
  ,"Espèce": "Species"
  ,"Extension du cockpit": "Cockpit extension"
  ,"Extensions dégâts et impacts": "Damage and impact extensions"
  ,"Extensions support, docking et cargo": "Support, docking and cargo extensions"
  ,"Extensions énergie et propulsion": "Energy and propulsion extensions"
  ,"Flight Cursor non applicable au vaisseau courant": "Flight Cursor is not applicable to the current ship"
  ,"Gabarit et modes physiques": "Dimensions and physics modes"
  ,"Génération mission": "Mission generation"
  ,"Identité du vaisseau": "Ship identity"
  ,"Manifeste actif": "Active manifest"
  ,"Matrice des sous-systèmes": "Subsystem matrix"
  ,"Menaces entrantes": "Incoming threats"
  ,"NAVIGATION_STATE non produit actuellement": "NAVIGATION_STATE is not currently produced"
  ,"Navigation et navpoints": "Navigation and navpoints"
  ,"Pause moteur": "Engine pause"
  ,"Performances nominales de classe": "Nominal class performance"
  ,"Phase mission": "Mission phase"
  ,"Protections et cycle de vie": "Protections and lifecycle"
  ,"Resynchronisations": "Resynchronizations"
  ,"Récupération des boucliers": "Shield recovery"
  ,"Réserves énergétiques": "Energy reserves"
  ,"Rôles": "Roles"
  ,"Santé et environnement moteur": "Engine health and environment"
  ,"Scope radar": "Radar scope"
  ,"Sphère d’attitude inertielle": "Inertial attitude sphere"
  ,"Structure et boucliers": "Structure and shields"
  ,"Synchronisation": "Synchronization"
  ,"Séquence delta": "Delta sequence"
  ,"Trous transport": "Transport gaps"
  ,"brouillage global, historique global de visibilité, impact/probabilité autoritaires, résultat de tir, coordonnées HUD projetées et vidéo de cible H.264": "global jamming, global visibility history, authoritative impact/probability, shot outcome, projected HUD coordinates and H.264 target video"
  ,"données prévues mais non produites par la couverture actuelle": "planned data not produced by the current coverage"
  ,"données prévues mais non produites par le contrat actuel": "planned data not produced by the current contract"
  ,"données prévues mais non produites par le profil actuel": "planned data not produced by the current profile"
  ,"données tactiques non produites par la couverture actuelle": "tactical data not produced by the current coverage"
  ,"durées, géométrie et vue tactique non produites par la couverture actuelle": "timings, geometry and tactical view not produced by the current coverage"
  ,"flux vidéo non produit actuellement": "video stream is not currently produced"
  ,"groupe REQUEST_COUNTERS absent pour l’entité courante": "REQUEST_COUNTERS group missing for the current entity"
  ,"Équipe / IFF / espèce": "Team / IFF / species"
  ,"État de connexion": "Connection status"
  ,"Événements communication": "Communication events"
  ,"Synchronizing": "Synchronizing"
  ,"Disconnected": "Disconnected"
  ,"champ optionnel absent pour l’entité courante": "optional field missing for the current entity"
  ,"GLIDE FORCÉ": "FORCED GLIDE"
  ,"AMORT. NEWTONIEN": "NEWTONIAN DAMPING"
  ,"WARP ENTRANT": "WARP IN"
  ,"WARP SORTANT": "WARP OUT"
  ,"SCRIPTÉ": "SCRIPTED"
  ,"ONDE DE CHOC": "SHOCKWAVE"
  ,"IMMOBILE": "STATIONARY"
  ,"ORIENTATION VERROUILLÉE": "ORIENTATION LOCKED"
  ,"PRIMAIRE LIÉ": "PRIMARY LINKED"
  ,"AFTERBURNER DEMANDÉ": "AFTERBURNER REQUESTED"
  ,"VAISSEAU": "SHIP"
  ,"VUE": "VIEW"
  ,"AUTOPILOTE": "AUTOPILOT"
  ,"ABSENT": "ABSENT"
  ,"VERROUILLÉ": "LOCKED"
  ,"AFTERBURNER DISPONIBLE": "AFTERBURNER AVAILABLE"
  ,"AFTERBURNER VERROUILLÉ": "AFTERBURNER LOCKED"
  ,"AFTERBURNER ACTIF": "AFTERBURNER ACTIVE"
  ,"BOOSTER ACTIF": "BOOSTER ACTIVE"
  ,"GLIDE ACTIF": "GLIDE ACTIVE"
  ,"RCS ACTIF": "RCS ACTIVE"
  ,"INVULNÉRABLE": "INVULNERABLE"
  ,"PROTÉGÉ": "PROTECTED"
  ,"PERTURBÉ": "DISRUPTED"
  ,"CIBLABLE": "TARGETABLE"
  ,"RÉVÉLÉ": "REVEALED"
  ,"MOUVEMENT VERROUILLÉ": "MOVEMENT LOCKED"
  ,"BEAM LIBRE": "BEAM FREE"
  ,"BEAM VERROUILLÉ": "BEAM LOCKED"
  ,"MOTEUR": "ENGINE"
  ,"RADAR": "RADAR"
  ,"NAVIGATION": "NAVIGATION"
  ,"COMMUNICATION": "COMMUNICATION"
  ,"RÉACTEUR": "REACTOR"
  ,"MANŒUVRE": "MANEUVERING"
  ,"HANGAR": "HANGAR"
  ,"AUTRE": "OTHER"
  ,"ARRIÈRE": "REAR"
  ,"PRIMAIRES LIÉES": "PRIMARY LINKED"
  ,"GÂCHETTE PRIMAIRE": "PRIMARY TRIGGER"
  ,"GÂCHETTE SECONDAIRE": "SECONDARY TRIGGER"
  ,"PRIMAIRES VERROUILLÉES": "PRIMARY LOCKED"
  ,"SECONDAIRES VERROUILLÉES": "SECONDARY LOCKED"
  ,"DÉTONATEURS DISTANTS": "REMOTE DETONATORS"
  ,"BOMBE": "BOMB"
  ,"BALISTIQUE": "BALLISTIC"
  ,"SANS MUNITIONS": "NO AMMUNITION"
  ,"CONTRE-MESURE": "COUNTERMEASURE"
  ,"GUIDÉE": "GUIDED"
  ,"TÉLÉ-DÉTONABLE": "REMOTE-DETONATABLE"
  ,"PRIMAIRE": "PRIMARY"
  ,"MISSILE": "MISSILE"
  ,"SPÉCIAL": "SPECIAL"
  ,"CHALEUR": "HEAT"
  ,"CYCLE AVANT": "FORWARD CYCLE"
  ,"CYCLE ARRIÈRE": "REVERSE CYCLE"
  ,"ALÉATOIRE EXHAUSTIF": "EXHAUSTIVE RANDOM"
  ,"ALÉATOIRE SANS RÉPÉTITION": "RANDOM WITHOUT REPETITION"
  ,"ALÉATOIRE RÉPÉTITIF": "REPEATING RANDOM"
  ,"DEMANDÉ": "REQUESTED"
  ,"EN APPROCHE": "APPROACHING"
  ,"AMARRAGE": "DOCKING"
  ,"RÉPARATION": "REPAIR"
  ,"RÉARMEMENT": "REARMING"
  ,"OBSTRUCTION": "OBSTRUCTED"
  ,"ABANDONNÉ": "ABORTED"
  ,"APPROCHE": "APPROACH"
  ,"AMARRÉ": "DOCKED"
  ,"SÉPARATION": "SEPARATION"
  ,"NON SCANNABLE": "NOT SCANNABLE"
  ,"SCAN EN COURS": "SCAN IN PROGRESS"
  ,"TERMINÉ": "COMPLETE"
  ,"ATTENTE RÉPARATION": "WAITING FOR REPAIR"
  ,"PRISE EN CHARGE": "IN SERVICE"
  ,"INTERVIENT SUR UN AUTRE": "SERVICING ANOTHER"
  ,"ANGLE": "ANGLE"
  ,"LIGNE DE VUE": "LINE OF SIGHT"
  ,"COURTE": "SHORT"
  ,"LONGUE": "LONG"
  ,"INFINIE": "INFINITE"
  ,"PERSONNALISÉE": "CUSTOM"
  ,"HORS LIGNE": "OFFLINE"
  ,"DÉGRADÉ": "DEGRADED"
  ,"EN LIGNE": "ONLINE"
  ,"PISTE MÉMORISÉE": "REMEMBERED TRACK"
  ,"DISTORDUE": "DISTORTED"
  ,"ARME": "WEAPON"
  ,"NŒUD DE SAUT": "JUMP NODE"
  ,"ASTÉROÏDE": "ASTEROID"
  ,"DÉBRIS": "DEBRIS"
  ,"AUCUNE": "NONE"
  ,"LOCK EN COURS": "LOCK IN PROGRESS"
  ,"BRILLANT": "BRIGHT"
  ,"FURTIF": "STEALTH"
  ,"TAGUÉ": "TAGGED"
  ,"MENACE": "THREAT"
  ,"VAISSEAU NORMAL": "NORMAL SHIP"
  ,"INCONNUE": "UNKNOWN"
  ,"DIMINUE": "DECREASING"
  ,"STABLE": "STABLE"
  ,"AUGMENTE": "INCREASING"
  ,"ACQUIS": "ACQUIRED"
  ,"ACQUISITION": "ACQUIRING"
  ,"MISSILES": "MISSILES"
  ,"CAPTURE": "CAPTURE"
  ,"LATÉRAL": "LATERAL"
  ,"VERTICAL": "VERTICAL"
  ,"LONGITUDINAL": "LONGITUDINAL"
  ,"COORDONNÉES HUD": "HUD COORDINATES"
  ,"RÉFÉRENCE GRAVITATIONNELLE": "GRAVITY REFERENCE"
  ,"HORIZON PLANÉTAIRE": "PLANETARY HORIZON"
  ,"VITESSE DÉSIRÉE": "DESIRED SPEED"
  ,"ACCÉLÉRATION": "ACCELERATION"
  ,"unités": "units"
  ,"points": "points"
  ,"PRÊT": "READY"
  ,"INTÉGRITÉ": "INTEGRITY"
  ,"RÉPARTITION ETS EXACTE": "EXACT ETS DISTRIBUTION"
  ,"POUSSÉE EFFECTIVE": "EFFECTIVE THRUST"
  ,"CHARGES INSTANTANÉES": "INSTANTANEOUS LOADS"
  ,"DERNIÈRE SOURCE": "LAST SOURCE"
  ,"DERNIÈRE ARME": "LAST WEAPON"
  ,"DÉGÂTS CUMULÉS": "CUMULATIVE DAMAGE"
  ,"CONTRIBUTEURS": "CONTRIBUTORS"
  ,"SÉPARÉES": "SEPARATE"
  ,"GÂCHETTE P": "PRIMARY TRIGGER"
  ,"GÂCHETTE S": "SECONDARY TRIGGER"
  ,"DÉTONATEURS": "DETONATORS"
  ,"énergie": "energy"
  ,"CIBLE · DISTANCE · LEAD": "TARGET · DISTANCE · LEAD"
  ,"TIRS / IMPACTS CONFIRMÉS": "CONFIRMED SHOTS / IMPACTS"
  ,"état courant · pas une promesse de remise à niveau": "current state · not a restoration promise"
  ,"banques énergétiques ou absentes": "energy-based or absent banks"
  ,"aucun tertiaire": "no tertiary"
  ,"accumulation théorique": "theoretical accumulation"
  ,"progression figée": "frozen progress"
  ,"DÉBITS ET CAUSES": "RATES AND CAUSES"
  ,"VERDICT DE RÉUSSITE": "SUCCESS OUTCOME"
  ,"ÉTAT DÉTAILLÉ DES CAPTEURS": "DETAILED SENSOR STATUS"
};

export function translateText(text: string, language: DashboardLanguage): string {
  if (language === "fr") return text;
  const exact = ENGLISH[text];
  if (exact !== undefined) return exact;
  return text
    .replace(/^ENTITÉ (.+)$/u, "ENTITY $1")
    .replace(/^SOUS-SYSTÈME (.+)$/u, "SUBSYSTEM $1")
    .replace(/^Inspecter (.+)$/u, "Inspect $1")
    .replace(/^portée (.+)$/u, "range $1")
    .replace(/^(.+) éléments$/u, "$1 items")
    .replace(/^(.+) SOURCES$/u, "$1 SOURCES")
    .replace(/^SCOPE RADAR · (.+) PISTES?$/u, "RADAR SCOPE · $1 TRACKS")
    .replace(/^VERROUILLAGES · (.+)$/u, "LOCKS · $1");
}

interface I18nValue {
  language: DashboardLanguage;
  setLanguage: (language: DashboardLanguage) => void;
  t: (text: string) => string;
}

const I18nContext = createContext<I18nValue | null>(null);

function storedLanguage(): DashboardLanguage {
  try {
    return window.localStorage.getItem(STORAGE_KEY) === "en" ? "en" : "fr";
  } catch {
    return "fr";
  }
}

export function LanguageProvider({ children }: { children: ReactNode }) {
  const [language, setLanguageState] = useState<DashboardLanguage>(storedLanguage);
  const setLanguage = useCallback((next: DashboardLanguage) => {
    setLanguageState(next);
    try {
      window.localStorage.setItem(STORAGE_KEY, next);
    } catch {
      // Storage is optional; the active tab still switches immediately.
    }
  }, []);
  useEffect(() => {
    document.documentElement.lang = language;
  }, [language]);
  const value = useMemo<I18nValue>(() => ({
    language,
    setLanguage,
    t: (text) => translateText(text, language)
  }), [language, setLanguage]);
  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>;
}

export function useI18n(): I18nValue {
  const value = useContext(I18nContext);
  if (value === null) throw new Error("useI18n must be used inside LanguageProvider");
  return value;
}

export function LanguageSelector() {
  const { language, setLanguage } = useI18n();
  return (
    <label className="language-selector">
      <span className="sr-only">{language === "fr" ? "Langue" : "Language"}</span>
      <select
        aria-label={language === "fr" ? "Langue du dashboard" : "Dashboard language"}
        value={language}
        onChange={(event) => setLanguage(event.currentTarget.value as DashboardLanguage)}
      >
        <option value="fr">FR</option>
        <option value="en">EN</option>
      </select>
    </label>
  );
}
