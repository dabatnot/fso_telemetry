# 02 — Architecture, contrats et interfaces

## 1. Objet

Ce document définit les composants Phase 3 et leur frontière avec FS2Open. Les
interfaces prolongent l’architecture Phase 2 sans introduire de thread, de
socket ou de protocole parallèle.

## 2. Pipeline produit

```text
EngineUpdate (thread principal)
  -> EngineSourceGuard
  -> TargetingCollector
  -> CockpitSensorProjection
  -> ThreatCargoNavigationCollector
  -> filtre COCKPIT
  -> SensorIdentityRegistry
  -> catalogue autorisé
  -> Phase3Image
  -> diff cumulatif / keyframe
  -> encodeur FSTL existant
```

L’ordre est normatif. En particulier, le registre public et le catalogue ne
voient jamais un objet rejeté par le filtre `COCKPIT`.

## 3. Composants

### 3.1 `EngineSourceGuard`

Il valide au même tick :

- `Player`, `Player_obj`, `Player_ship` et `Player_ai` ;
- `objnum`, `instance`, type, signature et génération ;
- cible courante/précédente et sous-systèmes associés ;
- chaque `lock_info` et sa banque d’arme ;
- objets examinés par le radar et classes correspondantes ;
- missiles entrants, cible et sous-système homing ;
- navpoints, waypoint lists et index ;
- sources de cargo et de scan.

Une référence invalide ne devient ni ID zéro improvisé, ni objet partiellement
copié. Le collecteur retourne une cause fermée et la projection candidate n’est
pas publiée.

### 3.2 `TargetingCollector`

Le collecteur copie dans un DTO possédé :

- cible courante et précédente ;
- identité effectivement révélée ;
- sous-système ciblé et sous-système de lock ;
- dernière observation furtive autorisée ;
- tendances, cône, lead et banque ;
- attaquant, arme dangereuse et cible verrouillée la plus proche ;
- liste complète des locks.

Les caches écran, accumulateurs de pixels, animations de lock et structures HUD
ne traversent pas l’interface.

### 3.3 `CockpitSensorProjection`

Cette projection produit la même décision de visibilité que le cockpit local.
Elle consomme les prédicats moteur de radar, AWACS, furtivité, cloak, tag, IFF,
EMP et visibilité de piste. Si la logique est aujourd’hui enfermée dans un gauge, elle
PEUT être extraite dans un helper moteur générique pur partagé par le HUD et la
télémétrie. Ce helper :

- ne dépend pas de l’activation réseau ;
- ne connaît ni FSTL, ni client, ni test ;
- reçoit uniquement des sources moteur validées ;
- retourne une décision de visibilité et les valeurs publiques autorisées ;
- ne modifie aucun objet moteur.

Dupliquer approximativement la logique du HUD dans le sérialiseur ou injecter
une liste de contacts de test dans le produit est interdit.

### 3.4 `ThreatCargoNavigationCollector`

Il copie :

- le niveau de menace et les missiles entrants autorisés ;
- l’état de scan pairwise du joueur ;
- le texte cargo uniquement après divulgation ;
- les navpoints, la route et la décision d’autopilote autorisés.

Les sources sont copiées au même tick que leur décision de visibilité. Une
navigation liée à une entité cachée omet le lien si le layout le permet, ou
omet le navpoint lorsque le point lui-même n’est pas autorisé.

### 3.5 `SensorIdentityRegistry`

Le registre mappe une clé interne validée `(object_signature, object_type)` vers
un `entity_id` public :

- allocation au premier instant où l’objet devient autorisé ;
- ID strictement croissant et non nul ;
- même ID lors d’une nouvelle observation de la même signature ;
- aucune réutilisation après destruction ou changement de signature ;
- aucune entrée publique pour un objet jamais autorisé ;
- capacité fixe de 65 536 identités par session.

Le mapping privé peut retenir une signature momentanément non visible afin de
réutiliser son ID si la même piste réapparaît. Il n’est jamais sérialisé.

Les entités de la fermeture Phase 2 utilisent le registre hérité. Lorsqu’un
objet de cette fermeture apparaît aussi comme contact, cible ou menace, tous les
records réutilisent le même `entity_id`.

## 4. DTO publics internes

Les DTO logiques sont :

```text
TargetingProjection
  target
  locks[0..64]

SensorProjection
  radar
  contacts[0..4096]
  threat

OperationsProjection
  cargo_scan
  navigation

Phase3Projection
  inherited_complete_ship
  targeting
  sensors
  operations
  sample_times
  catalog_dependencies
```

Ils possèdent leurs chaînes et collections dans des capacités préallouées. Leur
forme n’est pas une ABI publique et ne doit pas reproduire le layout des
structures moteur.

## 5. Filtrage avant exposition

Pour chaque objet candidat, la projection décide dans cet ordre :

1. objet valide et vivant au sample time ;
2. connaissance par le joueur observé ;
3. visibilité radar `NOT_VISIBLE`, `VISIBLE` ou `DISTORTED` ;
4. identité, nom, classe, équipe et IFF effectivement révélés ;
5. relations et sous-systèmes publiables ;
6. classe ou arme de catalogue nécessaire ;
7. allocation ou réutilisation de l’ID public ;
8. création de l’atome.

Un contact `NOT_VISIBLE` n’est publié que si le moteur conserve une piste
autorisée ou si un état de ciblage autorisé exige sa dernière observation. Un
objet que le cockpit ne connaît pas est absent, même si sa vérité existe dans
les tables moteur.

## 6. Catalogues

Le `catalog_fingerprint` Phase 3 étend celui de Phase 2 avec :

- le groupe `RADAR_ICON` des classes où il est applicable et autorisé ;
- les classes de vaisseau référencées par `revealed_class_id` ;
- les classes d’armes référencées par un contact ou un missile entrant ;
- les définitions statiques nécessaires aux calculs de lock déjà autorisées.

Le fingerprint exclut les IDs alloués, les signatures, les positions, la
visibilité courante et la composition courante des contacts.

Une définition n’entre dans le manifeste que si un champ public peut la
référencer. Le producteur ne précharge pas toutes les classes de mission « au
cas où ». Lorsqu’une nouvelle définition devient révélable :

1. retenir l’image courante ;
2. construire le manifeste `N+1` exhaustif ;
3. publier et attendre `APPLIED(N+1)` ;
4. produire une keyframe exhaustive qui référence `N+1` ;
5. exposer le nouveau champ révélé seulement dans cette keyframe.

## 7. Interfaces runtime

Les interfaces minimales sont conceptuellement :

```cpp
CaptureResult capture_targeting(const EngineView&, TargetingProjection&) noexcept;
CaptureResult project_cockpit_sensors(const EngineView&, SensorProjection&) noexcept;
CaptureResult capture_operations(const EngineView&, OperationsProjection&) noexcept;
ValidationResult validate_phase3_projection(const Phase3Projection&) noexcept;
```

Les signatures concrètes peuvent évoluer, mais les propriétés suivantes sont
obligatoires :

- appels uniquement sur le thread principal ;
- aucune allocation après `Ready` ;
- aucune exception traversant la boucle moteur ;
- aucune écriture gameplay ;
- causes d’échec fermées ;
- durée et cardinalité observables.

## 8. Frontière moteur

Les chemins moteur attendus sont les autorités existantes de :

- `Player_ai` et l’état de ciblage ;
- lock secondaire et banques d’armes ;
- filtrage radar/HUD et fonctions AWACS ;
- flags furtivité, cloak, tag, EMP et visibilité de piste ;
- recherche de missiles visant le joueur ;
- logique de scan cargo ;
- navigation et autopilote.

Un hook moteur n’est justifié que si une décision produit ne peut pas être
observée autrement. La Phase 3 ne promet aucune capture exacte de transition
brève et n’ajoute donc aucun hook événementiel spécialisé.

## 9. Client de référence

Le client de référence :

- décode les records avec le parseur indépendant ;
- installe manifeste et snapshot atomiquement ;
- maintient les atomes de contacts par clé ;
- expose valeur brute, sample time et provenance ;
- calcule les valeurs `D` dans une couche séparée ;
- ne reconstruit jamais une identité omise ;
- ne transforme pas `NOT_VISIBLE` en vérité monde ;
- purge les atomes à `DELETE`, keyframe ou changement de session.

## 10. Traçabilité

Ce document couvre `P3-REQ-009` à `P3-REQ-018`, `P3-REQ-037`,
`P3-REQ-043` à `P3-REQ-045`.
