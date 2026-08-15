# 06 — Validation, sécurité et conformité

## 1. Objet

Ce document définit les preuves nécessaires pour déclarer FSTL 1.0 implémentable et interopérable. Il spécifie :

- le contrat de `PacketWriter` et `PacketReader` ;
- l'ordre de validation des datagrammes, messages, records et données métier ;
- les budgets et le modèle de sécurité ;
- la taxonomie des rejets et abandons ;
- le format des golden vectors ;
- les tests unitaires, croisés, négatifs, de propriété et de fuzzing ;
- la gate de conformité de la Phase 0.

Il complète le [format filaire](02-format-filaire-et-registres.md), la [fiabilité](03-session-horloges-fiabilite.md), le [modèle de données](04-modele-de-donnees-v1.md) et les [vues spécialisées](05-capabilities-et-vues-specialisees.md).

## 2. Principe de défense en profondeur

Tout octet reçu est non fiable. Une entrée ne devient successivement :

1. un datagramme FSTL candidat ;
2. un fragment FSTL validé ;
3. un message logique complet ;
4. un payload structurellement valide ;
5. une valeur métier acceptable ;
6. un état candidat ;
7. un état publié ;

qu'après avoir franchi la validation du niveau précédent.

Une validation tardive NE DOIT PAS annuler une allocation ou une mutation déjà rendue visible. L'application d'un message logique est transactionnelle : succès complet ou aucun changement observable.

## 3. `PacketWriter` borné

### 3.1 Interface minimale

L'implémentation de référence fournit une abstraction équivalente à :

```cpp
class PacketWriter {
public:
    explicit PacketWriter(std::span<std::uint8_t> output);

    bool write_u8(std::uint8_t value);
    bool write_i8(std::int8_t value);
    bool write_u16(std::uint16_t value);
    bool write_i16(std::int16_t value);
    bool write_u32(std::uint32_t value);
    bool write_i32(std::int32_t value);
    bool write_u64(std::uint64_t value);
    bool write_i64(std::int64_t value);
    bool write_f32(float value);
    bool write_bytes(std::span<const std::uint8_t> value);
    bool write_utf8(std::string_view value, std::size_t field_limit);
    bool write_zeroes(std::size_t count);

    std::size_t size() const;
    std::size_t remaining() const;
    bool ok() const;
};
```

Le nom exact de l'API PEUT varier ; les propriétés suivantes sont obligatoires :

- le buffer de sortie et sa capacité sont fournis par l'appelant ;
- aucune croissance automatique ni allocation proportionnelle à une valeur métier ;
- chaque opération vérifie l'addition `position + taille` sans overflow ;
- un échec rend l'écrivain fautif de manière persistante ;
- aucune opération après échec n'écrit d'octet ;
- les écritures multi-octets produisent explicitement du little-endian ;
- `write_f32` rejette NaN et infinis, canonicalise tout zéro vers `+0.0` et sérialise le motif IEEE-754 binary32 ;
- `write_utf8` valide l'UTF-8, la limite métier et la limite `u16` avant d'écrire la longueur ;
- aucun `reinterpret_cast` d'une structure C++ vers le buffer n'est autorisé ;
- les octets réservés sont écrits explicitement à zéro ;
- la taille finale doit être comparée à la taille calculée par le schéma avant émission.

### 3.2 Écriture transactionnelle

Pour un champ variable ou une liste, l'écrivain DOIT soit :

- pré-calculer et valider sa taille complète ;
- écrire dans un sous-buffer borné puis le committer ;
- ou restaurer sa position en cas d'échec avant que le buffer ne soit exposé.

Un payload partiellement écrit ne doit jamais être fragmenté ou envoyé.

## 4. `PacketReader` borné

### 4.1 Interface minimale

L'implémentation de référence fournit une abstraction équivalente à :

```cpp
class PacketReader {
public:
    explicit PacketReader(std::span<const std::uint8_t> input);

    bool read_u8(std::uint8_t& value);
    bool read_i8(std::int8_t& value);
    bool read_u16(std::uint16_t& value);
    bool read_i16(std::int16_t& value);
    bool read_u32(std::uint32_t& value);
    bool read_i32(std::int32_t& value);
    bool read_u64(std::uint64_t& value);
    bool read_i64(std::int64_t& value);
    bool read_f32(float& value);
    bool read_bytes(std::size_t count, std::span<const std::uint8_t>& value);
    bool read_utf8(std::size_t field_limit, std::string_view& value);
    bool subreader(std::size_t count, PacketReader& value);
    bool skip(std::size_t count);

    std::size_t consumed() const;
    std::size_t remaining() const;
    bool at_end() const;
    bool ok() const;
};
```

Propriétés obligatoires :

- aucune lecture hors du span d'entrée ;
- contrôle d'overflow avant toute addition ou multiplication ;
- un échec ne retourne aucune valeur partiellement initialisée ;
- un échec rend le lecteur fautif de manière persistante ;
- `read_f32` rejette tout motif non fini non autorisé par le champ et canonicalise `-0.0` en `+0.0` ;
- `read_utf8` valide d'abord la longueur, puis l'UTF-8 complet, sans chercher de NUL ;
- `subreader` borne un record ou item à sa longueur déclarée ;
- le décodeur d'une version connue DOIT consommer exactement sa longueur connue ou ignorer uniquement une extension finale autorisée ;
- tout octet trailing non prévu dans un payload fixe est une erreur ;
- ignorer un record inconnu signifie sauter exactement `record_length`, jamais rechercher le record suivant par heuristique.

### 4.2 UTF-8

Le validateur UTF-8 DOIT rejeter :

- séquences tronquées ;
- octets de continuation isolés ;
- encodages surlongs ;
- code points de substitution UTF-16 ;
- code points supérieurs à `U+10FFFF` ;
- NUL lorsque le champ l'interdit ;
- séparateurs ou contrôles interdits par une règle métier de chemin.

FSTL 1.0 n'applique aucune normalisation Unicode implicite. Les comparaisons canoniques mentionnées comme « bytewise » utilisent les octets UTF-8 exacts.

## 5. Ordre de validation d'un datagramme

Le récepteur DOIT suivre cet ordre, sans sauter d'étape :

1. recevoir dans un buffer fixe de 1200 octets au plus et conserver la taille réelle ;
2. vérifier que l'adresse source satisfait la politique de bind/allowlist ;
3. refuser une taille inférieure à 68 ou supérieure à 1200 ;
4. lire uniquement les champs fixes nécessaires sans cast de structure ;
5. vérifier le magic, `version_major == 1`, `version_minor == 0` et `header_size == 68` ;
6. vérifier les bits réservés de `flags` ;
7. vérifier `payload_size <= 1132` ;
8. vérifier `header_size + payload_size == taille_reelle` avec arithmétique sans overflow ;
9. vérifier `crc32` du datagramme ;
10. vérifier la règle de session applicable au type de message ;
11. vérifier direction, endpoint et type de message ;
12. vérifier les maxima de classe logique ;
13. vérifier `fragment_count`, `fragment_index`, offset canonique et tranche ;
14. vérifier la cohérence avec une entrée de réassemblage existante ou réserver dans les quotas ;
15. accepter un doublon seulement si ses métadonnées et octets sont identiques ;
16. stocker le fragment validé ;
17. après complétude, vérifier `message_crc32` sur le payload logique ;
18. parser le payload selon son `message_type` ;
19. valider les records puis la sémantique métier ;
20. construire et publier atomiquement le résultat ;
21. émettre l'ACK approprié uniquement après le niveau d'acceptation requis.

Un fragment contradictoire invalide et libère tout le réassemblage concerné. Un message fiable encore identifiable PEUT recevoir un `NACK BadFragmentLayout`; une entrée pré-session ou non autorisée au sens de l'allowlist est abandonnée silencieusement afin de limiter l'amplification.

## 6. Validation d'un message logique

### 6.1 Payload fixe

Un payload fixe DOIT :

- avoir exactement la taille définie pour sa version ;
- avoir tous ses octets réservés à zéro ;
- satisfaire les enums fermées, plages et relations entre champs ;
- ne contenir aucun octet trailing ;
- référencer une session, capability, génération et ressource encore valides.

### 6.2 Séquence de records

Pour un message à records :

- le préfixe déclare `record_count` ;
- exactement ce nombre de records est lu ;
- chaque longueur est vérifiée avant création du sous-lecteur ;
- un `record_type` inconnu est sauté si sa longueur tient dans le message ; en v1.0, il ne peut être requis par aucune matrice connue ;
- un `record_version` autre que `1` est traité comme un record inconnu, sans notion séparée de majeure/mineure au niveau record ;
- une extension terminale d'un record connu n'est ignorée que si la **version mineure du protocole négociée** l'autorise selon [02](02-format-filaire-et-registres.md) ; une session 1.0 exige le layout 1.0 exact ;
- les couples `MessageType`/`RecordType` et les records requis d'un snapshot sont déterminés par la matrice statique et `SESSION_STATE.state_domain_coverage` de [04](04-modele-de-donnees-v1.md), jamais par un manifeste de schéma implicite ;
- les types singleton ne peuvent apparaître qu'une fois ;
- les clés d'items d'une liste doivent être uniques ;
- l'ordre des records n'a pas de sémantique d'application ;
- les références croisées sont résolues après parsing complet ;
- aucun record n'est appliqué si un record requis du message échoue.

### 6.3 Valeurs métier

Le validateur métier contrôle notamment :

- booléens strictement `0` ou `1` ;
- enums fermées ;
- bits réservés nuls et bitmaps extensibles masqués ;
- quaternion fini, de norme acceptable et signe canonique ;
- plages d'axes, ETS, compteurs et durées ;
- cardinalité dynamique des boucliers, banques et sous-systèmes ;
- unicité des IDs et relations parent/enfant ;
- absence de référence à une entité interdite par le mode ;
- génération exacte des manifestes requis ;
- cohérence entre présence, ID nul et enum `NONE` ;
- cohérence entre durée de communication et manifeste local ;
- cohérence vidéo entre message, flux, génération, cible et taille encodée.

Le producteur DOIT valider ou borner ses propres données avant sérialisation. Le client DOIT néanmoins les revalider.

## 7. Taxonomie locale des validations

Les codes suivants sont stables dans les fixtures et métriques. Ils ne sont pas automatiquement envoyés sur le réseau et ne révèlent pas de contenu de payload.

| Valeur | Code | Niveau |
|---:|---|---|
| 0 | `NONE` | succès |
| 1 | `SOURCE_NOT_ALLOWED` | datagramme |
| 2 | `DATAGRAM_TOO_SHORT` | datagramme |
| 3 | `DATAGRAM_TOO_LARGE` | datagramme |
| 4 | `BAD_MAGIC` | en-tête |
| 5 | `UNSUPPORTED_MAJOR` | en-tête |
| 6 | `UNSUPPORTED_MINOR` | en-tête |
| 7 | `BAD_HEADER_SIZE` | en-tête |
| 8 | `RESERVED_HEADER_FLAG` | en-tête |
| 9 | `BAD_DATAGRAM_LENGTH` | en-tête |
| 10 | `BAD_DATAGRAM_CRC` | datagramme |
| 11 | `UNKNOWN_MESSAGE_TYPE` | en-tête |
| 12 | `WRONG_DIRECTION` | session |
| 13 | `SESSION_MISMATCH` | session |
| 14 | `ENDPOINT_MISMATCH` | session |
| 15 | `MESSAGE_TOO_LARGE` | fragment |
| 16 | `BAD_FRAGMENT_COUNT` | fragment |
| 17 | `BAD_FRAGMENT_INDEX` | fragment |
| 18 | `BAD_FRAGMENT_OFFSET` | fragment |
| 19 | `BAD_FRAGMENT_SLICE` | fragment |
| 20 | `REASSEMBLY_QUOTA` | fragment |
| 21 | `INCONSISTENT_FRAGMENT` | fragment |
| 22 | `REASSEMBLY_TIMEOUT` | fragment |
| 23 | `BAD_MESSAGE_CRC` | message |
| 24 | `TRUNCATED_PAYLOAD` | payload |
| 25 | `TRAILING_BYTES` | payload |
| 26 | `UNKNOWN_REQUIRED_RECORD` | record |
| 27 | `UNSUPPORTED_RECORD_VERSION` | record |
| 28 | `BAD_RECORD_LENGTH` | record |
| 29 | `DUPLICATE_RECORD` | record |
| 30 | `DUPLICATE_ITEM_KEY` | record |
| 31 | `INVALID_UTF8` | champ |
| 32 | `STRING_TOO_LONG` | champ |
| 33 | `NON_FINITE_FLOAT` | champ |
| 34 | `OUT_OF_RANGE` | champ |
| 35 | `UNKNOWN_ENUM` | champ |
| 36 | `RESERVED_FLAG` | champ |
| 37 | `INVALID_ABSENCE` | champ |
| 38 | `UNKNOWN_ENTITY` | métier |
| 39 | `STALE_BASELINE` | métier |
| 40 | `STALE_GENERATION` | métier |
| 41 | `MISSING_MANIFEST` | métier |
| 42 | `CAPABILITY_NOT_NEGOTIATED` | métier |
| 43 | `VISIBILITY_VIOLATION` | métier |
| 44 | `INVALID_STATE_TRANSITION` | métier |
| 45 | `RATE_LIMITED` | sécurité |
| 46 | `RESOURCE_LIMIT` | sécurité |
| 47 | `INTERNAL_SERIALIZATION_ERROR` | producteur |

Les tests comparent un code principal et PEUVENT conserver un contexte local non stable : offset, type, champ ou ID. Les logs de production ne doivent jamais imprimer le payload complet par défaut.

## 8. Types inconnus et extensions

Les comportements sont distincts :

- `message_type` inconnu sous une majeure compatible : abandon du message ; `NACK UnsupportedMessage` uniquement si la session, l'endpoint et `ACK_REQUIRED` sont validés ;
- `record_type` inconnu : saut borné par `record_length` ; en v1.0, les exigences de présence viennent uniquement de la matrice statique de [04](04-modele-de-donnees-v1.md) ;
- `record_version` autre que `1` : même traitement borné qu'un record inconnu ; si le type connu était requis dans un snapshot, l'absence d'une instance v1 valide fait échouer ce snapshot ;
- enum fermée inconnue : rejet du message logique ;
- bitmap explicitement extensible : bits inconnus ignorés à la lecture et jamais réémis par un relais v1 ;
- flag réservé : rejet du niveau qui le porte ;
- capability inconnue : bit ignoré et non négocié.

Aucun de ces cas ne doit provoquer un crash, une fermeture de processus ou une allocation hors budget.

## 9. Modèle de sécurité opérationnel

### 9.1 Valeurs sûres par défaut

Une configuration conforme utilise par défaut :

- `enabled = false` ;
- bind loopback IPv4 et IPv6 uniquement ;
- découverte désactivée ;
- `Cockpit` comme seul mode autorisé ;
- vue vidéo désactivée tant que renderer, readback et encodeur ne sont pas validés ;
- aucune adresse non-loopback sans allowlist explicite.

L'exemple `0.0.0.0` de l'analyse n'est pas un défaut normatif.

### 9.2 Validation de source

Après acceptation d'un `HELLO`, la session est liée au tuple transport source `(adresse, port)` et au `client_nonce`. Un changement de tuple exige une nouvelle négociation en FSTL 1.0.

L'allowlist accepte des adresses ou CIDR explicitement configurés. Une résolution DNS n'est pas réévaluée à chaque datagramme et ne remplace pas la validation de l'adresse reçue.

### 9.3 Anti-amplification

Avant preuve de retour par ACK valide de `WELCOME`, le producteur :

- n'envoie aucun manifeste, snapshot ou flux vidéo ;
- borne la taille cumulée de ses réponses à la taille cumulée reçue multipliée par trois ;
- ne réserve pas de ressources vidéo ou de snapshot volumineux ;
- applique le rate limit `HELLO` par adresse source.

L'ACK de `WELCOME` valide seulement la joignabilité du tuple, pas l'identité cryptographique du client.

### 9.4 Rate limits minimaux

Les limites exactes de session et vidéo figurent dans [03](03-session-horloges-fiabilite.md) et [05](05-capabilities-et-vues-specialisees.md). Toute implémentation DOIT au minimum borner séparément :

- `HELLO` par adresse source ;
- création de session par producteur ;
- `RESYNC_REQUEST` par session ;
- ACK/NACK par session et message cible ;
- changements de capabilities ;
- abonnements, reconfigurations et arrêts vidéo ;
- demandes d'IDR par flux ;
- débit vidéo par client et globalement.

Une entrée rate-limitée est ignorée ou reçoit une réponse compacte si celle-ci ne crée pas d'amplification. Elle ne modifie jamais la simulation.

### 9.5 Limites de ressources

Les maxima FSTL 1.0 incluent :

| Ressource | État/fiable | Vidéo |
|---|---:|---:|
| taille logique d'un message | 1 048 576 octets | 2 097 152 octets |
| fragments par message | 1024 | 2048 |
| réassemblages simultanés/client | 4 | 3 |
| mémoire de réassemblage/client | 4 Mio | 6 Mio |
| payload par datagramme | 1132 octets | 1132 octets |

Une configuration PEUT abaisser ces valeurs. La multiplication par le nombre maximal de clients est vérifiée au démarrage ; un overflow ou un budget global impossible désactive le module avec diagnostic.

Une réservation vidéo ne peut jamais évincer un message d'état fiable. En cas de surcharge ou de `WOULD_BLOCK`, l'émetteur abandonne dans cet ordre les `TARGET_VIDEO_STATS`, les interframes, puis les fragments non encore envoyés d'une nouvelle IDR ; il ne bloque jamais le thread de jeu et n'abandonne jamais un ACK ou un snapshot au profit de la vidéo.

## 10. Golden vectors

### 10.1 Arborescence requise

L'implémentation de Phase 0 DOIT produire au minimum :

```text
protocol-v1/
├── schema/
│   └── fstl-v1.yaml
├── vectors/
│   ├── valid/
│   │   ├── datagrams/
│   │   ├── messages/
│   │   └── records/
│   └── invalid/
│       ├── datagrams/
│       ├── messages/
│       └── records/
├── expected/
│   └── *.json
└── README.md
```

Les fichiers `.bin` contiennent uniquement les octets à décoder. Les métadonnées `.json` de même basename contiennent l'attendu, jamais un remplacement des octets.

### 10.2 Métadonnées minimales

Chaque fixture déclare :

```json
{
  "schema": "FSTL-1.0",
  "name": "full_snapshot_minimal",
  "kind": "datagram-sequence",
  "valid": true,
  "messageType": 6,
  "inputFiles": ["000.bin"],
  "expectedCanonicalJson": "full_snapshot_minimal.json",
  "expectedValidationError": 0,
  "notes": "ASCII only; one record"
}
```

Pour une fixture invalide, `expectedCanonicalJson` est absent et `expectedValidationError` est non nul. Les offsets et CRC attendus PEUVENT être dupliqués dans les métadonnées pour faciliter le diagnostic, mais les octets `.bin` restent autoritaires.

Les `u64` et `i64` du JSON canonique sont écrits en chaînes décimales afin d'éviter la perte de précision des consommateurs JavaScript. Les byte strings sont hexadécimales minuscules. Les floats utilisent une représentation décimale round-trip et `-0` est canonisé selon la règle du champ.

### 10.3 Valeurs déterministes de fixtures

Les vecteurs utilisent des valeurs fixes, jamais un générateur aléatoire à l'exécution :

- `producer_id = 0x0102030405060708` ;
- `session_id = 0x1122334455667788` ;
- `client_nonce = 0x8877665544332211` ;
- premiers IDs métier à `1` ;
- timestamps simples, distincts et documentés ;
- chaînes ASCII puis cas UTF-8 multioctets dédiés ;
- CRC calculés par un outil indépendant et vérifiés contre la valeur de contrôle ISO-HDLC.

### 10.4 Catalogue minimal valide

Au moins un vector valide est requis pour :

- chaque scalaire, chaîne, bytes et liste ;
- un payload logique vide pour tester isolément CRC/fragmenter/réassembleur — aucun `MessageType` v1 n'accepte ce payload comme message applicatif —, un message valide non fragmenté et un message valide multi-fragments ;
- chacun des 20 `MessageType` définis en v1 ;
- chacun des 28 `RecordType` ;
- chaque enum et chaque bit de flag/capability v1 ;
- `HELLO` accepté et refusé, `WELCOME`, échange de heartbeat complet ;
- ACK `VALIDATED`, ACK `APPLIED` et NACK avec bitmap ;
- manifeste, snapshot initial, keyframe candidate et delta cumulatif ;
- perte d'un delta intermédiaire suivie d'un delta plus récent ;
- changement de baseline avec modification pendant l'aller-retour de l'ACK ;
- `COMM_ASSET_MANIFEST`, `COMM_VIEW_STATE`, `COMM_VIEW_EVENT START/STOP` ;
- tous les messages `TARGET_VIDEO_*`, dont IDR fragmentée ;
- wrap proche des compteurs soumis à arithmétique sérielle ;
- chaînes UTF-8, quaternion et limites min/max métier.

### 10.5 Catalogue minimal invalide

Les fixtures invalides couvrent au minimum :

- chaque longueur possible de troncature d'un en-tête ;
- magic, version, header size et flags réservés invalides ;
- taille réelle différente de la taille déclarée ;
- CRC datagramme et message erronés ;
- fragment count nul, index hors borne, offset non canonique et tranche overflow ;
- fragments incohérents, chevauchants ou doublons contradictoires ;
- message et liste au-delà des maxima ;
- quotas dépassés avant allocation ;
- payload fixe tronqué ou avec trailing bytes ;
- record tronqué, longueur excessive, singleton dupliqué et item key dupliquée ;
- UTF-8 invalide et chaîne trop longue ;
- NaN, `+Inf`, `-Inf`, booléen hors `{0,1}` et enum fermée inconnue ;
- quaternion non fini, nul, non normalisable ou non canonique ;
- baseline, génération, stream, cible ou manifeste inconnus ;
- ACK/NACK forgé, tardif, mauvais endpoint, mauvais CRC ou bitmap incohérent ;
- bundle/hash/durée de communication incohérents ;
- profil/codec/résolution/bitrate vidéo non négociés ;
- `encoded_frame_size` différent des octets restants ;
- ancienne frame après changement de cible ;
- message client simulant une commande non définie.

## 11. Tests unitaires et de propriété

### 11.1 Sérialisation

Pour chaque type :

- round-trip valeur → octets → valeur ;
- comparaison octet à octet au golden vector ;
- test sur buffer exactement dimensionné ;
- échec sur buffer d'un octet trop court ;
- invariance du résultat entre compilateurs/plateformes ;
- absence de padding ;
- ordre little-endian explicite ;
- rejet des non-finis et valeurs hors domaine ;
- test de tous les bits de présence et combinaisons interdites.

### 11.2 Propriétés

Des tests génératifs vérifient :

- `decode(encode(x)) == canonicalize(x)` ;
- `encode(decode(golden)) == golden` pour toute fixture canonique ;
- aucune entrée de longueur `n` ne lit au-delà de `n` ;
- une erreur reste sticky dans reader/writer ;
- le fragmenter puis réassembleur reproduit exactement le payload ;
- toute permutation des fragments valides donne le même message ;
- toute duplication identique est idempotente ;
- un delta plus récent de même baseline suffit sans les précédents ;
- l'application atomique ne publie jamais un sous-ensemble de records ;
- les budgets mémoire ne sont jamais dépassés quelle que soit la séquence d'entrées.

### 11.3 Arithmétique

Les tests couvrent :

- addition/multiplication proches de `SIZE_MAX` ;
- conversions `u64`/`i64` ;
- différence de timestamps dans un entier signé élargi ;
- division et arrondi de l'offset d'horloge ;
- arithmétique sérielle avant/après wrap ;
- interpolation de communication avec taux positif, nul et négatif ;
- modulo euclidien et clamp ;
- normalisation et signe canonique du quaternion ;
- `-0.0`, subnormaux finis et valeurs float maximales autorisées.

## 12. Tests de session et transport simulé

Un harness sans moteur simule deux endpoints et injecte :

- perte indépendante et par rafales ;
- duplication ;
- réordonnancement ;
- jitter ;
- coupure puis reprise ;
- client lent ou silencieux ;
- `WOULD_BLOCK` ;
- ACK perdus ;
- fragments manquants ;
- session remplacée ;
- wrap de séquence ;
- resynchronisations répétées.

Scénarios obligatoires :

1. handshake accepté puis installation manifeste/snapshot ;
2. refus de version sans création de session ;
3. anti-amplification avant ACK du `WELCOME` ;
4. retransmission idempotente du même snapshot après ACK perdu ;
5. delta intermédiaire perdu, delta cumulatif récent appliqué ;
6. delta de la baseline candidate connue mis en attente selon la limite puis appliqué après commit ; delta d'une baseline totalement inconnue abandonné immédiatement puis `ResyncRequest UnknownBaseline` rate-limité ;
7. keyframe candidate reçue en désordre avec les deltas de l'ancienne baseline ;
8. mutation, création et suppression entre capture de keyframe et `ACK APPLIED` ;
9. timeout de réassemblage fiable et renouvellement par nouvelle keyframe ;
10. fin de session perdue puis détectée par timeout ;
11. retrait dynamique d'une capability sans perte de la télémétrie d'état ;
12. flux vidéo saturé sans retard des ACK, snapshots ou deltas.

Les profils UDP de 1 %, 5 % et 20 % pendant dix minutes appartiennent à l'intégration des phases ultérieures ; la Phase 0 DOIT néanmoins fournir le harness, les graines déterministes et les assertions de priorité nécessaires.

## 13. Tests des vues spécialisées

### 13.1 Communication

- bundle compatible, absent, ancien et hash invalide ;
- collision d'`asset_id` tronqué ;
- manifeste non canonique ;
- asset absent dans un bundle annoncé compatible ;
- `START` dupliqué et `STOP` retardé ;
- remplacement `STOP` puis `START` ;
- connexion pendant lecture ;
- offset non nul, pause, accélération et lecture inverse ;
- correction périodique et resynchronisation ;
- durée d'état différente du manifeste ;
- changement de mission/session ;
- capability producteur absente mais état général `Live`.

### 13.2 Vidéo cible

- subscribe accepté, rejeté et retransmis avec le même `request_id` ;
- aucune intersection codec/profil/niveau/render profile ;
- dimensions impaires, hors limite ou préférence supérieure au maximum ;
- configuration acquittée avant première IDR ;
- interframe incomplète abandonnée à 200 ms ;
- NACK sélectif d'IDR dans la fenêtre de 500 ms ;
- expiration sans retransmission tardive puis nouvelle demande d'IDR ;
- changement de cible/génération et purge ;
- ancienne frame jamais présentée ;
- désabonnement idempotent ;
- perte d'encodeur et `CAPABILITY_UPDATE` ;
- stats remplaçables et rate-limitées ;
- quotas vidéo incapables d'évincer l'état.

## 14. Fuzzing

### 14.1 Cibles obligatoires

Des fuzz targets séparés couvrent :

- parseur d'en-tête ;
- CRC et calcul de tranche ;
- réassembleur multi-datagrammes stateful ;
- chaque payload de contrôle ;
- séquence de records ;
- chaque record métier ;
- UTF-8 et chaînes de chemin ;
- ACK/NACK et bitmaps ;
- snapshot/delta/baseline stateful ;
- manifeste de communication ;
- métadonnées vidéo, sans invoquer un décodeur H.264 réel en Phase 0.

### 14.2 Invariants du fuzzing

Pour toute entrée :

- aucun crash, abort non contrôlé ou exception non capturée ;
- aucune lecture/écriture hors borne ;
- aucune allocation dépassant les budgets ;
- aucun temps de traitement superlinéaire non justifié par une limite faible ;
- aucun état partiellement publié ;
- résultat déterministe pour une même entrée et un même état initial ;
- pas de réponse réseau amplifiante à une source non validée.

Les builds de fuzzing activent au minimum AddressSanitizer et UndefinedBehaviorSanitizer sur les plateformes compatibles ; les jobs Windows utilisent les équivalents disponibles. Le corpus initial contient tous les golden vectors.

## 15. Compatibilité croisée

La gate exige deux chemins indépendants :

- implémentation de production prévue en C++ ;
- décodeur de référence dans un langage ou une base de code ne réutilisant pas ses structures.

Ils doivent :

- décoder les mêmes `.bin` valides vers le même JSON canonique ;
- rejeter les mêmes `.bin` invalides avec le même code principal ;
- produire les mêmes octets canoniques ;
- passer sur au moins une machine little-endian et un test d'endianness simulé/big-endian ;
- vérifier la valeur de contrôle CRC indépendante ;
- échanger `HELLO/WELCOME/HEARTBEAT/ACK/NACK/RESYNC_REQUEST` dans le harness UDP.

Un test qui ne fait que comparer deux fonctions partageant le même encodeur n'est pas une preuve croisée.

## 16. Observabilité obligatoire

Les compteurs suivants sont exposés sans journaliser les payloads :

- datagrammes/octets reçus, acceptés et abandonnés ;
- rejets par `ValidationError` ;
- fragments reçus, dupliqués, contradictoires et expirés ;
- mémoire de réassemblage actuelle et pic ;
- messages CRC-valides et messages appliqués ;
- ACK/NACK envoyés, reçus, ignorés et rate-limités ;
- retransmissions par classe ;
- resync demandés, acceptés et refusés ;
- baseline active/candidate, âge et delta sequence ;
- messages abandonnés par priorité ;
- état de session et capabilities ;
- erreurs de manifeste/bundle ;
- métriques vidéo définies par [05](05-capabilities-et-vues-specialisees.md).

Les logs incluent type, session tronquée pour diagnostic, endpoint et code d'erreur. Ils NE DOIVENT PAS inclure contenu cargo caché, access unit H.264, chemin absolu, secret externe ou dump complet par frame.

## 17. Critères de sortie de conformité

La Phase 0 satisfait ce document lorsque :

- toutes les interfaces bornées sont implémentées et testées sans moteur ;
- le schéma machine-readable est validé contre les tableaux normatifs ;
- le catalogue complet de vectors valides et invalides existe ;
- deux implémentations indépendantes passent les fixtures ;
- la couverture des 20 messages, 28 records, enums, flags et capabilities est automatique ;
- le harness prouve la convergence des baselines et deltas cumulatifs ;
- le fuzzing atteint le corpus et les parseurs sans défaut bloquant connu ;
- les budgets sont vérifiés avant allocation dans tous les chemins ;
- les valeurs sûres par défaut et rate limits sont testés ;
- aucun payload client ne commande la simulation ;
- les résultats et versions d'outils sont reproductibles en CI.
