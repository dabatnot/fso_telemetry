# Contrat v1.0 — Session, horloges, fiabilité et réplication

## 1. Statut et portée

Ce document est normatif. Il définit le cycle de vie d'une session v1.0 et, sans altérer celui-ci, l'amendement de négociation v1.1 nécessaire au profil Phase 1. Il couvre les horloges, les séquences, la fiabilité au-dessus d'UDP, les transactions paginées, les snapshots, les deltas cumulatifs, les resynchronisations, les délais, les limites de débit et les priorités.

Les mots **DOIT**, **NE DOIT PAS**, **DEVRAIT**, **NE DEVRAIT PAS** et **PEUT** ont le sens normatif défini dans [02 — Format filaire et registres](02-format-filaire-et-registres.md).

Ce document s'appuie sur :

- [la proposition UDP](../../03-udp-protocol.md) ;
- [la feuille de route d'implémentation](../../04-implementation-roadmap.md) ;
- [le format filaire v1.0](02-format-filaire-et-registres.md) ;
- [le modèle de données v1](04-modele-de-donnees-v1.md) ;
- [les capabilities et vues spécialisées](05-capabilities-et-vues-specialisees.md).

## 2. Transport, endpoint et configuration réseau

### 2.1 Paramètres v1.0

| Paramètre | Défaut | Règle |
|---|---:|---|
| port UDP | `42042` | configurable |
| taille maximale du datagramme | 1200 octets | non relevable en v1.0 |
| découverte | désactivée | activation explicite |
| broadcast | désactivé | jamais implicite |
| adresse d'écoute | loopback | exposition LAN explicite + allowlist non vide |
| transport | UDP | unique |
| simulation distante | lecture seule | aucune commande de jeu |

Le socket producteur **DOIT** être non bloquant. L'implémentation **DEVRAIT** accepter IPv4 et IPv6 via un socket dual-stack lorsque la plateforme le permet. Une erreur `WouldBlock` entraîne une politique d'abandon ou de remise en file bornée ; elle ne bloque jamais la frame du jeu.

La découverte, lorsqu'elle est activée, envoie `Discovery` toutes les 2 secondes, avec un jitter déterministe de ±10 %, uniquement vers les destinations unicast ou multicast configurées. Elle n'ouvre pas de session et ne remplace ni l'allowlist ni `Hello`.

### 2.2 Identité et endpoint

`producer_id` est un `u64` non nul persistant dans le profil producteur. Il identifie la même installation configurée entre deux activations. Il ne constitue ni un secret ni une session.

Une session active est indexée par :

~~~text
(session_id, adresse IP source, port UDP source)
~~~

Après `Welcome Accepted`, tout datagramme d'une session reçu d'un autre endpoint est rejeté avant réassemblage. Le rebinding NAT et la migration d'adresse ne sont pas négociés en v1.0 ; le client ouvre une nouvelle session par `Hello`.

Le producteur filtre les sources selon sa configuration. Un datagramme provenant d'une source non autorisée est abandonné sans réponse afin de ne pas servir d'amplificateur.

### 2.3 session_id

Le producteur génère `session_id` avec un générateur aléatoire cryptographiquement sûr :

- valeur non nulle ;
- imprévisible ;
- différente de toute session active ;
- jamais réutilisée pendant la durée du processus.

Un redémarrage peut théoriquement reproduire une valeur ; la combinaison avec `producer_id`, le nouveau handshake et l'endpoint empêche de rattacher automatiquement un ancien état. Le client détruit tous les réassemblages, baselines, transactions, estimations d'horloge et sous-états spécialisés dès qu'il accepte un nouveau `session_id`.

## 3. Portée et wrap des identifiants

### 3.1 Tableau normatif

| Identifiant | Portée | Initialisation | Incrément / wrap |
|---|---|---|---|
| `packet_sequence:u32` | direction + session | valeur aléatoire quelconque | +1 par datagramme, modulo 2³² |
| `message_id:u32` | direction + session | 1 | +1 par message logique ; aucun wrap dans une session |
| `frame_id:u32` | producteur + session | 1 ; zéro si non applicable | +1 par capture logique ; aucun wrap |
| `manifest_id:u32` | producteur → client + session | 1 | strictement croissant ; aucun wrap |
| `snapshot_id:u32` | producteur → client + session | 1 | strictement croissant ; aucun wrap |
| `delta_sequence:u32` | baseline | 1 | strictement croissant ; nouvelle baseline avant wrap |
| `batch_id:u32` | producteur → client + session | 1 | strictement croissant ; aucun wrap |
| `event_id:u64` | session | 1 | strictement croissant ; aucun wrap |
| `probe_id:u32` | initiateur + session | 1 | modulo 2³², sans collision en vol |
| `request_id:u32` | famille de requête + direction + session | 1 | aucune réutilisation dans la fenêtre de déduplication |
| `capability_generation:u32` | émetteur + session | 1 | strictement croissant ; aucun wrap |
| `advert_sequence:u32` | `producer_id` + activation | aléatoire | modulo 2³² |

Les identifiants spécialisés `playback_id`, `stream_id`, `config_generation` et `video_frame_id` suivent le document 05.

Un compteur déclaré « aucun wrap » provoque une nouvelle session avant d'émettre sa valeur maximale puis de revenir à zéro. Zéro est réservé comme « inconnu/non applicable » pour tous ces compteurs, sauf `packet_sequence` et `advert_sequence`.

### 3.2 Comparaison sérielle

`packet_sequence` et `advert_sequence` utilisent l'arithmétique sérielle :

~~~text
newer(a, b) = (a != b) && (u32(a - b) < 0x80000000)
~~~

Un écart d'au moins 2³¹ est ambigu et ne sert pas à estimer la perte. Les compteurs sans wrap se comparent comme des entiers non signés ordinaires.

Une retransmission conserve `message_id`, `message_size` et `message_crc32` mais reçoit un nouveau `packet_sequence`.

### 3.3 Cas pré-session

`Hello` porte `session_id = 0`. Sa déduplication utilise :

~~~text
(endpoint, client_nonce)
~~~

Le producteur conserve le résultat d'un handshake accepté ou rejeté pendant 10 secondes. Un `Hello` identique reçu dans cette fenêtre renvoie le même résultat et, s'il avait été accepté, le même `session_id` ; il ne crée pas une deuxième session.

Les compteurs pré-session ont des portées explicites :

- `Discovery` : `packet_sequence` et `message_id` appartiennent au canal sortant `(producer_id, activation du processus)` ; chaque annonce est un nouveau message logique ;
- `Hello` : `packet_sequence` appartient à `(endpoint producteur, client_nonce)` et progresse à chaque tentative ; les retransmissions du même `Hello` gardent son `message_id` ;
- `Welcome` rejeté : compteurs du canal `(endpoint client, client_nonce, activation producteur)` ;
- `Welcome` accepté : les compteurs appartiennent déjà au nouveau `session_id`.

Chaque canal pré-session initialise `packet_sequence` aléatoirement et `message_id` à 1. Un canal est détruit après 10 secondes d'inactivité. Il n'existe donc aucun wrap pré-session ambigu.

## 4. Négociation et machine d'état

### 4.1 États client

~~~mermaid
stateDiagram-v2
    [*] --> Disconnected
    Disconnected --> Negotiating: Hello
    Negotiating --> Synchronizing: Welcome Accepted
    Negotiating --> Disconnected: Welcome rejeté ou timeout
    Synchronizing --> Live: SessionBegin + manifestes requis + snapshot conforme
    Synchronizing --> Stale: timeout ou erreur
    Live --> Stale: heartbeats manqués ou baseline inconnue
    Stale --> Synchronizing: ResyncRequest accepté
    Live --> Disconnected: SessionEnd ou timeout long
    Stale --> Disconnected: timeout long
~~~

Le client ne publie jamais un état « complet » avant :

1. sélection de la version et du mode de visibilité ;
2. installation des manifestes requis ;
3. commit atomique d'un `FullSnapshot` autonome ;
4. émission des ACK `APPLIED` correspondants.

En `Stale`, le client peut conserver la dernière réplique pour l'affichage, mais il doit la marquer explicitement périmée et ne peut appliquer aucun delta dont la baseline n'est pas validée.

Les sous-états communication et vidéo sont indépendants. Leur indisponibilité n'empêche pas la télémétrie numérique de rester `Live`.

### 4.2 États producteur

~~~text
Listening -> Negotiating -> Synchronizing -> Live -> Closing
                         \-> Rejected
Live -> Synchronizing lors d'un resync
Live/Synchronizing -> Closing lors d'une expiration terminale
~~~

Le producteur maintient des files, fenêtres fiables, quotas de réassemblage et baselines séparés par client. Il ne lit l'état moteur que sur le thread autorisé par l'architecture ; les threads réseau consomment des snapshots immuables.

### 4.3 Hello

Le client :

1. choisit un `client_nonce` aléatoire non nul ;
2. capture `client_send_t0_us` sur son horloge monotone ;
3. annonce exactement l'intervalle de versions qu'il sait émettre et lire ;
4. demande un `VisibilityMode` ;
5. annonce ses capabilities et paramètres spécialisés ;
6. envoie `Hello` avec `session_id = 0`.

Tant qu'aucun `Welcome` correspondant n'est reçu, il retransmet le même `Hello` selon le backoff de la section 8, avec le même nonce et un nouveau `packet_sequence`. Le `message_id` logique reste identique.

### 4.4 Choix de version et visibilité

v1.0 accepte uniquement l'intersection `major=1, minor=0`. FSTL 1.1 ajoute l'intersection `major=1, minor=1` sans changer aucun layout. Le producteur choisit la plus haute mineure commune qu'il sait réellement servir. Sans intersection, il répond `Welcome UnsupportedVersion` avec `session_id = 0`.

Le producteur minimal de Phase 1 annonce et accepte exclusivement `minor=1`. Il rejette un client limité à 1.0 au lieu de rétrograder, car le contrat 1.0 exige `CORE_SHIP`. Une implémentation qui annonce `0..1` DOIT posséder deux chemins conformes : couverture 1.0 complète après sélection de 0, ou profil 1.1 après sélection de 1. Le choix de la mineure ne dépend jamais d'une capability visuelle.

Les modes de visibilité sont :

| Valeur | Nom | Sémantique |
|---:|---|---|
| 0 | `COCKPIT` | connaissance locale/autorisée du joueur |
| 1 | `TRUSTED_FULL_STATE` | état exhaustif réservé à un endpoint de confiance |

Le producteur est autoritaire :

- `TRUSTED_FULL_STATE` exige une option explicite et un endpoint allowlisté ;
- un producteur en `MULTIPLAYER_CLIENT` ne peut sélectionner que `COCKPIT` ;
- `TRUSTED_FULL_STATE` est possible en `SOLO` ou `MULTIPLAYER_MASTER` si la configuration l'autorise ;
- un master headless peut fournir l'état exhaustif sans annoncer de capability visuelle ;
- un refus de la demande peut être un downgrade vers `COCKPIT` ou `Welcome Unauthorized` selon la politique configurée.

`AuthorityMode` (`SOLO`, `MULTIPLAYER_CLIENT`, `MULTIPLAYER_MASTER`) n'est jamais demandé par le client. Il est observé et publié dans `SessionState`.

### 4.5 Welcome

À la réception d'un `Hello` syntaxiquement valide et autorisé, le producteur capture `t1` avant travail coûteux, choisit les paramètres, crée la session, puis capture `t2` immédiatement avant envoi.

Un `Welcome Accepted` :

- recopie le nonce et `t0` ;
- porte le `session_id` non nul dans l'en-tête ;
- sélectionne la plus haute mineure commune, v1.0 ou v1.1, et le mode de visibilité ;
- annonce `producer_id`, capabilities producteur et capabilities actives ;
- fixe l'intervalle de heartbeat et le timeout de réassemblage fiable.

Le client capture localement `t3` dès réception du datagramme valide, avant décodage d'extensions, allocation ou publication. Après acceptation, il envoie `Ack APPLIED` pour `Welcome`.

Un `Welcome` rejeté n'est pas acquitté et ne crée aucun état de session durable.

### 4.6 Synchronisation initiale

Après l'ACK de `Welcome`, le producteur envoie dans cet ordre logique :

1. `SessionBegin` ;
2. la transaction `Manifest` référencée par `required_manifest_id`, si non nulle ;
3. la transaction `FullSnapshot` référencée par `initial_snapshot_id` ;
4. les événements fiables intervenus après l'échantillon du snapshot, le cas échéant.

Le producteur **DEVRAIT** attendre le commit du manifeste avant d'envoyer un snapshot qui le référence. Il **PEUT** les pipeliner si les quotas candidats le permettent, mais le client ne committe jamais le snapshot avant le manifeste.

Un changement de manifeste pendant la session exige :

1. commit d'une nouvelle transaction `Manifest` ;
2. émission d'une nouvelle keyframe `FullSnapshot` avec ce `required_manifest_id`.

Un delta ne change jamais implicitement de manifeste.

Chaque transaction `Manifest` v1.0 est exhaustive et autonome. Il n'existe ni manifeste incrémental, ni `base_manifest_id` implicite.

Dans le profil minimal FSTL 1.1 de Phase 1, `SessionBegin.required_manifest_id` vaut zéro, aucune transaction `Manifest` n'est envoyée et le snapshot initial annonce exclusivement `PLAYER_KINEMATICS`. Il contient toujours `SESSION_STATE` et `MISSION_STATE`, puis, si un joueur observé existe, exactement son `ENTITY_LIFECYCLE` et son `FLIGHT_STATE`. Le client ne passe `Live` qu'après validation de cette complétude relative. Un changement de `mission_generation` ou le remplacement de l'identité du joueur observé impose une nouvelle keyframe ; il ne peut pas être publié par un delta seul.

### 4.7 Fin et remplacement de session

`SessionEnd` est fiable et idempotent. À sa réception validée, le client :

1. publie l'état terminal ;
2. envoie `Ack APPLIED` ;
3. arrête les sous-flux spécialisés ;
4. libère transactions, réassemblages et fenêtres lourdes ;
5. passe `Disconnected`.

Le client conserve toutefois un tombstone minimal `(session_id, endpoint, message_id, message_crc32, résultat APPLIED)` pendant 7 secondes. Si l'ACK est perdu, un doublon du même `SessionEnd` reçoit de nouveau `Ack APPLIED` sans republier l'état terminal. Un timeout long produit le même nettoyage local, avec raison locale `Timeout`.

## 5. Négociation et mise à jour des capabilities

### 5.1 Activation initiale

Les paires v1.0 sont :

| Fonction | Bit consommateur | Bit producteur |
|---|---|---|
| communication locale | `COMM_VIEW_LOCAL_ASSETS` | `COMM_VIEW_AUTHORITATIVE_SOURCE` |
| vidéo cible | `TARGET_VIDEO_H264` | `TARGET_VIDEO_REMOTE_RENDER` |

Une fonction est active seulement si les deux bits complémentaires sont annoncés et si ses paramètres spécialisés sont compatibles. `active_capabilities` contient les deux bits de chaque paire active.

`CAPABILITY_UPDATE` est actif seulement si les deux pairs annoncent ce bit. Les bits inconnus ne deviennent jamais actifs.

### 5.2 CapabilityUpdate

`CapabilityUpdate` est bidirectionnel, fiable et ordonné par `capability_generation` propre à son émetteur.

`CapabilityUpdateReason` est fermé :

| Valeur | Nom |
|---:|---|
| 0 | `Invalid`, jamais émis |
| 1 | `RuntimeAvailability` |
| 2 | `PeerRequest` |
| 3 | `ConfigurationChange` |
| 4 | `ErrorRecovery` |

Une perte locale de capability prend effet immédiatement : l'endpoint arrête d'utiliser le composant défaillant avant d'attendre le réseau. Il envoie ensuite sa nouvelle offre avec une génération supérieure.

v1.0 autorise uniquement le retrait :

- `advertised_capabilities` et `active_capabilities` sont des sous-ensembles de leurs valeurs précédemment acceptées ;
- l'émetteur ne retire de `advertised_capabilities` que les bits dont il est propriétaire ;
- lorsqu'un rôle disparaît, l'émetteur retire les deux bits de la paire dans `active_capabilities` ;
- ajouter un bit, réactiver une paire ou changer un paramètre/bundle exige une nouvelle session ;
- retirer `CAPABILITY_UPDATE` constitue la dernière mise à jour dynamique valide de cet émetteur.

Le récepteur vérifie ces sous-ensembles, arrête immédiatement toute fonction retirée, puis répond `Ack APPLIED`. Les deux pairs convergent par intersection monotone ; aucune mise à jour ne peut réactiver une fonction.

Une génération inférieure est obsolète : après validation syntaxique et contrôle de session/endpoint, elle n'est pas réappliquée et reçoit `Ack VALIDATED | APPLIED`, car l'état courant est déjà plus récent. Une génération égale et un payload octet pour octet identique est un doublon : le dernier ACK requis est renvoyé. Une génération égale avec un payload différent est une erreur sémantique et n'est pas acquittée, même en cas de collision de CRC. Une génération supérieure valide est appliquée normalement. Cette règle arrête les retransmissions d'un update retardé sans jamais faire régresser les capabilities.

La perte d'une capability visuelle ne termine pas la session de télémétrie. Le document 05 définit les `TargetVideoStop` et états de communication associés.

Si `CAPABILITY_UPDATE` n'a pas été négocié, une modification de l'offre nécessite une nouvelle session. Une défaillance locale peut toujours arrêter immédiatement un sous-flux ; elle ne peut pas activer une nouvelle capability sans handshake.

## 6. Horloges

### 6.1 Domaines

Chaque processus possède une horloge monotone locale :

- elle ne représente pas une heure civile ;
- elle continue pendant la pause de mission ;
- elle ne recule pas dans la durée du processus ;
- son origine n'est pas comparable directement à celle du pair.

`mission_time_us` est une donnée de simulation. Elle peut rester constante en pause, progresser selon la compression temporelle et être réinitialisée lors d'un changement de mission. Elle ne sert ni à ordonner les datagrammes, ni aux timeouts, ni à la synchronisation.

Les taux de lecture spécialisés sont signés. `playback_rate == 0` signifie pause ; une valeur négative signifie lecture inverse. La compression temporelle est déjà incluse dans ce taux.

### 6.2 Échange à quatre instants

Pour une requête client vers producteur :

~~~text
t0 : émission client
t1 : réception producteur
t2 : émission producteur
t3 : réception client

offset = ((t1 - t0) + (t2 - t3)) / 2
rtt    = (t3 - t0) - (t2 - t1)
producer_time_estimate = client_monotonic_time + offset
~~~

`offset` signifie « horloge producteur moins horloge client ».

Le calcul utilise un entier signé d'au moins 128 bits ou des opérations contrôlées garantissant le même résultat. Toute soustraction hors plage, `t2 < t1`, `t3 < t0` ou `rtt < 0` invalide l'échantillon. La division signée par deux tronque vers zéro.

### 6.3 Heartbeat

Après le handshake, chaque pair peut initier un `Heartbeat Request`. Le répondant :

1. capture `receive_t1_us` au plus tôt ;
2. recopie `probe_id` et `origin_t0_us` ;
3. capture `transmit_t2_us` immédiatement avant envoi ;
4. envoie `Heartbeat Response` sans ACK.

L'initiateur capture `t3` avant traitement. Une réponse dont le `probe_id` ou `origin_t0_us` ne correspond pas à une sonde en vol est ignorée. Au plus huit sondes peuvent être en vol par initiateur.

L'intervalle négocié est borné à 200–5000 ms :

- défaut hors mission : 1000 ms ;
- cible en mission : 200–500 ms si la charge le permet ;
- un répondant envoie sa réponse dès que possible, indépendamment de son propre rythme de sondes.

### 6.4 Filtre d'offset

Chaque pair conserve les huit derniers échantillons valides de la session. À chaque ajout :

1. sélectionner l'échantillon de RTT minimal dans la fenêtre ;
2. utiliser son offset comme `candidate_offset` ;
3. initialiser l'offset lissé avec le premier candidat ;
4. ensuite appliquer :

~~~text
smoothed_offset += trunc_toward_zero((candidate_offset - smoothed_offset) / 8)
~~~

Le RTT minimal de la fenêtre sert aussi au calcul du timeout de retransmission. Le filtre est invalidé :

- immédiatement au changement de session ;
- après trois intervalles de heartbeat sans réponse valide ;
- après une erreur d'overflow ou une discontinuité monotone locale.

Une deadline exprimée en temps producteur n'est utilisée que si le filtre est valide. Sinon le client met la deadline à zéro et le producteur applique la fenêtre de rétention locale de la classe.

### 6.5 Stale et timeout long

Soit `H` l'intervalle de heartbeat négocié :

| Événement | Délai |
|---|---:|
| horloge invalide / session `Stale` | `max(3 × H, 3000 ms)` sans réponse valide |
| session déconnectée | `max(10 × H, 10 000 ms)` sans datagramme valide |

Tout datagramme de session valide prouve l'activité réseau, mais seule une réponse heartbeat valide rafraîchit l'estimation d'horloge.

## 7. Classes de livraison et ACK requis

### 7.1 Matrice

| Message | Livraison | ACK requis |
|---|---|---|
| `Discovery` | périodique | aucun |
| `Hello` | répété jusqu'à `Welcome` | `Welcome` fait réponse |
| `Welcome Accepted` | fiable | `APPLIED` |
| `SessionBegin` | fiable | `APPLIED` |
| chaque part `Manifest` | fiable transactionnel | `VALIDATED` puis `APPLIED` |
| chaque part `FullSnapshot` | fiable transactionnel | `VALIDATED` puis `APPLIED` |
| `Delta` | remplaçable | aucun |
| `EventBatch Replaceable` | remplaçable | aucun |
| `EventBatch Reliable` | fiable | `APPLIED` |
| `Heartbeat` | périodique | aucun |
| `Ack` / `Nack` | immédiat | jamais |
| `ResyncRequest` | fiable | `VALIDATED`, puis réponse de synchronisation |
| `SessionEnd` | fiable | `APPLIED` |
| `TargetVideoSubscribe` | fiable | `APPLIED` |
| `TargetVideoConfig` | fiable | `APPLIED` |
| interframe `TargetVideoFrame` | non fiable | aucun |
| IDR `TargetVideoFrame` | fenêtre sélective bornée | NACK seulement |
| `TargetVideoKeyframeRequest` | fiable | `VALIDATED` |
| `TargetVideoStop` | fiable et idempotent | `APPLIED` |
| `TargetVideoStats` | périodique | aucun |
| `CapabilityUpdate` | fiable | `APPLIED` |

Tout message exigeant un ACK porte `ACK_REQUIRED`. Aucun autre message ne le porte. Un `Ack` ou `Nack` n'est jamais lui-même acquitté.

### 7.2 VALIDATED et APPLIED

`Ack VALIDATED` signifie :

- réassemblage complet ;
- `message_crc32` correct ;
- parsing et validation structurelle réussis ;
- message acceptable dans la session.

Il ne signifie pas que l'état est visible, ne fait pas avancer une baseline et ne permet pas de libérer un message qui exige `APPLIED`.

`Ack APPLIED` porte les deux bits `VALIDATED | APPLIED`. Il signifie que la mutation est publiée atomiquement et durable dans l'état courant de la session.

Lorsqu'un message peut être validé et appliqué immédiatement, le récepteur peut envoyer directement `VALIDATED | APPLIED` sans ACK intermédiaire.

### 7.3 Correspondance d'un ACK/NACK

Avant de modifier une fenêtre d'envoi, l'émetteur vérifie :

- même `session_id` et endpoint ;
- `target_message_id` encore retenu ;
- même `target_message_type` ;
- même `target_fragment_count` ;
- même `target_message_crc32` ;
- flags ACK valides ou raison/bitmap NACK valides ;
- deadline NACK acceptable pour la classe.

Un ACK/NACK inconnu, tardif ou incohérent est ignoré et comptabilisé. Il ne prolonge aucune deadline.

### 7.4 Déduplication

Le récepteur conserve, pendant la fenêtre fiable plus 2 secondes, une table bornée :

~~~text
(session_id, endpoint, message_type, message_id, message_crc32)
    -> dernier résultat VALIDATED/APPLIED ou erreur
~~~

Un doublon identique :

- n'est ni reparsé ni republié si son résultat est déjà connu ;
- reçoit de nouveau le dernier ACK applicable ;
- ne fait jamais régresser l'état.

Le cache utilise au plus 4096 entrées par client. À saturation, il supprime seulement une entrée expirée ; sinon il rejette la nouvelle réservation avec `ResourceLimit`.

### 7.5 Transitions d'état fiables

Un `Delta` reste remplaçable même s'il contient `CREATE` ou `DELETE`. Il ne peut donc jamais être l'unique livraison d'une transition que le modèle de données déclare fiable.

Le producteur envoie au minimum par une voie fiable :

- les créations et suppressions autorisées de catalogues dans une transaction `Manifest` ;
- les créations et suppressions d'entités ou d'atomes dynamiques dans un `EventBatch Reliable` ;
- une révélation de cargo, une configuration immuable nouvellement connue et tout événement non reconstructible dans un `EventBatch Reliable` ;
- les débuts, remplacements et fins de communication selon le document 05.

La même mutation d'état est aussi reflétée dans les deltas cumulatifs tant qu'elle diffère de la baseline, puis dans la keyframe suivante. La répétition entre voie fiable et état cumulatif est idempotente grâce aux clés de records et `event_id`.

## 8. Fenêtres fiables, backoff et expiration

### 8.1 Constantes

| Constante | Valeur v1.0 |
|---|---:|
| RTO par défaut sans RTT | 250 ms |
| RTO minimal | 100 ms |
| RTO maximal | 1000 ms |
| facteur de backoff | 2 |
| jitter | ±10 % |
| fenêtre d'envoi fiable ordinaire | 5000 ms |
| timeout de réassemblage fiable | 2000 ms |
| fenêtre d'une transaction paginée | 10 000 ms |
| réassemblage d'un delta remplaçable | 500 ms |
| réassemblage d'une interframe vidéo | 200 ms |
| rétention IDR vidéo | au plus 500 ms |

Si une fenêtre RTT valide existe :

~~~text
base_rto = clamp(2 * min_rtt_in_window, 100 ms, 1000 ms)
~~~

Sinon `base_rto = 250 ms`.

Pour la retransmission numéro `n`, à partir de zéro :

~~~text
delay_n = min(base_rto * 2^n, 1000 ms) * jitter(message_id, n)
~~~

`jitter` est une valeur pseudo-aléatoire uniforme dans `[0.9, 1.1]`, dérivée de l'état de session et non contrôlée par le pair. Les tests peuvent fixer la graine.

### 8.2 Règles de retransmission

Une retransmission :

- conserve le payload logique, `message_id`, `message_crc32`, `frame_id` et temps d'échantillon ;
- génère un nouveau `packet_sequence` et un nouveau `sent_time_us` ;
- recalcule `crc32` ;
- positionne `RETRANSMISSION` ;
- ne repousse jamais la deadline absolue de la fenêtre.

Sans NACK, l'émetteur peut retransmettre tous les fragments. Un `Nack MissingFragments` valide autorise la retransmission des seuls fragments marqués.

Un NACK peut déclencher une retransmission immédiate, sous réserve des priorités et rate limits, mais ne réinitialise ni le backoff ni la fenêtre.

### 8.3 Expiration

À expiration :

- buffers et cache d'envoi sont libérés ;
- aucune rétention ne devient illimitée ;
- un snapshot/manifeste candidat est abandonné puis remplacé par une transaction plus récente ou par un resync ;
- un `SessionBegin` ou `CapabilityUpdate` indispensable non acquitté rend la session `Stale` puis la ferme ;
- un événement fiable non reconstructible non acquitté déclenche une resynchronisation ou une fermeture explicite ;
- une interframe ou IDR expirée est abandonnée entièrement ;
- une demande d'IDR expirée peut être renouvelée selon le document 05.

Une nouvelle transaction ne doit pas évincer une transaction fiable plus ancienne encore dans sa fenêtre. Après expiration seulement, une candidate plus récente peut la remplacer.

## 9. NACK sélectif

### 9.1 Détection d'un trou

Pour un message fiable, la réception d'un fragment d'index supérieur révélant un trou arme un délai :

~~~text
nack_delay = min(25 ms, base_rto / 4)
~~~

À son expiration, si le trou existe encore, le récepteur envoie un bitmap `MissingFragments`. Il envoie au plus un NACK par message toutes les 50 ms. Le bitmap décrit l'état courant complet, pas seulement les nouveaux trous.

Un message remplaçable, notamment `Delta`, ne reçoit pas de NACK : un message plus récent ou un resync le corrige.

### 9.2 CRC et erreurs

- `BadMessageCrc` est envoyé après réassemblage complet si le CRC logique échoue.
- `BadFragmentLayout` invalide immédiatement tout le réassemblage.
- `ResourceLimit` indique qu'aucune réservation n'a été effectuée.
- `StaleBaseline` peut répondre à un delta ou événement fiable lié à une baseline inutilisable.
- `SemanticValidationFailed` indique un payload bien formé mais non acceptable.

Un NACK d'erreur sans bitmap ne demande pas implicitement tous les fragments. L'émetteur décide entre retransmission complète, nouvelle keyframe ou fermeture selon la classe.

Un NACK n'est envoyé qu'à un endpoint de session validé. Pour un type inconnu avec `ACK_REQUIRED`, au plus un `UnsupportedMessage` est envoyé pour le tuple logique, dans le budget commun. Sans `ACK_REQUIRED`, le message inconnu est abandonné en silence.

### 9.3 IDR vidéo

Seuls les fragments d'une IDR peuvent être retransmis dans le flux vidéo :

- copie producteur conservée au plus 500 ms après le premier envoi ;
- `needed_before_producer_time_us` non dépassé si non nul ;
- NACK sélectif uniquement ;
- priorité inférieure à toute télémétrie d'état ;
- abandon complet à deadline ;
- aucune retransmission d'interframe.

Cette exception ne transforme pas la vidéo en canal fiable.

## 10. Transactions Manifest et FullSnapshot

### 10.1 Identité et quotas

Une candidate est indexée par :

~~~text
(session_id, message_type, transaction_id, transaction_sha256)
~~~

`transaction_id` est `manifest_id` ou `snapshot_id`. Un client conserve au plus :

- une candidate `Manifest`, 16 Mio maximum ;
- une candidate `FullSnapshot`, 16 Mio maximum ;
- 32 Mio de régions de records candidates au total ;
- 64 parts par transaction ;
- une durée de 10 secondes depuis la première part valide.

Ces quotas sont distincts des 4 Mio de réassemblage de messages fiables. Une part doit d'abord être réassemblée sous le quota ordinaire, puis sa région de records validée est transférée dans le quota candidat.

### 10.2 Cohérence des parts

Toutes les parts d'une transaction ont les mêmes :

- ID, `part_count`, `transaction_size` et `transaction_sha256` ;
- `producer_sample_time_us` ;
- `manifest_kind` ou `snapshot_flags` ;
- `required_manifest_id` pour un snapshot.
- `frame_id` et `mission_time_us` dans l'en-tête.

`part_index` est unique. Un même index et des octets identiques est un doublon. Le même index avec un autre contenu, ou le même ID avec un autre hash pendant la fenêtre, invalide la candidate et produit `SemanticValidationFailed`.

Les parts peuvent arriver dans n'importe quel ordre. Aucun record ne traverse une part.

### 10.3 Validation par part

Après réassemblage, CRC et parsing d'une part :

1. vérifier son préfixe et ses bornes ;
2. vérifier exactement `record_count` records ;
3. vérifier chaque record structurellement sans le publier ;
4. réserver/copier la région de records dans la candidate ;
5. envoyer `Ack VALIDATED` pour le `message_id` de cette part.

L'émetteur conserve chaque part après `VALIDATED`, car la transaction n'est pas encore appliquée.

### 10.4 Validation et commit transactionnels

Quand toutes les parts sont présentes, le client :

1. concatène logiquement les régions de records par `part_index` ;
2. vérifie que leur somme est exactement `transaction_size` ;
3. calcule le SHA-256 et le compare à `transaction_sha256` ;
4. valide les contraintes inter-records, unicités et références ;
5. pour un snapshot, vérifie que `required_manifest_id` est déjà installé ;
6. construit un état candidat hors de l'état publié ;
7. committe l'ensemble atomiquement ;
8. envoie `Ack VALIDATED | APPLIED` pour **chaque part**.

Si une étape échoue, aucune part n'est publiée. Le client envoie un NACK adapté pour les parts encore connues puis libère la candidate.

Le producteur considère le manifeste installé ou le snapshot devenu baseline commune uniquement après réception de `APPLIED` pour tous les `message_id` de la transaction.

### 10.5 ACK perdu et idempotence

Le client conserve le résultat du commit pendant au moins la fenêtre fiable plus 2 secondes. Si une part d'une transaction déjà committée est retransmise :

- il vérifie ID, index, hash et CRC ;
- il renvoie `Ack VALIDATED | APPLIED` pour cette part ;
- il ne réapplique rien ;
- il ne fait jamais régresser le manifeste ni la baseline.

Un snapshot plus ancien que la baseline publiée est ignoré mais, s'il correspond exactement à une transaction précédemment appliquée encore dans le cache, ses parts sont réacquittées `APPLIED`.

## 11. Baselines et deltas cumulatifs

### 11.1 Baseline active et candidate

Le producteur maintient par client :

- exactement une baseline active acquittée ;
- au plus une candidate `FullSnapshot` ;
- un dirty-set relatif à chaque baseline suivie.

À la capture d'une candidate, il continue d'émettre les deltas de la baseline active. Il commence simultanément à suivre les différences entre l'état courant et la candidate.

La candidate ne devient active que lorsque toutes ses parts ont reçu `APPLIED`. Le producteur calcule alors le dirty-set initial de la nouvelle baseline par comparaison avec l'état courant ou avec un journal complet depuis la capture.

Il **NE DOIT PAS** remettre ce dirty-set aveuglément à vide. Toute création, modification ou suppression survenue entre capture et ACK doit apparaître dans le premier delta de la nouvelle baseline.

### 11.2 Contenu cumulatif

Pour une baseline donnée, chaque `Delta` contient :

- toutes les valeurs courantes différentes du `FullSnapshot` de baseline, à la granularité de remplacement du record ;
- toutes les suppressions explicites survenues depuis cette baseline ;
- un `delta_sequence` strictement supérieur au précédent ;
- son `producer_sample_time_us`.

Le delta est reconstruit contre la baseline immuable, jamais contre le delta précédent. Un delta plus récent de la même baseline remplace intégralement tout delta antérieur. La perte d'un nombre quelconque de deltas intermédiaires ne rend donc pas le suivant incomplet.

Si le cumul ne tient plus dans 1 Mio, le producteur crée une nouvelle keyframe au lieu de fractionner un delta en plusieurs messages.

### 11.3 Application client

Le client :

1. recherche la baseline immuable `baseline_snapshot_id` ;
2. ignore un `delta_sequence` inférieur ou égal au dernier appliqué ;
3. clone la baseline, puis superpose tous les records et suppressions du delta ;
4. considère tout champ absent comme égal à la baseline, pas au delta précédent ;
5. valide l'état candidat complet ;
6. publie atomiquement ;
7. mémorise le `delta_sequence` appliqué.

Aucune continuité de séquence n'est exigée.

Après commit d'une nouvelle baseline, tout delta de l'ancienne baseline est ignoré. Le producteur peut libérer les données fiables de l'ancienne baseline seulement après tous les ACK `APPLIED` de la nouvelle transaction.

### 11.4 Delta reçu avant sa baseline

Le client conserve au plus un delta en attente pour une baseline candidate connue :

- au plus 1 Mio ;
- seulement le `delta_sequence` le plus élevé ;
- pendant au plus 2 secondes et jamais au-delà de l'expiration de la transaction snapshot.

Il ne l'applique jamais sur l'ancienne baseline. Après commit du snapshot, il peut l'appliquer s'il reste valide et plus récent.

Un delta dont la baseline est entièrement inconnue est abandonné et déclenche un `ResyncRequest UnknownBaseline` rate-limité. Il n'est pas mis en attente.

### 11.5 Manifeste requis

Un `FullSnapshot` ne peut être committé que si son `required_manifest_id` est zéro ou correspond au manifeste installé. Un manifeste plus ancien ou différent ne suffit pas, même si ses records paraissent décodables.

Après installation d'un nouveau manifeste, le producteur émet une keyframe qui le référence. Aucun delta ne peut changer `required_manifest_id`.

## 12. Événements

`event_id:u64` est strictement croissant dans la session et ne wrappe pas. `first_event_id` désigne l'ID du premier événement du batch ; les records `Events` et spécialisés portent les IDs exacts définis par les documents 04 et 05. Les événements d'un batch sont ordonnés par ID strictement croissant.

Un événement reconstructible peut être `Replaceable` si un snapshot ou état périodique corrige sa perte. Un événement non reconstructible est `Reliable`.

Le récepteur déduplique par `event_id` dans une fenêtre bornée d'au moins 4096 IDs. Un ID déjà vu est ignoré ; un ID plus faible mais jamais vu n'est pas silencieusement considéré comme un doublon. Les événements reçus en désordre sont ordonnés avant publication lorsque leur schéma exige un ordre causal. Si un événement fiable arrive trop tard pour être appliqué sans violer cet ordre, le client demande un resync plutôt que de l'appliquer dans un ordre faux. Un batch fiable est appliqué atomiquement et acquitté `APPLIED`.

Une perte d'événement de communication `START` ou `STOP` est corrigée par l'état `CommViewState` d'une keyframe. Un `STOP` retardé ne s'applique que si son `playback_id` correspond encore à la lecture visible.

## 13. Resynchronisation

### 13.1 Déclencheurs

Le client demande une resynchronisation lorsqu'au moins une condition vaut :

- baseline inconnue ;
- transaction snapshot/manifeste expirée ;
- échec sémantique empêchant de reconstruire un état complet ;
- dirty-set/delta impossible à valider ;
- retour depuis `Stale` ;
- demande manuelle.

La perte d'un seul delta, sans autre erreur, n'est pas un déclencheur puisque les deltas sont cumulatifs.

### 13.2 Protocole

Le client :

1. incrémente `request_id` ;
2. indique sa dernière baseline et séquence appliquées ;
3. demande un manifeste si nécessaire et toujours un snapshot complet ;
4. envoie `ResyncRequest` fiable ;
5. répète avec backoff jusqu'à `Ack VALIDATED` ou expiration.

Le producteur déduplique `request_id`, répond `VALIDATED` après acceptation, puis crée une transaction manifeste éventuelle et une keyframe `RESYNC` récente. Il ne retransmet pas un historique non borné.

Tant que le resync n'est pas committé, le client reste `Synchronizing` ou `Stale`. Le commit du nouveau snapshot et ses ACK `APPLIED` rétablissent `Live`.

### 13.3 Requêtes répétées

Une nouvelle requête reçue pendant une transaction de resync valide renvoie le même ACK et ne crée pas une deuxième candidate. Après expiration, le producteur peut créer une candidate plus récente.

## 14. Priorités et backpressure

Les files d'envoi utilisent cet ordre strict :

| Priorité | Classe |
|---:|---|
| 0, maximale | session, horloge, `Ack`/`Nack`, `ResyncRequest`, `CapabilityUpdate`, `TargetVideoSubscribe`, `TargetVideoConfig`, `TargetVideoStop`, `TargetVideoKeyframeRequest` |
| 1 | manifestes, snapshots, événements fiables |
| 2 | deltas et états périodiques numériques |
| 3 | retransmissions de fragments de la dernière IDR |
| 4 | nouvelle IDR |
| 5 | interframes vidéo |
| 6, minimale | `TargetVideoStats` |

Une file pleine abandonne d'abord `TargetVideoStats`, puis les interframes, puis les fragments non encore envoyés d'une nouvelle IDR. Elle peut ensuite remplacer un état numérique remplaçable par une version plus récente de la même clé. Elle n'abandonne jamais un message fiable plus ancien pour accepter une vidéo.

Les files sont bornées par configuration et observées. Aucune attente réseau, aucun encodage synchrone et aucune attente GPU ne se déroule dans la frame du jeu.

## 15. Rate limits

Les limites suivantes sont des maxima v1.0. `Hello` et la création de session sont comptés par adresse IP source avant session ; les autres entrées sont comptées par session et endpoint, et les ACK/NACK sont en plus bornés par message cible. Une configuration peut les abaisser :

| Action reçue | Débit | Burst |
|---|---:|---:|
| `Hello` | 4/s | 8 |
| `Heartbeat Request` | 10/s | 20 |
| `ResyncRequest` | 1/s | 2 |
| `CapabilityUpdate` | 2/s | 2 |
| ACK/NACK cumulés | 100/s | 200 |
| NACK `UnsupportedMessage` | 10/s | 10 |
| création d'une session | 1/s | 4 |

Un token bucket à recharge continue est utilisé ; chaque message coûte un token de sa classe. Un dépassement entraîne abandon et compteur, sans allocation proportionnelle à l'entrée.

Les limites des abonnements, changements de configuration et demandes d'IDR sont définies dans le document 05. Les réponses heartbeat légitimes ne consomment pas le bucket de requêtes mais restent soumises au budget réseau global.

## 16. Libération et nettoyage

Un réassemblage est libéré immédiatement s'il :

- expire ;
- appartient à une session remplacée ;
- contredit un fragment déjà reçu ;
- dépasse un quota ;
- concerne une ancienne `config_generation` ou un flux vidéo arrêté ;
- appartient à une transaction abandonnée.

À la fermeture d'une session, le producteur et le client libèrent :

- fragments et transactions ;
- fenêtres d'envoi et cache de déduplication ;
- baselines, dirty-sets et delta en attente ;
- sondes heartbeat et filtre d'horloge ;
- états de communication et vidéo.

Aucun objet filaire d'une ancienne session ne peut être réutilisé dans la nouvelle.

## 17. Sécurité v1.0

La cible v1.0 est un LAN de confiance avec endpoints configurés. UDP ne chiffre et n'authentifie pas.

Le producteur **DOIT** néanmoins :

- utiliser un `session_id` imprévisible ;
- vérifier endpoint et session avant allocation importante ou NACK ;
- appliquer allowlist, quotas et rate limits ;
- ne pas activer le broadcast par défaut ;
- ne jamais accepter de commande de simulation ;
- journaliser les connexions acceptées/refusées et erreurs agrégées ;
- ne pas imprimer les payloads complets à chaque frame.

Avant preuve de retour par `Ack APPLIED` de `Welcome`, le producteur n'envoie ni manifeste, ni snapshot, ni flux vidéo, ne réserve aucune ressource lourde associée et limite la taille cumulée de ses réponses à trois fois la taille cumulée reçue de cet endpoint.

Une exposition Internet exige une couche d'authentification, d'intégrité cryptographique et idéalement de confidentialité qui est hors du périmètre v1.0.

## 18. Critères d'acceptation du contrat

Une implémentation de Phase 0 est conforme à ce document si les tests sans moteur démontrent au minimum :

1. handshake accepté, rejeté, dupliqué et expiré ;
2. sélection stricte v1.0, sélection v1.1 avec intervalle `1..1`, rejet `UnsupportedVersion` sans intersection et contrôle du mode de visibilité ;
3. rejet d'un endpoint ou `session_id` incorrect ;
4. calcul d'offset/RTT, overflow, filtrage et invalidation après heartbeats manqués ;
5. wrap sériel de `packet_sequence` et absence de wrap des IDs stricts ;
6. ACK `VALIDATED`, `APPLIED`, doublons et ACK perdus ;
7. NACK sélectif, bitmap exact, NACK tardif/incohérent ignoré ;
8. backoff borné et libération à expiration ;
9. perte, duplication, désordre, jitter et coupure temporaire ;
10. transaction manifeste/snapshot à 1, 2 et 64 parts reçues dans le désordre ;
11. taille/hash transactionnels incorrects et absence de publication partielle ;
12. ACK `VALIDATED` par part puis `APPLIED` par part après un unique commit ;
13. snapshot bloqué tant que `required_manifest_id` n'est pas installé ;
14. ACK `APPLIED` perdu puis réacquittement sans rollback ;
15. modification, création et suppression pendant l'aller-retour de la keyframe, toutes présentes dans le premier delta de la nouvelle baseline ;
16. perte d'un delta intermédiaire puis application du delta cumulatif suivant ;
17. delta ancien, baseline inconnue et delta de nouvelle baseline reçu avant le snapshot ;
18. resynchronisations répétées sans croissance non bornée ;
19. baisse dynamique d'une capability sans perte de la télémétrie numérique ;
20. client lent ou silencieux et respect de toutes les limites mémoire ;
21. priorité de la télémétrie sur les fragments vidéo ;
22. aucune retransmission d'interframe et rétention IDR au plus 500 ms.
23. snapshot Phase 1 accepté avec `PLAYER_KINEMATICS` seul, `required_manifest_id = 0`, capabilities et couverture événementielle nulles ;
24. snapshot Phase 1 rejeté si un record obligatoire manque, si `CORE_SHIP` est annoncé sans sa matrice complète ou si le joueur change sans nouvelle keyframe ;
25. exécution inchangée de tous les scénarios et golden vectors FSTL 1.0.

Les tests de perte doivent utiliser des graines reproductibles. Le `PacketReader` est fuzzé avec buffers tronqués, valeurs inconnues, non-finis, tailles en overflow, incohérences inter-fragments et datagrammes supérieurs à 1200 octets.
