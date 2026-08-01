# 01 — Cadre normatif et périmètre

## 1. Objet

Ce document fixe le cadre de conception et d'acceptation de FSTL 1.0. Il répond aux livrables de la [Phase 0](../../04-implementation-roadmap.md#2-phase-0--contrat-de-protocole) sans dépendre de la boucle, des structures ou de l'ABI de FS2Open.

Le contrat distingue trois niveaux :

1. le **modèle exporté**, qui définit la vérité observable et ses règles d'autorité ;
2. le **protocole logique**, qui définit session, messages, records, fiabilité et évolution ;
3. le **format filaire**, qui définit les octets, limites et validations.

Une implémentation n'est conforme que si elle respecte simultanément les trois niveaux.

## 2. Objectifs normatifs

FSTL 1.0 DOIT permettre à un consommateur autorisé de :

- découvrir optionnellement un producteur sur un LAN ou joindre une adresse configurée ;
- négocier une version, un mode, des capabilities et des limites compatibles ;
- identifier sans ambiguïté le producteur, la session et les entités ;
- estimer l'horloge monotone du producteur sans utiliser une heure civile ;
- installer les catalogues et manifestes requis avant publication de l'état ;
- appliquer un snapshot complet de l'état autorisé ;
- maintenir cet état avec des deltas cumulatifs remplaçables ;
- recevoir les transitions brèves garanties sous forme d'événements ordonnés ;
- détecter une baseline inconnue, un état périmé ou une session remplacée ;
- demander une resynchronisation sans bloquer le producteur ;
- utiliser ou refuser indépendamment les vues de communication et de cible ;
- rejoindre une mission en cours sans historique préalable ;
- rester cohérent sous perte, duplication, désordre, jitter et coupure temporaire ;
- rejeter les données invalides avec mémoire et coût CPU bornés ;
- exposer uniquement une API de lecture de la simulation.

FSTL 1.0 NE DOIT PAS être une seconde simulation déterministe, une API C++ distante, un canal de commande de jeu ou un tunnel des paquets multijoueur existants.

## 3. Acteurs et responsabilités

### 3.1 Producteur

Le **producteur** est le processus qui collecte ou possède une vue autoritaire, sérialise FSTL et répond aux messages de contrôle. Il est responsable de :

- filtrer les données selon le mode avant sérialisation ;
- normaliser unités, repères, IDs, chaînes et valeurs numériques ;
- ne jamais publier de pointeur, handle ou indice local instable ;
- produire des snapshots autonomes par rapport à l'historique dynamique ;
- maintenir une baseline et une fenêtre fiable distinctes par client ;
- appliquer les quotas, priorités et rate limits ;
- ne jamais attendre un client, un socket ou une opération de retransmission ;
- retirer une capability devenue indisponible ;
- journaliser les refus sans exposer de données sensibles.

### 3.2 Client

Le **client** est un consommateur FSTL qui négocie, valide, réassemble et publie une réplique. Il est responsable de :

- capturer les timestamps de réception avant traitement coûteux ;
- valider avant allocation proportionnelle à l'entrée ;
- installer manifestes et snapshot de manière transactionnelle ;
- conserver la baseline appliquée immuable ;
- reconstruire chaque candidat depuis cette baseline ;
- ne jamais appliquer un message logique partiel ;
- ignorer les messages d'une ancienne session, baseline, génération ou lecture ;
- borner ses buffers, files, demandes de resync et demandes d'IDR ;
- maintenir les sous-états visuels indépendamment de l'état de télémétrie ;
- traiter toutes les données reçues comme non fiables au sens sécurité.

### 3.3 Décodeur et outil de conformité

Un **décodeur de conformité** ne dépend ni de FS2Open, ni des DTO C++ de l'implémentation principale. Il DOIT lire les fichiers golden, produire une représentation canonique et expliquer chaque rejet par un code stable défini dans [06 — Validation, sécurité et conformité](06-validation-securite-et-conformite.md).

## 4. Identité, session et portée

### 4.1 Identifiant du producteur

`producer_id` est un `u64` non nul, généré aléatoirement lors de la création du profil de télémétrie et persistant dans ce profil. Il sert à distinguer deux installations ou profils ; il n'est ni un secret, ni une preuve d'identité.

Une copie volontaire d'un profil PEUT copier le même `producer_id`. Une application qui exige une identité forte doit l'obtenir hors FSTL 1.0.

### 4.2 Identifiant de session

`session_id` est un `u64` non nul, imprévisible et nouvellement généré pour chaque session client acceptée pendant une activation du producteur FSTL. Deux clients simultanés ont donc deux `session_id`, deux baselines et deux fenêtres fiables distincts. Une nouvelle session est également obligatoire avant toute réutilisation ambiguë d'un compteur de transport.

Les valeurs suivantes sont toujours réservées :

- `session_id == 0` : échange préalable à la session, notamment `HELLO` ou refus de négociation ;
- `entity_id == 0` : aucune entité ;
- tout autre ID métier égal à `0` : valeur absente ou non attribuée lorsque son schéma l'autorise explicitement.

La clé publique d'une entité est `(producer_id, session_id, entity_id)`. `signature` et `net_signature` ne sont que des corrélations diagnostiques.

### 4.3 Une session par producteur

FSTL 1.0 ne fusionne pas deux producteurs dans une même session. Un serveur/master et un client joueur observé publient donc des sessions distinctes, même s'ils décrivent la même mission.

`SESSION_STATE.observed_player_entity_id` et les métadonnées de mission permettent une corrélation volontaire. Toute fusion de sessions, résolution des conflits d'autorité ou association externe appartient à l'application consommatrice et NE DOIT PAS modifier les baselines FSTL.

Cette règle résout la séparation d'autorité suivante :

- un serveur/master peut fournir `TrustedFullState` sans renderer ;
- un processus joueur peut fournir `Cockpit`, Talking Head et vidéo cible ;
- aucune capability visuelle n'est déduite du mode d'état.

## 5. Modes d'autorité et complétude

### 5.1 `Cockpit`

Le mode `Cockpit` exporte l'état complet autorisé au cockpit observé : vaisseau joueur, systèmes, cible, contacts et décisions réellement accessibles à ses capteurs.

Le producteur DOIT filtrer avant sérialisation :

- objets non détectés ;
- identité, classe, équipe ou cargo non révélés ;
- navpoints non autorisés ;
- vérité physique qu'un contact distordu ne devrait pas révéler.

Un contact radar `DISTORTED` ou une dernière observation furtive représente une **observation capteur**, pas nécessairement l'état réel de l'entité. Le producteur NE DOIT PAS contourner ce filtrage en envoyant simultanément un record d'entité complet non autorisé.

### 5.2 `TrustedFullState`

Le mode `TrustedFullState` exporte toutes les entités et relations connues de la source autoritaire. Il est destiné au diagnostic, au replay ou à un client explicitement de confiance.

Ce mode :

- est désactivé par défaut ;
- nécessite une configuration explicite ;
- n'est annoncé que par une source possédant l'autorité correspondante ;
- ne rend pas disponibles par lui-même Talking Head ou vidéo cible ;
- ne doit jamais être activé automatiquement à la demande d'un client.

### 5.3 Sens de « snapshot complet »

Un `FULL_SNAPSHOT` est complet par rapport :

- au mode négocié ;
- aux capabilities négociées ;
- au périmètre de visibilité du producteur ;
- aux versions de manifestes référencées par son préfixe.

Il est autonome vis-à-vis des anciens snapshots et deltas, mais il peut référencer des catalogues et manifestes statiques. Le client NE DOIT publier le snapshot qu'après avoir installé et acquitté toutes les générations de manifestes requises. Un manifeste manquant rend l'ensemble `Synchronizing`, pas partiellement `Live`.

L'indisponibilité d'une capability visuelle n'empêche pas l'état canonique de devenir `Live`.

## 6. Taxonomie contractuelle des données

Chaque champ du [modèle v1](04-modele-de-donnees-v1.md) porte exactement une nature principale :

| Code | Nature | Source de vérité | Livraison |
|---|---|---|---|
| `A` | état autoritaire | état courant du producteur | snapshot et delta cumulatif |
| `C` | catalogue | définition stable/versionnée | manifeste fiable, puis référence par ID |
| `D` | dérivé | calcul depuis `A` et `C` | absent du canon ; diagnostic explicitement marqué uniquement |
| `E` | événement | transition ordonnée au point autoritaire | événement fiable ou remplaçable selon la table QoS |

Règles :

- une valeur `D` NE DOIT PAS contredire ses sources `A/C` ;
- les ratios, coordonnées écran, animations de jauge et ETA estimées sont `D` sauf décision contraire explicitement nommée ;
- une transition pouvant commencer et finir entre deux captures DOIT être un `E` si la couverture est annoncée comme garantie ;
- un état final reconstructible DOIT aussi être représenté par `A`, même lorsqu'un événement accélère l'interface ;
- les caches `last_*`, handles et timers locaux ne deviennent pas des champs publics ;
- un timestamp moteur de type échéance est converti en durée restante bornée ; il n'est jamais sérialisé brut.

### 6.1 Niveaux de couverture des événements

Pour résoudre l'écart entre événements détectés par diff et hooks ultérieurs, FSTL 1.0 publie deux bitmaps par famille dans `SESSION_STATE` ; ce ne sont pas des bits du registre `Capability` :

- `event_coverage_state_derived` : seuls les événements reconstructibles ou détectés par échantillonnage sont garantis ;
- `event_coverage_exact` : les transitions brèves de la famille sont capturées au point autoritaire.

Le client NE DOIT PAS interpréter l'absence d'un événement comme la preuve qu'il ne s'est pas produit sans le bit de famille correspondant dans `event_coverage_exact`. Les changements de communication disposent d'une couverture exacte lorsque la paire de capabilities Talking Head est active et que la famille `COMMUNICATION` est annoncée dans ce bitmap.

## 7. Invariants du modèle public

### 7.1 Repère et unités

- repère direct : `+X` droite, `+Y` haut, `+Z` avant ;
- positions et vitesses linéaires : repère monde ;
- vitesses angulaires : repère local, `x = pitch`, `y = yaw/heading`, `z = bank/roll` ;
- distances : world-unit FS2Open ;
- durées réseau : microsecondes entières, sauf suffixe d'unité explicite ;
- angles : radians ;
- quaternion : local vers monde, ordre `(w, x, y, z)`, normalisé et signe canonique ;
- ratios normalisés : intervalle `[0, 1]` seulement lorsqu'ils sont explicitement sérialisés comme diagnostic.

Les conversions exactes et types scalaires sont définis dans [02 — Format filaire et registres](02-format-filaire-et-registres.md).

### 7.2 Optionalité

Une valeur ne peut être absente que par l'un des mécanismes suivants :

1. un bit de présence documenté ;
2. une cardinalité nulle documentée ;
3. l'ID nul explicitement autorisé ;
4. un enum `NONE` explicite ;
5. l'absence d'un record facultatif explicitement autorisée par le snapshot.

NaN, infini, chaîne magique, valeur maximale entière ou structure tronquée NE DOIVENT PAS servir de sentinelle implicite.

### 7.3 Granularité des deltas

Chaque record définit une unité atomique de remplacement. Dans un delta :

- un record inclus contient la valeur courante complète de cette unité ;
- un record absent reprend la valeur de la baseline ;
- une suppression est explicite ;
- une liste dynamique est remplacée selon la clé et la granularité indiquées par son schéma ;
- un delta plus récent de même baseline remplace intégralement le delta précédent.

Une implémentation NE DOIT PAS accumuler des patches différentiels successifs sur la réplique publiée.

## 8. Exigences fonctionnelles de Phase 0

| ID | Exigence |
|---|---|
| `P0-F-001` | définir et attribuer toutes les valeurs numériques FSTL 1.0 |
| `P0-F-002` | définir l'ordre, le type, l'unité, la borne, le défaut et l'absence de chaque champ |
| `P0-F-003` | fournir un en-tête de 68 octets et des encodeurs/décodeurs sans padding implicite |
| `P0-F-004` | fournir `PacketWriter` et `PacketReader` bornés, sans `reinterpret_cast` réseau |
| `P0-F-005` | valider CRC, tailles, offsets et quotas avant allocation proportionnelle |
| `P0-F-006` | implémenter `HELLO`, `WELCOME`, `HEARTBEAT`, `ACK`, `NACK` et `RESYNC_REQUEST` indépendamment du moteur |
| `P0-F-007` | formaliser handshake, version/capability negotiation et synchronisation NTP-style |
| `P0-F-008` | formaliser fenêtre fiable, backoff, expiration, déduplication et resync |
| `P0-F-009` | formaliser snapshot candidat, `ACK APPLIED`, baseline immuable et delta cumulatif |
| `P0-F-010` | couvrir les 28 records et toutes les données de l'inventaire, y compris champs conditionnels |
| `P0-F-011` | définir les capabilities et sous-états indépendants des vues de communication et cible |
| `P0-F-012` | fournir golden vectors valides et invalides décodables sans FS2Open |
| `P0-F-013` | fournir une matrice de compatibilité et d'évolution major/minor/record |
| `P0-F-014` | fournir une taxonomie stable de rejets, drops et métriques |
| `P0-F-015` | garantir qu'aucun message client ne peut atteindre une commande de simulation |

## 9. Exigences non fonctionnelles

| ID | Exigence |
|---|---|
| `P0-NF-001` | aucune allocation ou file non bornée |
| `P0-NF-002` | aucun appel réseau bloquant |
| `P0-NF-003` | aucune dépendance à la fragmentation IP |
| `P0-NF-004` | résultat déterministe pour une valeur canonique et une version données |
| `P0-NF-005` | comportement identique sur architectures d'endianness différente |
| `P0-NF-006` | rejet sans crash des troncatures, incohérences, non-finis et valeurs inconnues critiques |
| `P0-NF-007` | mémoire de réassemblage strictement limitée par client et globalement |
| `P0-NF-008` | priorité absolue de l'état sur la vidéo sous congestion |
| `P0-NF-009` | compatibilité mineure additive et rupture majeure explicite |
| `P0-NF-010` | documentation et fixtures utilisables par une implémentation non C++ |
| `P0-NF-011` | configuration fermée par défaut hors loopback |
| `P0-NF-012` | absence d'impact moteur mesurable, la Phase 0 ne touchant pas la boucle FS2Open |

## 10. Modèle de menace de principe

FSTL 1.0 cible un LAN de confiance relative, mais tout datagramme reçu est considéré malformable ou hostile. Les menaces couvertes sont :

- troncature, corruption et incohérence accidentelles ;
- spoofing d'adresse sur un LAN ;
- amplification par `HELLO`, `NACK`, resync ou demande d'IDR ;
- épuisement mémoire par tailles, fragments ou sessions multiples ;
- épuisement CPU/GPU par abonnements et changements vidéo ;
- fuite de données `Cockpit`/`TrustedFullState` ;
- rejeu d'une ancienne session ou génération ;
- chemins d'assets hostiles et contenu local corrompu.

Les moyens obligatoires sont : validation stricte, session imprévisible, allowlist, quotas, rate limits, déduplication, absence de broadcast par défaut, bind loopback par défaut et séparation totale des commandes de simulation.

FSTL 1.0 ne couvre pas la confidentialité, l'authentification cryptographique, l'intégrité contre un attaquant actif ni la traversée sécurisée d'Internet. Pour un réseau non fiable, un tunnel authentifié externe ou une version ultérieure sécurisée est obligatoire.

## 11. Valeurs de configuration : classification

Toute valeur numérique citée dans l'analyse est classée par la spécification comme l'une des catégories suivantes :

- **constante filaire** : ne change pas en FSTL 1.x ;
- **maximum FSTL 1.x** : peut être abaissé, jamais relevé sans évolution compatible explicite ;
- **défaut d'implémentation** : configurable dans les bornes ;
- **valeur négociée** : intersection des offres producteur/client ;
- **valeur d'observation** : utile pour qualifier une implémentation sans changer le wire.

Le port `42042` est un défaut configurable, pas un identifiant de protocole. La découverte est désactivée par défaut. Une écoute non-loopback et `TrustedFullState` requièrent toutes deux une activation explicite et une allowlist non vide.

## 12. Hors périmètre

Sont explicitement hors FSTL 1.0 :

- commande du vaisseau, de la mission, de l'IA ou du renderer par le client ;
- authentification/chiffrement natifs ;
- garantie de livraison temps réel ;
- réplication déterministe de tous les scripts, collisions et états aléatoires ;
- audio de communication ;
- transfert automatique du bundle d'assets pendant la session ;
- vidéo complète du HUD ou du target monitor ;
- modèles glTF et rendu 3D local comme chemin principal ;
- dépendance à Protobuf, FlatBuffers ou à la disposition d'une structure C++ ;
- réutilisation directe des paquets multijoueur FS2Open ;
- fusion automatique de plusieurs producteurs ;
- conservation non bornée d'un historique pour replay.

## 13. Critères produit

- **métier** : tous les domaines `A/C/D/E` sont couverts ou explicitement exclus ;
- **transport** : chaque octet, limite et transition de session est déterministe ;
- **interopérabilité** : les golden vectors sont décodés par deux implémentations indépendantes ;
- **sécurité** : les allocations, débits, sources et capacités coûteuses sont bornés ;
- **compatibilité** : chaque combinaison major/minor/record/capability possède un résultat défini ;
- **architecture** : le contrat public ne dépend d'aucun type moteur ni comportement d'une phase ultérieure.
