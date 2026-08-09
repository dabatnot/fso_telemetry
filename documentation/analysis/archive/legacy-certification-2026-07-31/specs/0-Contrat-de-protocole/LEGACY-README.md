# Phase 0 — Contrat de protocole

## Statut

Ce dossier spécifie la **Phase 0 — Contrat de protocole** de la feuille de route de télémétrie FS2Open. Il transforme les décisions et inventaires du dossier [`analysis`](../..) en un contrat de travail normatif, testable et indépendant du moteur.

La version filaire de référence est **FSTL 1.0**. Les documents `01` à `07`, le schéma `schema/fstl-v1.yaml` et leurs artefacts forment le socle gelé octet pour octet. L'amendement additif **FSTL 1.1** requis par la Phase 1 est spécifié séparément par les [sept documents normatifs de Phase 1](../1-Squelette-et-premier-flux/README.md) et par `test/telemetry/protocol/schema/fstl-v1.1.yaml` ; il ne réécrit aucun document canonique 1.0. Le contenu est prêt à guider l'implémentation de la Phase 0, mais la phase n'est considérée terminée qu'après production, revue croisée et validation de tous les artefacts exigés : schéma machine-readable, bibliothèque de lecture/écriture bornée, golden vectors binaires, tests négatifs et décodeur indépendant.

Les mots **DOIT**, **NE DOIT PAS**, **DEVRAIT**, **NE DEVRAIT PAS** et **PEUT** expriment respectivement une obligation, une interdiction, une recommandation forte, une recommandation négative et une faculté. Ils ont le sens de RFC 2119/RFC 8174 lorsqu'ils sont écrits en majuscules.

## Résultat attendu

À la sortie de la Phase 0, deux implémentations écrites sans partager de structures C++ doivent pouvoir :

1. produire exactement les mêmes octets pour une même valeur canonique ;
2. décoder les mêmes golden vectors sur architectures little-endian et big-endian ;
3. rejeter les mêmes entrées invalides avant toute allocation proportionnelle à une donnée non validée ;
4. négocier une session FSTL 1.0 ou FSTL 1.1 et synchroniser leurs horloges monotones ;
5. réassembler, valider, acquitter et expirer les messages selon les mêmes règles ;
6. appliquer atomiquement un snapshot et des deltas cumulatifs malgré perte, duplication et désordre ;
7. ignorer sans panne les extensions explicitement compatibles ;
8. refuser proprement toute incompatibilité de version, capability, limite ou bundle ;
9. ne jamais exposer une commande capable de modifier la simulation ;
10. rester dans les budgets de mémoire, de débit et de temps définis par le contrat.

## Documents de la phase

| Document | Rôle | Statut normatif |
|---|---|---|
| [01 — Cadre normatif et périmètre](01-cadre-normatif-et-perimetre.md) | objectifs, acteurs, autorité, taxonomie des données, sécurité de principe et gate de phase | normatif |
| [02 — Format filaire et registres](02-format-filaire-et-registres.md) | scalaires, en-tête, CRC, records, versions, IDs numériques, payloads génériques et fragmentation | normatif |
| [03 — Session, horloges et fiabilité](03-session-horloges-fiabilite.md) | handshake, machine d'état, ACK/NACK, retransmission, baselines, deltas cumulatifs et resynchronisation | normatif |
| [04 — Modèle de données v1](04-modele-de-donnees-v1.md) | couverture des 28 records, champs métier, enums, optionalité, unités et granularités de delta | normatif |
| [05 — Capabilities et vues spécialisées](05-capabilities-et-vues-specialisees.md) | négociation, Talking Head par assets locaux et cible 3D H.264 | normatif |
| [06 — Validation, sécurité et conformité](06-validation-securite-et-conformite.md) | lecteur/écrivain bornés, modèle de menace, taxonomie d'erreurs, tests, fuzzing et golden vectors | normatif |
| [07 — Livraison et traçabilité](07-livraison-et-tracabilite.md) | lots de travail, dépendances, critères d'acceptation et couverture des analyses | normatif pour le processus de Phase 0 |

En cas d'écart entre un document d'analyse et ce dossier, les sept documents canoniques de ce dossier portent exclusivement la décision FSTL 1.0. Les sept documents de Phase 1 portent seuls l'amendement versionné FSTL 1.1. L'écart DOIT être enregistré dans la matrice de traçabilité et ne doit pas être résolu implicitement dans le code.

## Sources utilisées

La spécification croise l'intégralité des documents suivants :

- [`README.md`](../../README.md) : objectif, décisions actées, modes `Cockpit` et `TrustedFullState` ;
- [`01-telemetry-data-inventory.md`](../../01-telemetry-data-inventory.md) : inventaire `A/C/D/E`, champs, unités, données dérivées et exclusions ;
- [`02-telemetry-architecture.md`](../../02-telemetry-architecture.md) : découplage moteur, pipeline, autorité, threading, socket et sérialisation ;
- [`03-udp-protocol.md`](../../03-udp-protocol.md) : format proposé, négociation, fragmentation, fiabilité et sécurité ;
- [`04-implementation-roadmap.md`](../../04-implementation-roadmap.md) : livrables et gate de la Phase 0, plan de tests et risques ;
- [`05-existing-network-and-api.md`](../../05-existing-network-and-api.md) : contraintes MTU, réseau existant et limites à ne pas réutiliser ;
- [`06-communication-view.md`](../../06-communication-view.md) : assets locaux, manifeste, synchronisation et erreurs Talking Head ;
- [`07-high-resolution-target-view.md`](../../07-high-resolution-target-view.md) : rendu distant, H.264, fragmentation, récupération IDR et priorités.

L'analyse repose sur la révision FS2Open `2e57072b57f305716ba1f437b80008974d04849b`. Cette révision est une provenance, jamais un identifiant de protocole ni une dépendance d'ABI.

## Décisions structurantes figées

Les décisions suivantes s'appliquent à toute la phase :

- UDP est l'unique transport FSTL 1.0 ; le socket est dédié, dual-stack si la plateforme le permet et non bloquant.
- Un datagramme FSTL mesure au plus 1200 octets, en-tête compris ; aucune fragmentation IP n'est requise.
- Le format est little-endian, sans padding implicite, avec flottants IEEE-754 binary32 finis et chaînes UTF-8 bornées.
- L'en-tête FSTL 1.0 mesure exactement 68 octets et protège le fragment et le message logique par CRC-32/ISO-HDLC.
- Tout parseur valide les tailles, offsets, comptes, quotas et CRC avant allocation ou application.
- Les identités publiques sont stables dans une session ; aucun pointeur, handle, indice de tableau moteur ou disposition C++ n'est sérialisé.
- Un `FULL_SNAPSHOT` est autonome par rapport à l'historique dynamique, mais dépend d'un ensemble explicite de manifestes déjà validé et acquitté.
- Un `DELTA` est cumulatif depuis une baseline immuable et acquittée par `ACK APPLIED` ; il n'est jamais différentiel depuis le delta précédent.
- Les états rapides sont remplaçables ; les éléments indispensables sont fiables au niveau applicatif avec rétention bornée.
- `Cockpit` signifie état complet **autorisé** par les capteurs du producteur ; `TrustedFullState` est sensible et désactivé par défaut.
- Les vues visuelles ont des capabilities séparées de l'état canonique et du mode d'autorité.
- La vue de communication transporte identité, offset et vitesse de lecture, jamais ses pixels ni son audio.
- La vue cible transporte des access units H.264 Annex B produites par un rendu natif hors écran, jamais une image HUD agrandie.
- Les frames vidéo ont la priorité la plus faible ; aucune vidéo ne peut retarder session, ACK/NACK, événements, snapshots ou deltas.
- FSTL 1.0 n'apporte ni chiffrement ni authentification cryptographique ; il cible un LAN explicitement autorisé et reste fermé par défaut hors loopback.
- Aucun message client FSTL 1.0 ne peut modifier l'état de simulation.
- L'amendement FSTL 1.1 séparé attribue `StateDomainCoverage.PLAYER_KINEMATICS` au bit 10 (`0x0000000000000400`) sans modifier aucune valeur ni aucun artefact FSTL 1.0. Une session Phase 1 annonce ce bit seul et NE DOIT PAS annoncer `CORE_SHIP`.

## Périmètre de la Phase 0

La Phase 0 comprend :

- la définition complète des conventions et registres FSTL 1.0 ;
- le schéma de chaque message, record, enum, flag, capability, ID et champ optionnel connu ;
- les règles de session, horloge, fiabilité, fragmentation, baseline et évolution ;
- les limites, défauts, validations et comportements d'erreur ;
- les interfaces pures `PacketWriter`, `PacketReader`, fragmenter et réassembleur ;
- l'implémentation sans moteur de `HELLO`, `WELCOME`, `ACK`, `NACK`, `HEARTBEAT` et `RESYNC_REQUEST` ;
- les golden vectors et leurs métadonnées ;
- les tests unitaires, croisés, négatifs, de propriété et de fuzzing ;
- un décodeur de référence indépendant de la boucle FS2Open.

Elle exclut :

- la lecture des structures `object`, `ship`, `physics_info` ou `ai_info` ;
- les hooks HUD et renderer ;
- l'ouverture du socket dans la boucle du jeu ;
- un client graphique applicatif ;
- l'encodage/décodage H.264 effectif ;
- la distribution des bundles d'assets ;
- toute commande de simulation ;
- l'exposition sur Internet.

Les formats de messages et records des phases ultérieures sont néanmoins entièrement réservés et documentés dès la Phase 0 afin d'empêcher une évolution ad hoc du filaire.

## Gate de sortie

Le lot documentaire et contractuel Phase 1 `WP01` PEUT produire l'amendement FSTL 1.1 nécessaire à la fermeture de cette gate. En revanche, `WP02`, tout hook moteur, collecteur, socket producteur ou client applicatif de Phase 1 NE DOIT PAS commencer tant que les conditions suivantes ne sont pas toutes satisfaites :

- aucun champ, enum, bit, unité, sentinelle, limite ou règle d'absence FSTL 1.0 n'est implicite ;
- le schéma machine-readable et les tableaux normatifs concordent ;
- chaque message et chaque record possède au moins un golden vector valide ;
- les cas limites et invalides obligatoires possèdent des fixtures et une décision de rejet ;
- deux décodeurs indépendants passent les mêmes fixtures ;
- le fuzzing du lecteur, du réassembleur et des décodeurs de payload ne révèle ni crash, ni dépassement, ni allocation non bornée ;
- le scénario baseline/delta cumulatif avec perte, désordre et ACK perdu converge ;
- les revues sécurité, compatibilité, données métier et transport sont approuvées ;
- les divergences avec les analyses sont documentées ;
- le commit de Phase 0 ne contient aucun collecteur moteur ni code applicatif de phase ultérieure.

Le détail vérifiable de cette gate figure dans [07 — Livraison et traçabilité](07-livraison-et-tracabilite.md).
