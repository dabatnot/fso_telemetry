# 06 — Validation, sécurité et conformité

## 1. Objet

Ce document définit la stratégie de preuve de la Phase 1. Il applique le [catalogue `P1-REQ`](01-cadre-normatif-et-perimetre.md#5-catalogue-des-exigences), l'ordre de validation de la [Phase 0](../0-Contrat-de-protocole/06-validation-securite-et-conformite.md) et le [plan de tests global](../../04-implementation-roadmap.md#10-plan-de-tests).

Aucun résultat n'est présumé. Chaque test, rapport, trace, hash ou mesure décrit ci-dessous est **à produire**.

## 2. Ordre de validation obligatoire

Le récepteur DOIT conserver l'ordre Phase 0 : source/allowlist, taille réelle, header, version, flags, tailles, CRC datagramme, session/direction/endpoint, limites de classe, layout de fragment, réservation dans quota, déduplication, stockage, CRC message, payload, records, métier, état candidat, publication atomique, ACK.

Pour FSTL 1.1, il ajoute avant publication :

1. version négociée `major=1, minor=1` ;
2. bit `PLAYER_KINEMATICS` interdit en 1.0 ;
3. `state_domain_coverage` immuable après `SESSION_BEGIN` ;
4. cardinalité exacte du record-set cinématique ;
5. égalité de l'ID observé avec les clés lifecycle/flight ;
6. `ENTITY_LIFECYCLE.object_type == SHIP` ;
7. `FLIGHT_STATE.presence == 0` ;
8. absence des records Phase 2 non négociés ;
9. quaternion fini, normalisé, local-vers-monde et de signe canonique ;
10. `required_manifest_id == 0` pour ce profil minimal.

Toute violation rejette atomiquement le message logique. Aucun ACK `APPLIED` n'est émis pour un état non publié.

## 3. Matrice de tests

### 3.1 Configuration, build et mode désactivé

| Test | Oracle | Exigences |
|---|---|---|
| fichier absent, JSON tronqué, clé inconnue, mauvais type, min−1/max/max+1 | module désactivé avant socket ; diagnostic unique | `P1-REQ-006` à `P1-REQ-009` |
| loopback par défaut puis bind LAN sans allowlist | loopback accepté ; LAN refusé | `P1-REQ-007`, `P1-REQ-013` |
| double `initialize()` | un seul abonnement par événement | `P1-REQ-015` |
| build variantes supportées, enabled/disabled | compilation et linkage sans dépendance nouvelle | `P1-REQ-005`, `P1-REQ-036` |
| 100 000 callbacks disabled | seuils `P1-REQ-033`, zéro allocation/syscall récurrent | `P1-REQ-033` |

### 3.2 Compatibilité et sérialisation

Le corpus DOIT contenir :

- tous les golden vectors FSTL 1.0 d'origine, avec hashes inchangés ;
- `HELLO/WELCOME` 1.1 accepté et 1.0 rejeté par le profil Phase 1 ;
- `SESSION_STATE` avec et sans joueur observé ;
- snapshot cinématique minimal canonique ; la fragmentation multi-datagrammes reste couverte par les messages Phase 0 assez grands pour l'exiger ;
- delta cumulatif où position, quaternion, vitesse et rotation changent puis reviennent à la baseline ;
- bit `PLAYER_KINEMATICS` en session 1.0, bit réservé, record requis absent, record dupliqué, mauvais ID, `SHIP_IDENTITY` inattendu ;
- limites float32, `-0.0`, subnormaux finis, NaN, infinis, quaternion nul, non normalisé ou non canonique ;
- chaque troncature de l'en-tête et des records utilisés ;
- CRC, offsets, tailles, comptes, type/version inconnus et trailing bytes invalides.

Chaque valeur valide passe `decode(encode(x)) == canonicalize(x)` et `encode(decode(golden)) == golden`. Le décodeur indépendant et le chemin C++ DOIVENT produire le même JSON ou le même code principal de rejet. Cette section prouve `P1-REQ-001`, `002`, `012`, `020` à `025` et `034`.

### 3.3 Session, horloge et fiabilité

Le harness déterministe couvre : handshake accepté/rejeté/dupliqué/expiré, endpoint erroné, anti-amplification, ACK `VALIDATED/APPLIED`, ACK/NACK forgé ou tardif, ACK perdu, heartbeats manqués, overflow d'horloge, timeout `Stale`/long, `SessionEnd` dupliqué, session remplacée, resync répété, collision injectée de `session_id` et atteinte de la borne process sans réutilisation.

Scénarios de baseline obligatoires :

1. snapshot initial publié une seule fois après tous ses `APPLIED` ;
2. perte de plusieurs deltas puis application du plus récent ;
3. delta ancien, dupliqué ou d'une baseline inconnue ;
4. candidate reçue en désordre pendant que l'ancienne baseline reste active ;
5. mutation entre capture et ACK, visible dans le premier delta de la nouvelle baseline ;
6. ACK de snapshot perdu puis réacquittement sans rollback ;
7. expiration de candidate puis keyframe récente ;
8. `RESYNC_REQUEST` répété sans seconde candidate ni croissance mémoire.

Cette section prouve `P1-REQ-018`, `019`, `026` à `030`.

### 3.4 Transport UDP et dégradation

Chaque seed est versionnée. Les profils suivants durent au moins dix minutes chacun, séparément pour perte indépendante et pertes en rafales : 1 %, 5 % et 20 %. Ils injectent aussi duplication, désordre, jitter, coupure temporaire, fragments manquants, client lent/silencieux et `WOULD_BLOCK`.

Oracles communs :

- aucune attente ou boucle non bornée ;
- files, fenêtres, réassemblages et mémoire sous leurs maxima ;
- aucun message fiable évincé par un delta ;
- resynchronisation puis retour `Live` en dix secondes au plus après fin de l'impairment ;
- dernier état publié égal au dernier état canonique reçu ;
- compteurs de perte, drop, retransmission et resync cohérents avec l'injection.

IPv4 loopback, IPv6 loopback, source non allowlistée, mauvais port source et changement d'endpoint sont testés. Cette section prouve `P1-REQ-010` à `014`, `027`, `030`, `031` et `038`.

### 3.5 Cycle de vie et données moteur

Les scénarios couvrent connexion avant, pendant et après chargement, pause, compression temporelle, retour menu, changement de mission, shutdown normal, crash du client, nouvelle mission et relance processus. Mort, observer et respawn sont exercés seulement comme preuve de dégradation sûre et d'absence de revendication complète ; leur réplication appartient à la Phase 2.

Un oracle capturé sur le thread principal compare chaque champ Phase 1 à sa source moteur. Le quaternion est comparé après normalisation et choix de signe ; les autres valeurs utilisent leur représentation float32 canonique. Les cas `Player`, `Player_obj` ou `Player_ship` invalides ne publient aucun record joueur incohérent.

Cette section prouve `P1-REQ-015` à `017`, `021` à `025`, `029` et `037`.

## 4. Fuzzing et propriétés

Les fuzz targets séparés couvrent header/CRC, réassembleur stateful, messages de contrôle, `SESSION_STATE`, `MISSION_STATE`, `ENTITY_LIFECYCLE`, `FLIGHT_STATE`, transaction snapshot, delta/baseline et parser JSON.

Pour toute entrée : zéro crash, lecture/écriture hors borne, allocation hors budget, temps superlinéaire non borné, publication partielle, réponse amplifiante ou mutation de simulation. Les builds compatibles utilisent ASan et UBSan ; Windows utilise les équivalents disponibles. Le corpus initial contient tous les vectors 1.0 et 1.1.

Les propriétés incluent permutation/duplication de fragments, sticky error des readers/writers, déterminisme, exactitude du budget mémoire, idempotence des doublons et suffisance du dernier delta cumulatif.

## 5. Sécurité opérationnelle

### 5.1 Menaces couvertes

- corruption, troncature et incohérence de fragments ;
- spoofing LAN et changement d'endpoint ;
- amplification `HELLO`, `NACK` ou resync ;
- épuisement mémoire/CPU par clients, fragments et requêtes ;
- rejeu de session ou baseline ;
- erreur de configuration ouvrant une interface réseau ;
- tentative d'inventer un message de commande de simulation.

### 5.2 Réponses obligatoires

Le producteur applique loopback/allowlist, CSPRNG, quotas, rate limits, validation avant allocation, déduplication, anti-amplification et logs agrégés. Toute action client non définie est abandonnée ou rejetée sans atteindre une fonction moteur. Les tests DOIVENT instrumenter les points de mutation de simulation et constater zéro appel.

## 6. Observabilité et performance

Chaque métrique de `P1-REQ-031` possède nom, type, unité, labels bornés, point d'incrément et scope de reset. Les tests forcent au moins une transition de chaque compteur et vérifient son reset à la nouvelle session.

Les rapports de performance consignent révision, plateforme, CPU, OS, build, configuration, durée et seed. Ils séparent collecte, diff, sérialisation, réception, envoi et première keyframe. Les seuils de `P1-REQ-033` sont des gates ; un résultat hors seuil ne peut être masqué par une moyenne globale.

Le soak de trente minutes relève toutes les minutes : mémoire courante/pic, handles/sockets, profondeur des files, clients, baselines et temps p99. Après shutdown, le détecteur de fuite ne trouve aucun bloc vivant appartenant au module. Une nouvelle mission dans le même processus incrémente `mission_generation` et impose une nouvelle session ; après relance du processus, `session_id` diffère tandis que `mission_generation` PEUT repartir à `1` dans la nouvelle portée.

## 7. Client console et décodeur indépendant

Le décodeur indépendant NE DOIT PAS importer ou générer ses layouts depuis les structures C++ producteur. Le client console PEUT le réutiliser ; il affiche les champs de `P1-REQ-035`, marque explicitement `Synchronizing`, `Live` et `Stale`, et fournit un transcript déterministe pour le scénario nominal, le resync et l'arrêt.

Ce client n'est pas une preuve de `ReplicaStore`, d'interpolation graphique, de vue de communication ou d'ESP32.

## 8. Matrice exigences-vers-preuves

| Exigences | Preuves principales |
|---|---|
| `P1-REQ-001`–`005` | compatibilité 1.0/1.1, revue de portée, matrice build |
| `P1-REQ-006`–`009` | table config, défauts sûrs, RNG/profile |
| `P1-REQ-010`–`014` | harness UDP v4/v6, sécurité, quotas, `WOULD_BLOCK` |
| `P1-REQ-015`–`019` | tests callbacks/lifecycle/session/horloge |
| `P1-REQ-020`–`025` | vectors 1.1, oracle moteur, tests métier négatifs |
| `P1-REQ-026`–`030` | scénarios snapshot/baseline/delta/resync |
| `P1-REQ-031`–`033` | tests métriques/logs et rapports benchmark |
| `P1-REQ-034`–`036` | interop indépendante, transcript console, builds |
| `P1-REQ-037`–`038` | soak/leak, profils réseau reproductibles |

## 9. Gate de conformité

La validation Phase 1 n'est acceptée que si :

- toutes les exigences ont au moins une preuve archivée ;
- tous les vectors 1.0 sont inchangés et tous les vectors 1.1 croisés ;
- aucun défaut bloquant de fuzz, sécurité, fuite ou concurrence n'est ouvert ;
- les budgets et seuils sont respectés sur la matrice supportée ;
- le client console converge dans les scénarios nominaux et dégradés ;
- le rapport de trente minutes et de relance est concluant ;
- la revue confirme l'absence d'artefacts Phase 2+.

Jusqu'à production de ces éléments, la gate reste **ouverte**.
