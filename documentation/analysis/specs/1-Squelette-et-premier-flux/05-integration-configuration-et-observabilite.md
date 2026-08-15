# 05 — Intégration, configuration et observabilité

## 1. Objet

Ce document fixe l'intégration build, les schémas JSON, l'ouverture des sockets, les budgets, les métriques, les logs et les mesures de performance de la **Phase 1 — Squelette et premier flux**. Les choix de sécurité sont fail-closed et appliquent `D1-007` à `D1-011` et `D1-014`.

Les noms de cibles et fichiers nouveaux sont proposés ; les deux seams upstream de [02](02-architecture-contrats-et-interfaces.md#2-seams-upstream-autorisés), les clés JSON, les bornes et le catalogue de métriques sont normatifs.

## 2. État du dépôt et points d'intégration

L'implémentation DOIT partir des faits vérifiés suivants :

- `code/source_groups.cmake` construit la liste `source_files` et ne contient pas encore de groupe `Telemetry` ;
- `code/CMakeLists.txt` inclut cette liste, ajoute `telemetry/protocol`, construit la librairie code et lie déjà `fstl_protocol` et Jansson ;
- `code/telemetry/protocol` fournit la librairie statique C++17 `fstl_protocol` ;
- `freespace2/freespace.cpp` initialise PSNET avant la boucle et émet `EngineUpdate`/`EngineShutdown` ;
- un décodeur Python indépendant existe déjà pour le protocole.

Ces faits n'autorisent pas à modifier PSNET, le gestionnaire CFile ou le système d'événements. Les nouvelles dépendances s'insèrent derrière le groupe `Telemetry` et l'appel `telemetry::initialize()`.

## 3. Intégration CMake et build

### 3.1 Groupe producteur

`code/source_groups.cmake` DOIT :

1. déclarer les headers/sources sous `code/telemetry/` hors `protocol/` ;
2. créer `source_group("Telemetry" FILES ...)` ;
3. ajouter ces fichiers à `source_files` une seule fois ;
4. ne changer aucun flag global, variant ou ordre de librairie sans nécessité démontrée.

Le code producteur rejoint la librairie code existante. `code/CMakeLists.txt` lie déjà `fstl_protocol` et Jansson ; aucune nouvelle librairie externe, aucun téléchargement et aucun gestionnaire de paquets ne sont ajoutés. Les sockets utilisent les bibliothèques plateforme déjà liées par le moteur.

### 3.2 Outils de référence

Les cibles proposées sont séparées :

| Artefact | Emplacement proposé | Règle de dépendance |
|---|---|---|
| schéma, fixtures et golden vectors wire | `test/telemetry/protocol/` existant | données versionnées, hashes FSTL 1.0 archivés et inchangés |
| décodeur indépendant | `test/telemetry/protocol/tools/fstl_reference_decoder.py` existant | NE DOIT PAS lier le parser, DTO ou codec C++ producteur |
| client console | `test/telemetry/protocol/tools/fstl_console_client.py` proposé | DOIT consommer le module indépendant utilisé par le décodeur de référence |

Le décodeur indépendant PEUT rester Python ou être un outil séparé. Les outils sont conditionnés par `FSO_BUILD_TOOLS` ou une option télémétrie dédiée sans modifier le build normal.

### 3.3 Compatibilité de build

Le module reste compatible avec les variantes supportées du dépôt, avec ou sans outils, sans changer le comportement d'une build où la télémétrie est désactivée.

## 4. Schémas JSON fermés

### 4.1 Fichier de configuration

Le chemin logique est `data/config/telemetry.json`. La Phase 1 accepte uniquement un fichier **loose** : elle appelle `cf_find_file_location("telemetry.json", CF_TYPE_CONFIG, CF_LOCATION_ALL)`, exige `found=true` et `offset=0`, puis ouvre exactement cette localisation avec CFile et Jansson. Un `.json` présent uniquement dans un VP n'est pas indexé par le pathtype actuel et est traité comme absent. Le module n'ajoute pas `.json` à une whitelist globale et ne modifie pas `cfile.cpp`.

Exemple complet normatif :

```json
{
  "schemaVersion": 1,
  "enabled": false,
  "bindAddresses": ["127.0.0.1", "::1"],
  "bindPort": 42042,
  "allowedClients": ["127.0.0.1/32", "::1/128"],
  "discoveryEnabled": false,
  "visibilityMode": "Cockpit",
  "maxClients": 1,
  "flightHz": 30,
  "keyframeSeconds": 2,
  "missionHeartbeatMs": 500,
  "idleHeartbeatMs": 1000,
  "maxDatagramsPerTick": 64
}
```

L'objet racine DOIT être un objet JSON sans clés dupliquées. `schemaVersion` est obligatoire si le fichier existe ; les autres clés sont optionnelles et prennent les défauts ci-dessus. Aucune conversion implicite string/nombre/booléen n'est permise. Les nombres sont des entiers JSON exactement représentables dans le type cible.

| Clé | Type | Défaut | Domaine Phase 1 |
|---|---|---|---|
| `schemaVersion` | entier | aucun si fichier présent | exactement `1` |
| `enabled` | booléen | `false` | `false` ou `true` |
| `bindAddresses` | tableau de strings | `127.0.0.1`, `::1` | 1–2 littéraux IPv4/IPv6 numériques, uniques |
| `bindPort` | entier | `42042` | 1024–65535 |
| `allowedClients` | tableau de strings | loopback v4/v6 | 1–32 CIDR numériques canoniques et uniques lorsque activé |
| `discoveryEnabled` | booléen | `false` | exactement `false` en Phase 1 |
| `visibilityMode` | string | `Cockpit` | exactement `Cockpit` |
| `maxClients` | entier | `1` | 1–4 |
| `flightHz` | entier | `30` | 1–60 |
| `keyframeSeconds` | entier | `2` | 1–5 |
| `missionHeartbeatMs` | entier | `500` | 200–5000 |
| `idleHeartbeatMs` | entier | `1000` | 200–5000 |
| `maxDatagramsPerTick` | entier | `64` | 1–256 |

Toute clé inconnue, duplication, type incorrect, valeur hors domaine, adresse non numérique, zone IPv6, hostname, CIDR invalide ou tableau vide invalide le fichier entier. Aucun champ n'est appliqué partiellement.

Une écoute non-loopback est explicite seulement si `enabled=true`, que chaque adresse est écrite littéralement et que `allowedClients` est non vide. Une entrée wildcard (`0.0.0.0` ou `::`) n'est jamais ajoutée par défaut ; si elle est écrite, l'allowlist ne peut contenir un catch-all (`0.0.0.0/0`, `::/0`) en Phase 1. Une allowlist ne transforme pas FSTL en protocole authentifié.

### 4.2 Profil d'identité producteur

Le chemin logique proposé est `data/config/telemetry-profile.json`, exclusivement dans le stockage écrivable de l'installation. En mode normal, toutes les recherches, créations, suppressions et renommages utilisent `CF_TYPE_CONFIG` avec `CF_LOCATION_ROOT_USER | CF_LOCATION_TYPE_ROOT`. En mode portable, où la racine utilisateur seule n'existe pas, elles utilisent `CF_LOCATION_ROOT_GAME | CF_LOCATION_TYPE_ROOT`. Un profil homonyme dans un mod ou un VP est ignoré :

```json
{
  "schemaVersion": 1,
  "producerId": "1844674407370955161"
}
```

Le schéma est fermé. `producerId` est une chaîne décimale canonique sans signe ni zéro initial, convertie avec contrôle d'overflow vers un `u64` non nul. Le format string évite la perte de précision JSON.

Si le profil est absent et la télémétrie activée, le module génère l'ID depuis la source d'entropie OS, écrit un fichier temporaire dans le même répertoire, force sa fermeture puis effectue un remplacement atomique. L'échec de RNG, création, écriture, flush, rename, relecture ou validation désactive le module avant socket. Un profil présent invalide n'est jamais écrasé automatiquement. Le `producer_id` n'est jamais dérivé de l'hôte, de l'adresse MAC ou de données joueur.

Chaque `session_id` utilise la même source d'entropie OS, est non nul, imprévisible et comparé à l'ensemble des IDs déjà utilisés dans le processus. Cet ensemble est préalloué à la constante `MaxSessionIdsPerProcess = 65 536`; après 16 tirages en collision ou lorsque cette limite est atteinte, le runtime refuse toute nouvelle session et passe `Faulted`, sans réutiliser un ID. Aucun fallback horloge/PRNG faible n'est autorisé. Cette limite est une borne de sûreté, pas une politique d'éviction.

### 4.3 Ordre de chargement et résultat

1. rechercher `telemetry.json` ;
2. absent : `Disabled`, zéro socket, zéro log récurrent ;
3. refuser un fichier de plus de 16 Kio, parser avec `JSON_REJECT_DUPLICATES`, exiger EOF après l'objet racine et une profondeur maximale de quatre niveaux ; le profil est limité à 1 Kio et deux niveaux ;
4. valider schéma, types, bornes et relations croisées ;
5. si `enabled=false` : `Disabled` sans charger le profil ;
6. charger/créer et relire le profil ;
7. générer l'identité de session initiale, calculer tous les budgets avec arithmétique vérifiée ;
8. préallouer ;
9. ouvrir les sockets transactionnellement.

Une erreur avant l'étape 9 implique zéro socket. Une erreur à l'étape 9 ferme tout socket déjà ouvert. Au plus un diagnostic de démarrage agrégé est émis, avec une enum de raison et sans chemin absolu. Les fichiers ne sont relus qu'au prochain démarrage du processus.

## 5. Bind et transport dédié

### 5.1 Cas par défaut

Le défaut ouvre transactionnellement deux sockets privés :

- `AF_INET`, `SOCK_DGRAM`, `IPPROTO_UDP`, bind `127.0.0.1:42042` ;
- `AF_INET6`, `SOCK_DGRAM`, `IPPROTO_UDP`, `IPV6_V6ONLY=1`, bind `[::1]:42042`.

Chaque appel `socket`, option, bind, passage non bloquant et vérification de l'adresse effective est contrôlé. Sur Windows, le passage non bloquant utilise l'abstraction/`FIONBIO` déjà disponible après `psnet_init()` ; sur POSIX, l'équivalent non bloquant conserve les flags existants. Aucun socket n'est publié au runtime tant que les deux binds demandés ne sont pas réussis.

### 5.2 Dual-stack et adresses explicites

Une configuration contenant une adresse IPv6 wildcard PEUT demander un unique socket dual-stack. Le module règle `IPV6_V6ONLY=0`, relit l'option avec `getsockopt` et vérifie par tests v4/v6. Si la plateforme refuse ou ment, le bind échoue fermé ; il ne crée pas implicitement `0.0.0.0`. Deux sockets sont utilisés seulement lorsque les deux adresses correspondantes sont présentes explicitement.

Une adresse spécifique non-loopback ouvre un socket de la famille correspondante et lie exactement cette adresse. Résolution DNS, sélection automatique d'interface, broadcast, multicast et découverte sont interdits. Le module ne reçoit ni n'envoie via `psnet_send()`, `multi_io_send()` ou le socket multijoueur.

### 5.3 Buffers et I/O

Les buffers datagrammes sont préalloués à 1200 octets. Les tailles socket OS PEUVENT être ajustées à des constantes documentées, mais leur échec non critique doit être mesuré et ne change aucune borne applicative. Chaque `try_receive`/`try_send` effectue au plus un syscall. `WOULD_BLOCK` termine la direction pour le tick selon [03](03-flux-cycle-de-vie-et-concurrence.md#62-budget-commun-de-datagrammes).

Un datagramme tronqué ou dont la longueur excède 1200 octets est rejeté avant parse. L'endpoint source est copié dans une structure valeur canonique IPv4/IPv6 ; padding et scope non pertinents sont normalisés avant comparaison.

## 6. Validation, quotas et budgets

### 6.1 Ordre d'ingress

L'ordre Phase 0 reste impératif : longueur/en-tête/magic/version/type/flags, CRC, endpoint et allowlist, session et séquences, quotas/anti-amplification, puis seulement allocation bornée, défragmentation, décodage métier et mutation. Une erreur arrête le pipeline et incrémente exactement une raison primaire.

Avant validation de l'adresse source, aucune réponse n'est envoyée. Avant validation du `HELLO`, le total des octets retournés à un endpoint ne dépasse pas trois fois les octets validés reçus. Les rate limits et budgets précèdent toute réservation proportionnelle au contenu annoncé.

### 6.2 Bornes héritées et calculées

| Budget | Borne |
|---|---:|
| datagramme / en-tête / payload | 1200 / 68 / 1132 octets |
| message d'état | 1 Mio |
| fragments par message | 1024 |
| réassemblages actifs | 4 par client |
| mémoire réassemblage | 4 Mio par client |
| clients | 1–4 |
| identifiants de session sur la durée du processus | 65 536, sans éviction ni réutilisation |
| baseline | 1 active + 1 candidate par client |
| dernier delta | 1 par baseline/client |
| datagrammes ingress + egress | 1–256 par tick au total |

Le démarrage calcule les maxima avec additions/multiplications vérifiées dans `size_t` et dans le type de métrique. Il vérifie notamment `maxClients × 4 Mio`, slots de réassemblage, fenêtres fiables, buffers, deux baselines et serialization scratch. Un overflow, une allocation supérieure au plafond statique compilé ou un échec de préallocation désactive avant bind.

Le runtime ne réduit pas silencieusement une configuration valide faute de mémoire. Les high-water marks restent inférieurs ou égaux aux bornes ; atteindre une borne provoque drop/rejet/remplacement selon la classe, jamais croissance.

## 7. Surface d'observabilité

L'observabilité Phase 1 est locale et en lecture seule. Elle comprend :

1. une structure `TelemetryMetricsSnapshot` copiable par le client diagnostic local ;
2. des compteurs/gauges/histogrammes en mémoire à cardinalité fixe ;
3. un résumé agrégé au démarrage réussi, à la fin d'une session et au shutdown ;
4. des sources d'horloge et compteurs d'allocation isolés du comportement métier.

Aucun endpoint de métriques réseau, aucune commande distante, aucun dump par frame et aucun label contrôlé par le pair n'est introduit. Les enums de raisons sont fermées et stables.

### 7.1 Types, unités et scopes

- **counter** : `u64` monotone dans son scope, addition saturante avec compteur d'overflow ;
- **gauge** : valeur courante, remise à la valeur neutre à la fin du scope ;
- **high-water** : maximum observé dans le scope ;
- **histogram** : compteur par buckets fixes en microsecondes `0–5`, `>5–10`, `>10–25`, `>25–50`, `>50–100`, `>100–250`, `>250–500`, `>500–1000`, `>1000`, plus count/sum saturants.

Scopes : `process` jusqu'au shutdown ; `mission` remis à zéro à chaque `GameMissionLoad`/sortie ; `session` remis à zéro lors de l'allocation d'un slot. Les totaux process correspondants ne sont jamais remis à zéro avant shutdown.

## 8. Catalogue normatif des métriques

Toutes les métriques suivantes DOIVENT être présentes. Les suffixes `_total`, `_bytes`, `_us`, `_active` et `_high_water` portent leur unité. Les familles `{reason}`, `{direction}`, `{result}` et `{kind}` utilisent exclusivement les enums fermées décrites après le tableau.

| Nom | Type | Scope | Incrément / valeur exacte |
|---|---|---|---|
| `telemetry_runtime_state` | gauge enum | process | état global courant |
| `telemetry_callbacks_total{kind}` | counter | process | entrée de chacun des cinq callbacks |
| `telemetry_callback_duration_us{kind}` | histogram | process | durée complète du callback |
| `telemetry_runtime_faults_total{reason}` | counter | process | transition vers `Faulted` |
| `telemetry_allocations_total{kind}` | counter | process | allocation réussie par runtime/config/session/scratch |
| `telemetry_allocation_failures_total{kind}` | counter | process | allocation échouée |
| `telemetry_allocated_bytes` | gauge | process | octets actuellement possédés |
| `telemetry_allocated_bytes_high_water` | high-water | process | pic d'octets possédés |
| `telemetry_udp_sockets_open` | gauge | process | handles privés ouverts, 0–2 |
| `telemetry_datagrams_total{direction,result}` | counter | process | tentative receive/send classée une fois |
| `telemetry_datagram_bytes_total{direction}` | counter | process | octets validement reçus ou envoyés |
| `telemetry_would_block_total{direction}` | counter | process | syscall retournant `WouldBlock` |
| `telemetry_datagram_drops_total{reason}` | counter | process | datagramme abandonné avant effet métier |
| `telemetry_validation_errors_total{reason}` | counter | process | raison primaire du pipeline Phase 0 |
| `telemetry_allowlist_drops_total` | counter | process | source hors allowlist, réponse nulle |
| `telemetry_rate_limit_drops_total{kind}` | counter | process | quota/rate limit ayant refusé l'action |
| `telemetry_anti_amplification_drops_total` | counter | process | réponse interdite par budget `3×` |
| `telemetry_sessions_started_total` | counter | process | slot devenu session négociée |
| `telemetry_sessions_ended_total{reason}` | counter | process | session purgée, une raison finale |
| `telemetry_clients_active` | gauge | process | slots actifs, 0–`maxClients` |
| `telemetry_clients_active_high_water` | high-water | process | pic de slots actifs |
| `telemetry_session_state` | gauge enum | session | état machine Phase 0 du slot |
| `telemetry_heartbeat_probes_total{direction}` | counter | session + total process | sonde émise/reçue valide |
| `telemetry_heartbeat_samples_total{result}` | counter | session + total process | mesure acceptée/rejetée |
| `telemetry_heartbeat_rtt_us` | gauge | session | RTT de l'échantillon minimal courant |
| `telemetry_heartbeat_offset_us` | gauge signé | session | offset lissé courant |
| `telemetry_fragments_total{direction,result}` | counter | session + total process | fragment validé/émis/rejeté |
| `telemetry_reassemblies_active` | gauge | session | réassemblages actifs, 0–4 |
| `telemetry_reassemblies_active_high_water` | high-water | session | pic de réassemblages |
| `telemetry_reassembly_bytes` | gauge | session | octets actuellement réservés, ≤4 Mio |
| `telemetry_reassembly_bytes_high_water` | high-water | session | pic d'octets réservés |
| `telemetry_reassembly_expired_total` | counter | session + total process | réassemblage expiré |
| `telemetry_acks_total{result}` | counter | session + total process | ACK reçu/émis classé |
| `telemetry_nacks_total{result}` | counter | session + total process | NACK reçu/émis classé |
| `telemetry_retransmissions_total{kind}` | counter | session + total process | datagramme fiable retransmis |
| `telemetry_reliable_window_items` | gauge | session | éléments fiables en vol |
| `telemetry_reliable_window_items_high_water` | high-water | session | pic de fenêtre fiable |
| `telemetry_resync_requests_total{result}` | counter | session + total process | resync reçu, dédupliqué, limité ou servi |
| `telemetry_capture_attempts_total{result}` | counter | mission + total process | capture `Valid`, `NoPlayer` ou `InvalidSource` |
| `telemetry_capture_duration_us` | histogram | mission + total process | lecture, validation et copie moteur |
| `telemetry_diff_duration_us` | histogram | mission + total process | canonicalisation et diff cumulatif |
| `telemetry_serialization_duration_us{kind}` | histogram | session + total process | encodage contrôle/snapshot/delta |
| `telemetry_network_duration_us` | histogram | process | temps socket total du tick |
| `telemetry_tick_duration_us` | histogram | process | travail télémétrie total du tick |
| `telemetry_current_player_entity_id` | gauge u64 | mission | ID observé ou 0 si absent |
| `telemetry_player_discontinuities_total{reason}` | counter | mission + total process | perte/changement/source invalide |
| `telemetry_snapshots_created_total{kind}` | counter | session + total process | initial/keyframe/resync créé |
| `telemetry_snapshots_applied_total{kind}` | counter | session + total process | promotion après ACK APPLIED complet |
| `telemetry_snapshot_candidates` | gauge | session | 0 ou 1 |
| `telemetry_baselines_active` | gauge | session | 0 ou 1 |
| `telemetry_deltas_created_total` | counter | session + total process | état cumulatif calculé |
| `telemetry_deltas_replaced_total` | counter | session + total process | ancien delta remplacé avant émission |
| `telemetry_deltas_dropped_total{reason}` | counter | session + total process | delta abandonné |
| `telemetry_pending_items{kind}` | gauge | session | éléments des ensembles bornés |
| `telemetry_pending_items_high_water{kind}` | high-water | session | pic par classe bornée |
| `telemetry_budget_exhaustions_total{kind}` | counter | process | plafond tick/client/mémoire atteint |

Enums fermées minimales :

- callback `kind` : `EngineUpdate`, `EngineShutdown`, `GameMissionLoad`, `GameEnterState`, `GameLeaveState` ;
- `direction` : `Rx`, `Tx` ;
- résultat I/O : `Complete`, `WouldBlock`, `Closed`, `Error` ;
- résultat capture : `Valid`, `NoPlayer`, `InvalidSource` ;
- snapshot `kind` : `Initial`, `Periodic`, `Resync` ;
- pending `kind` : `Reliable`, `Reassembly`, `SnapshotCandidate`, `Delta`, `ResyncIntent`, `HeartbeatProbe` ;
- drop delta : `Replaced`, `StaleBaseline`, `UnknownBaseline`, `SessionEnd`, `WouldBlock` ;
- fin session : `Timeout`, `MissionDiscontinuity`, `PeerClosed`, `ProtocolError`, `TransportError`, `Shutdown` ;
- runtime fault : `Config`, `Entropy`, `IdentityStore`, `BudgetOverflow`, `Allocation`, `Socket`, `Invariant` ;
- validation/drop/rate-limit/ACK/NACK/retransmission/result : reprendre exactement les enums Phase 0. Le mauvais mineur utilise `ValidationError::UnsupportedMinor` (`UNSUPPORTED_MINOR`) et le bit `PLAYER_KINEMATICS` sous FSTL 1.0 utilise `ValidationError::ReservedFlag` (`RESERVED_FLAG`); le refus de handshake utilise séparément `WelcomeStatus::UnsupportedVersion`. FSTL 1.1 n'ajoute aucune valeur d'erreur ad hoc.

Les métriques `session + total process` ont deux stockages : valeur du slot remise à zéro et agrégat process monotone. Il est interdit d'utiliser `session_id`, IP, port, entity ID arbitraire, type de message inconnu ou texte reçu comme label.

## 9. Logs normatifs

| Événement | Niveau | Fréquence maximale | Contenu autorisé |
|---|---|---:|---|
| config absente/désactivée | info/debug selon conventions moteur | une fois au démarrage | statut et schéma |
| config/profil invalide | warning | une fois au démarrage | enum raison, clé connue concernée sans valeur sensible |
| activation réussie | info | une fois | familles v4/v6, port, limites effectives, version 1.1 |
| bind/fault transport | error | une fois par transition `Faulted` | famille, opération, code plateforme normalisé |
| session ouverte/fermée | info | une fois par transition | slot ordinal local, raison, durées et totaux agrégés |
| mission entrée/sortie | info | une fois par transition | compteur de mission, durée et totaux |
| drops/validation/rate limits | warning agrégé | au plus une fois par seconde globalement | compte par enum depuis le dernier résumé |
| budget/high-water | warning agrégé | première atteinte puis résumé session | nom fermé, limite et high-water |
| shutdown | info | une fois | durée process et snapshot final des totaux |

Les logs ne contiennent ni payload ou fragment complet, dump JSON reçu, sortie par frame/datagramme, chemin absolu, adresse IP complète d'un pair, secret, donnée cachée, callsign, nom joueur, pointeur, handle ou octets d'identité aléatoire. Un code socket est normalisé ; le message libre de l'OS n'est pas relayé tel quel.

`producer_id` et `session_id` ne sont pas inscrits dans les logs de livraison. Un identifiant de corrélation local séquentiel de slot est suffisant.

## 10. Travail borné et allocations

### 10.1 Fast path désactivé

Après le premier diagnostic éventuel, chaque `EngineUpdate` désactivé effectue seulement une lecture de l'état et un branchement. Il ouvre zéro socket, effectue zéro syscall, allocation et log récurrent. Un test déterministe vérifie ces observables directement.

### 10.2 Runtime actif

Collecte, diff, sérialisation et réseau sont non bloquants et bornés par les cardinalités et budgets configurés. Les buffers, slots, fenêtres et scratch sont préalloués avant `Ready`. En steady-state, `telemetry_allocations_total` ne doit pas augmenter sans création/destruction de session.

La première keyframe est observée séparément avec ses allocations et sa taille. Un hitch visible lors de l'observation produit est signalé avec ses conditions et son impact, sans seuil temporel automatique.

### 10.3 Relevé produit

Le relevé indique la build, la plateforme, la mission, `flightHz`, le nombre de clients et la taille des états. Il consigne attendu, observé, écart et impact. Aucun percentile, microbenchmark ou campagne longue n'est un critère de livraison.

## 11. Matrice configuration / résultat

| Cas | Runtime | Socket | Diagnostic |
|---|---|---:|---|
| fichier absent | `Disabled` | 0 | au plus un info/debug |
| JSON invalide / clé inconnue / borne invalide | `Disabled` | 0 | un warning agrégé |
| `enabled=false` valide | `Disabled` | 0 | au plus un info |
| profil invalide ou non persistant | `Faulted` | 0 | un error/warning de démarrage |
| RNG échoue | `Faulted` | 0 | un error de démarrage |
| budget overflow/allocation échoue | `Faulted` | 0 | un error de démarrage |
| défaut valide loopback | `Ready` | 2 | un info agrégé |
| dual-stack explicitement vérifié | `Ready` | 1 | un info agrégé |
| bind partiel | `Faulted` après rollback | 0 | un error |
| non-loopback sans allowlist | `Disabled` | 0 | un warning config |
| découverte activée | `Disabled` | 0 | un warning config |
| erreur socket permanente en cours | `Faulted` | 0 après purge | un error puis résumé |

## 12. Qualité de l'observabilité

- chaque métrique expose unité, portée et valeur courante ;
- les gauges reviennent à zéro après purge ;
- les high-water restent sous les bornes ;
- les compteurs saturent sans overflow ;
- une entrée hostile ne crée ni label ni allocation non bornée ;
- les logs respectent les règles de confidentialité ;
- les échantillons nécessaires aux percentiles restent accessibles.

## 13. Traçabilité

| Exigence | Couverture |
|---|---|
| `P1-REQ-005` | sections 2 et 3, groupe CMake et seam unique |
| `P1-REQ-006`–`009` | section 4 et matrice 11 |
| `P1-REQ-010`–`014` | sections 5 et 6 |
| `P1-REQ-015`–`019` | ordre de démarrage, bind après PSNET et métriques lifecycle/session |
| `P1-REQ-020`–`030` | métriques capture, snapshot, baseline, delta et resync |
| `P1-REQ-031` | sections 7, 8 et 12 : catalogue exact, unités et resets |
| `P1-REQ-032` | section 9 : agrégation et contenu interdit |
| `P1-REQ-033` | section 10 : seuils et protocole reproductible |
| `P1-REQ-034`–`035` | section 3.2 : outils séparés et dépendance indépendante |
| `P1-REQ-036` | section 3 : variantes et aucune dépendance externe |
| `P1-REQ-037`–`038` | sections 10.3 et 12, session représentative et récupération |
