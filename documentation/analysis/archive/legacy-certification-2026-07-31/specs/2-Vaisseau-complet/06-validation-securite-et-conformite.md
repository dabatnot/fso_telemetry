# 06 — Validation, sécurité et conformité

<!-- certification-budget-minutes: 660 -->
<!-- failure-policy: dependency-scoped-continue-independent -->

## 1. Objet

Ce document définit les tests, oracles, scénarios réseau, contrôles de sécurité et preuves nécessaires pour fermer la Phase 2. Une compilation réussie ou une démonstration visuelle ne suffit pas.

## 2. Ordre de validation obligatoire

Chaque entrée est validée dans cet ordre :

1. configuration et budgets avant socket ;
2. datagramme : longueur, en-tête, magic, version, type, flags et CRC ;
3. endpoint, allowlist, session, séquences, quotas et anti-amplification ;
4. fragmentation/réassemblage avec taille promise bornée ;
5. transaction : part, compte, taille, SHA-256 et ordre ;
6. records : type/version/flags/longueur, champs et UTF-8 ;
7. manifeste : exhaustivité, IDs, références et closure ;
8. matrice de domaines et cohérences croisées ;
9. commit atomique ;
10. ACK `APPLIED`, promotion de baseline et exposition au tableau de bord.

Une erreur arrête le pipeline et produit exactement une raison primaire. Aucun objet partiel n’est exposé.

## 3. Stratégie de preuve

### 3.1 Niveaux

| Niveau | Objet | Indépendance requise |
|---|---|---|
| unitaires | conversions, bornes, closure, IDs, formules | fixtures sans moteur |
| protocolaires | codecs, transactions, validation, goldens | encodeur et décodeur de référence comparés |
| intégration runtime | sessions, capture, lifecycle, ACK, files | faux moteur + transport injecté |
| intégration native | build FS2Open et mission réelle | processus producteur + client séparé |
| dégradation réseau | loss/jitter/reorder/duplication | proxy/harness déterministe |
| oracle métier | valeur moteur vs valeur décodée | capture oracle et décodeur indépendant |
| performance/endurance | budgets, allocations, fuite, relance | Release et échantillons bruts |
| sécurité/fuzz | entrées hostiles, limites et confidentialité | corpus + fuzzer reader |

### 3.2 Décodeur indépendant

Le client de preuve étend `test/telemetry/protocol/tools/fstl_console_client.py`. Il NE DOIT importer ni appeler le codec C++ du producteur. Il lit le schéma/goldens, valide transactions et couverture, maintient manifeste/baseline, puis affiche l’état Phase 2.

Le décodeur doit reconnaître au minimum les records 1–14, 20–22 et `EVENTS` type 28, les manifests 3/4, les cinq kinds lifecycle `1 ENTITY_APPEARED`, `17 SHIP_DISABLED`, `18 SHIP_DYING_STARTED`, `19 ENTITY_DESTROYED`, `3 ENTITY_DISAPPEARED`, ainsi que les mutations de delta. Un type inconnu suit strictement la politique FSTL, sans heuristique.

### 3.3 Cadence, risque et budget

La boucle `inner-loop` utilise uniquement une cible dédiée et un oracle déterministe court. Le `wp-checkpoint` lie les preuves Release directement invalidées. La `gate-certification` est réservée aux risques qui ne peuvent pas être fermés par ces preuves courtes.

Toute opération de plus de cinq minutes DOIT déclarer son risque, sa durée estimée, sa qualification, son critère d’arrêt du scénario, son éventuel arrêt partagé, son domaine d’échec, son cône d’invalidation et sa clé de réutilisation. Une campagne de certification NE DOIT PAS servir au diagnostic : un échec arrête le scénario concerné, conserve son résultat brut, place son cône dépendant en `deferred-by-dependency` et programme un reproducer court. Les preuves indépendantes continuent tant que leurs racines de confiance partagées restent valides.

| Preuve | Risque couvert | Cadence | Durée estimée | Qualification | Commande ou procédure | Critère d’arrêt du scénario | Arrêt partagé | Domaine d’échec et suite autorisée | Invalidation | Réutilisation | Propriétaire / gate |
|---|---|---|---:|---|---|---|---|---|---|---|---|
| build Release ciblé + qualifications WP11 | harness ou binaire non représentatif | `wp-checkpoint` | 20 min | tests contractuels et deux scénarios courts | build et tests Release ciblés | build/test rouge de la cible | toolchain, configuration ou artefact commun invalide | échec local à la cible ; différer ses consommateurs, continuer les preuves indépendantes | sources, tests, harness, CMake | `P2-EV-WP11-READY` | WP11 / G2-F |
| refonte performance cadence/image/delta/frame | coût structurel du fast path et oracle frame non représentatif | `wp-checkpoint` | 90 min | tests déterministes du masque, de la conservation atomique, du dirty-set, du comptage keyframe et de la mission témoin | build Release ciblé, deux qualifications courtes dont le cas v22 historiquement rouge, puis revue indépendante | premier contrat rouge ou seuil court dépassé ; au plus deux qualifications après correction | source commune, mission témoin, horloge ou oracle frame invalide | échec limité au cône performance ; conserver v22, différer certification performance/soak et poursuivre les audits indépendants autorisés | capture runtime, builder image, diff, harness performance, mission témoin | `P2-EV-PERF-INCREMENTAL-READY` | WP11 / G2-F |
| 12 smokes de perte | cellule de matrice non exercée | `gate-certification` | 10 min | `core-gate-20-burst` et `complete-ship-20-burst` verts en court | runner loss sur la matrice smoke | scénario rouge ou rapport incomplet | oracle loss, seed, environnement ou fingerprint partagé invalide | échec local au scénario ; différer son cône loss, poursuivre performance et soak qualifiés | runtime, harness, oracle, profils, seed | `P2-EV-LOSS-SMOKE` | WP11 / G2-F |
| 4 scénarios de perte représentatifs | convergence aux frontières de risque | `gate-certification` | 25 min | matrice smoke 12/12 verte ou dépendances explicitement qualifiées | runner loss avec les quatre cas nommés | scénario rouge ou borne dépassée | oracle loss, seed, environnement ou fingerprint partagé invalide | échec local au scénario ; différer ses consommateurs, poursuivre les preuves indépendantes | mêmes dépendances que le smoke | `P2-EV-LOSS-LONG` | WP11 / G2-F |
| performance contrôle/actif | hitch, allocation ou p99 hors budget | `gate-certification` | 45 min | runner court, compteurs et percentiles qualifiés | runner performance contrôle puis actif | seuil dépassé ou rapport incomplet | hôte, horloge, configuration ou fingerprint partagé invalide | échec local à performance ; différer AC-013, poursuivre loss et soak indépendants | runtime, runner, flags, hôte | `P2-EV-PERF` | WP11 / G2-F |
| soak composite | fuite, deadlock ou défaut de lifecycle tardif | `gate-certification` | 60 min | scénario composite court vert | runner soak composite | fuite, croissance, deadlock ou état final faux | harness, environnement ou fingerprint partagé invalide | échec local au soak ; différer AC-015, poursuivre loss et performance indépendants | runtime, harness, configuration composite | `P2-EV-SOAK` | WP11 / G2-F |
| audit et revue de gate | preuve incohérente ou périmètre dépassé | `gate-certification` | 20 min | preuves précédentes conclusives ou états différés explicités | audit des rapports, empreintes, tracker et diff | preuve absente, contradictoire ou empreinte stale | racine de confiance commune ou tracker canonique invalide | garder G2-F ouverte ; auditer les preuves non affectées et produire la liste des cônes différés | rapports, tracker, diff | `P2-EV-G2F-AUDIT` | WP12 / G2-G |

Le budget nominal est donc de 180 minutes, égal au plafond nominal de 180 minutes. Les 45 minutes de performance comprennent 2 400 s mesurées, quatre warm-ups de 60 s et au plus 60 s d’orchestration et de validation du rapport. Une preuve WP09 ou WP10 reste réutilisable tant que son empreinte de production, test, harness, build et configuration n’est pas invalidée.

Chaque scénario prend l’un des états `passed`, `failed`, `deferred-by-dependency` ou `not-run`. Un résultat rouge reste `failed`, n’est jamais agrégé ni réécrit en succès, et maintient la gate ouverte. Seul son cône dépendant est différé ; les preuves indépendantes restent autorisées. Un arrêt global n’est permis que si une racine de confiance partagée — build commun, oracle commun, environnement qualifié, fingerprint ou tracker canonique — est invalide.

### Certification budget exception

L’utilisateur a approuvé explicitement le 30 juillet 2026 l’exception `P2-WP11-G2F-COMPLETION-BUDGET-EXCEPTION-20260730` de 100 minutes, portant le plafond total de WP11/G2-F à 280 minutes. Le budget nominal des preuves reste de 180 minutes ; l’exception couvre uniquement le diagnostic de l’écart entre le contrat et l’outillage, la clarification normative indispensable, l’adaptation test-only de l’outillage, ses qualifications ciblées et la revue indépendante.

Le risque est classé `critical` : une campagne annoncée comme conforme pourrait fermer faussement G2-F alors qu’elle exécute des workloads séparés, un ordre contrôle/actif non défini ou une durée supérieure au contrat. Les alternatives rejetées sont la campagne historique de 434 minutes, la concaténation a posteriori de rapports non coordonnés, la répétition des preuves WP01 à WP10 et l’utilisation d’une campagne longue comme outil de diagnostic.

La première exception est ponctuelle, limitée à WP11/G2-F, et NE DOIT PAS être réutilisée implicitement pour autoriser un nouveau défaut ou augmenter le budget d’une campagne future.

L’utilisateur a approuvé explicitement le 31 juillet 2026 une seconde exception, `P2-PHASE-COMPLETION-BUDGET-EXCEPTION-20260731`, de 170 minutes supplémentaires, portant le plafond cumulé à 450 minutes. Elle couvre exclusivement les 20 minutes de rebuild et qualifications ciblées encore invalidées, les 25 minutes de perte représentative, les 45 minutes de performance, les 60 minutes de soak composite et les 20 minutes d’audit final G2-F/G2-G. Elle n’autorise ni la répétition des preuves fraîches WP01–WP10 ou loss smoke, ni une campagne de diagnostic, ni une extension implicite supplémentaire.

Le risque reste `critical` : sans ces preuves, G2-F/G2-G ne peuvent pas distinguer un défaut réel de performance/endurance d’un défaut du fixture de certification. Les alternatives rejetées sont la clôture sur preuve partielle, la réécriture des rouges historiques, la réduction des durées contractuelles et le replay des preuves déjà fraîches.

L’utilisateur a approuvé explicitement le 31 juillet 2026 une troisième exception, `P2-PERFORMANCE-RESTART-ALLOCATION-BUDGET-EXCEPTION-20260731`, de 30 minutes supplémentaires, portant le plafond cumulé à 480 minutes. Elle couvre exclusivement un reproducer court de l’allocation observée après le premier restart composite à quatre clients, sa correction minimale, le rebuild et la requalification ciblés, puis la campagne normative unique performance + soak et l’audit final. Après deux reproductions rouges et l’identification statique d’une identité `SubsystemState` préallouée à 8 octets au lieu des 12 octets requis, l’utilisateur a autorisé explicitement une seconde stratégie bornée : un auto-test déterministe du contrat de capacité, la seule correction 8 → 12 octets pour ce type, puis une unique requalification ciblée exigeant zéro croissance agrégée. Cette autorisation NE couvre aucune troisième stratégie, aucun replay de preuve loss/WP01–WP10 et aucune réduction des durées ou seuils contractuels. Les alternatives rejetées sont le lancement d’une campagne de 105 minutes avec un smoke rouge, l’acceptation d’allocations après `Ready`, et la clôture sans audit.

L’utilisateur a approuvé explicitement le 31 juillet 2026 une quatrième exception, `P2-HOST-PREFLIGHT-AND-FINAL-AUDIT-BUDGET-EXCEPTION-20260731`, de 20 minutes supplémentaires, portant le plafond cumulé à 500 minutes. Elle couvre la preuve hôte alimentation/température/throttling, la campagne normative unique de 105 minutes et l’audit indépendant final de 20 minutes. Le même message préautorise toutes les extensions ultérieures strictement nécessaires à la clôture de la Phase 2 : chaque relèvement effectif DOIT néanmoins être chiffré et consigné avec son risque et son périmètre, sans nouvelle demande utilisateur. Cette préautorisation NE permet ni replay de preuve fraîche, ni réduction de durée ou de seuil, ni campagne de diagnostic longue.

Sous cette préautorisation, le coordinateur consigne une cinquième exception, `P2-HOST-MONITOR-INTEGRATION-BUDGET-EXCEPTION-20260731`, de 10 minutes, portant le plafond cumulé à 510 minutes. Elle couvre exclusivement l’intégration fail-closed du moniteur hôte, son rebuild Release, une qualification courte et la revue de readiness ; elle ne couvre aucune répétition de campagne normative.

Sous la même préautorisation, le coordinateur consigne une sixième exception, `P2-PERFORMANCE-CAPTURE-OPTIMIZATION-BUDGET-EXCEPTION-20260731`, de 45 minutes, portant le plafond cumulé à 555 minutes. Elle couvre exclusivement la conservation des qualifications rouges v19/v20, le diagnostic par chronométrage des sous-étapes, la suppression des effacements de buffers inactifs, le rebuild Release et au plus deux qualifications courtes de la correction. Elle réserve sans réduction les 105 minutes de campagne normative et les 20 minutes d’audit final ; elle n’autorise ni campagne longue de diagnostic, ni assouplissement des seuils, ni replay d’une preuve verte.

Sous la même préautorisation, le coordinateur consigne une septième exception, `P2-PERFORMANCE-MEASUREMENT-PROTOCOL-BUDGET-EXCEPTION-20260731`, de 15 minutes, portant le plafond cumulé à 570 minutes. Elle couvre uniquement la correction test-only approuvée par revue indépendante (durées frame/télémétrie exclusives, événement keyframe réel, enveloppes comparables, auto-test d’agrégation), son rebuild et l’unique requalification courte v22. Le rouge v22 interdit toute nouvelle correction production ou campagne longue sans décision explicite sur l’élargissement de stratégie.

Le 31 juillet 2026, l'utilisateur autorise explicitement l'élargissement de stratégie `P2-PERFORMANCE-INCREMENTAL-PIPELINE-BUDGET-EXCEPTION-20260731`. Sous la préautorisation permanente, le coordinateur consigne 90 minutes supplémentaires et porte le plafond cumulé à 660 minutes. Le périmètre est strictement limité au masque de capture par cadence, à la conservation atomique des blocs non dus, à la construction incrémentale de l'image, au dirty-set de delta, au comptage keyframe sur toute la fenêtre de service et à une preuve frame sur mission native comparable. Il couvre le build ciblé, les contrats déterministes, deux qualifications courtes dont l'ancien pire cas v22 et les revues indépendantes. Il ne modifie aucun wire, seuil, cardinalité ou durée de certification et n'autorise ni campagne longue de diagnostic ni replay d'une preuve fraîche.

La durée cumulée de la Phase 2 NE DOIT PAS dépasser le plafond courant consigné par le marqueur et le tracker, soit 660 minutes à cette décision. Un résultat rouge arrête uniquement le scénario et son cône dépendant, conserve les artefacts et déclenche un reproducer court ; les preuves indépendantes continuent dans le budget restant. Une dérive de hash ou de fingerprint, un heartbeat absent depuis plus de 15 minutes, un environnement non qualifié, une preuve partagée non fraîche ou le plafond courant atteint arrête le périmètre dépendant. Aucun scénario rouge ni son cône n’est relancé avant un reproducer vert et la revue de son invalidation.

## 4. Oracles et tolérances

### 4.1 Paire de capture

En build de preuve, un `ProofOracleSink` reçoit au même `EngineUpdate` :

- le DTO validé avant sérialisation ;
- l’image canonique produite ;
- les octets envoyés et l’état décodé par le client indépendant ;
- la projection de tableau de bord.

Chaque échantillon porte `producer_sample_time_us`, `mission_generation`, `manifest_id`, `snapshot_id/baseline_snapshot_id` et `entity_id`. L’oracle ne relit jamais le moteur depuis un autre thread.

### 4.2 Comparaisons exactes

| Type | Critère |
|---|---|
| entiers, enums, flags, IDs, counts | égalité exacte |
| chaînes UTF-8 | octets exacts après canonisation documentée |
| listes | cardinalité, ordre canonique, clés et valeurs exacts |
| `float32` copié | bits exacts après canonisation `-0`; tolérance 1 ULP uniquement si une conversion arithmétique obligatoire précède l’écriture |
| quaternion converti | erreur absolue par composante `<=1e-5`, norme dans `[0.9999,1.0001]`, signe canonique |
| durées converties | valeur entière exacte de la fonction de conversion testée ; aucune sentinelle |
| ratios/progressions dérivés | erreur absolue `<=1e-6` face au calcul double de référence |
| norme de vitesse dérivée | erreur relative `<=1e-6` ou absolue `<=1e-6` près de zéro |
| absence conditionnelle | bit et groupe tous deux absents ; aucune valeur zéro substitutive |

Une comparaison tolérante n’autorise jamais NaN, infini, champ supplémentaire ou cardinalité différente.

### 4.3 Exhaustivité du tableau de bord

Le harness génère un inventaire machine-readable de chaque champ reçu, sa nature `A/C/D`, sa source ou formule et le résultat de comparaison. La gate exige :

- 100 % des champs `A/C` présents dans les fixtures comparés ;
- 100 % des champs `D` affichés liés à une formule testée ;
- zéro champ sans provenance ;
- zéro mismatch sur les scénarios de sortie ;
- chaque groupe de présence testé au moins une fois présent et une fois absent lorsqu’il est atteignable.

## 5. Matrice de tests

### 5.1 Configuration, build et mode désactivé

| ID | Scénario | Résultat attendu |
|---|---|---|
| `P2-TST-001` | config Phase 1 sans `systemsHz` | défaut 10, mêmes autres valeurs |
| `P2-TST-002` | bornes 1/20, types faux, 0/21, `systemsHz>flightHz` | bornes valides acceptées, schedulers indépendants, valeurs invalides refusées sans application partielle |
| `P2-TST-003` | clé inconnue/dupliquée, JSON profond/long | runtime désactivé avant socket |
| `P2-TST-004` | fichier absent et `enabled=false` | seuils fast path Phase 1, zéro socket/allocation récurrente |
| `P2-TST-005` | MSVC Debug/Release et CI non-Windows | build et suites fonctionnelles |
| `P2-TST-006` | inventaire CMake | chaque nouveau fichier déclaré, aucun glob implicite |
| `P2-TST-007` | seams cleanup/support/control/cargo, off puis on | mêmes contrôles et gameplay ; cargo avance au plus une fois par appel historique, mêmes resets/révélation/HUD ; hooks `noexcept`, zéro allocation/socket |
| `P2-TST-008` | `SOLO`, puis `MULTIPLAYER_CLIENT`, `MULTIPLAYER_MASTER`, dedicated/headless et `TrustedFullState` | seul `SOLO+COCKPIT+trusted=false` annonce Phase 2 ; les autres refusent le profil avant `WELCOME`, sans fuite ni allocation métier |
| `P2-TST-009` | injection records 15–19/23, `COMM_*`, vidéo et capability spécialisée sous `0x0583`; scan binaire/build | `InvalidAbsence` ou `CapabilityNotNegotiated` selon le validateur ; aucune dépendance FFmpeg/OpenGL/commande ajoutée au producteur Phase 2 |

### 5.2 Compatibilité et profils

| ID | Scénario | Résultat attendu |
|---|---|---|
| `P2-TST-010` | corpus FSTL 1.0 gelé | hash/arbre et octets inchangés |
| `P2-TST-011` | négociation minor 0 seulement | refus `UnsupportedVersion` |
| `P2-TST-012` | golden Phase 1 `0x0400` | validation inchangée |
| `P2-TST-013` | golden existant `phase2-promotion` `0x0401` | validation réussie, `9+N` records |
| `P2-TST-014` | `0x0401` sans manifeste ou record cœur | rejet précis |
| `P2-TST-015` | `CONTROL_STATE`, `WEAPON_STATE` ou `SUPPORT_STATE` sans domaine | rejet exact `InvalidAbsence` pour chaque fixture |
| `P2-TST-016` | profil final `0x0583`, closure K=1,2,64 | validation réussie avec exactement `4+10K+N` records ; minimum K=1 `14+N` |
| `P2-TST-017` | `0x0583` sans cargo, docking, support, control, weapon ou catalogues | rejet précis par chaque omission |
| `P2-TST-018` | tentative de mutation du mask par delta/capability update | `InvalidStateTransition`, `SESSION_END(ProtocolError,RECONNECT_ALLOWED)` et slot `FaultedSession` |
| `P2-TST-019` | ancien client/reader sur types connus v1 | comportement compatible documenté |

### 5.3 Manifestes et identités

| ID | Scénario | Résultat attendu |
|---|---|---|
| `P2-TST-020` | fermeture joueur non armé | classe installée, génération armes vide installée, snapshot valide |
| `P2-TST-021` | classes/armes dans ordre moteur aléatoire | mêmes octets/IDs canoniques |
| `P2-TST-022` | même nom de classe/arme avec `ClassDescriptor`/`WeaponDescriptor` différent ; nom auxiliaire dupliqué sur deux index | descripteurs différents donnent deux IDs ; ambiguïté d’un registre auxiliaire nommé est refusée, jamais résolue par l’index moteur |
| `P2-TST-023` | bornes chaînes, `record_length`, record count par part/image, banques classe, parts, transaction | 65 535 octets/records et 192 banques acceptés dans leurs scopes ; 65 536/193 et toute borne+1 rejetées avant egress |
| `P2-TST-024` | perte d’une part manifeste, duplication/réordre | aucun commit partiel, retransmission puis commit |
| `P2-TST-025` | SHA/taille/count/manifest generation incohérents | rejet avant exposition |
| `P2-TST-026` | ajout d’une instance déjà cataloguée puis changement de définition/loadout pendant ACK | topologie seule : keyframe sous N ; catalogue : `active=N`, `staged=N+1`, snapshot N+1 après `APPLIED`, troisième changement coalescé sans troisième génération |
| `P2-TST-027` | ID moteur `-1`, index recyclé, pointer-like value | `-1` devient zéro uniquement dans un champ autorisant l’absence ; index/pointeur sans clé publique donne `InvalidSource`; aucune valeur brute ne fuit |
| `P2-TST-028` | manifeste >16 Mio ou >64 parts | session refusée avant envoi |
| `P2-TST-029` | projection `CoreGate` face à une extension invalide ; closure `CompleteShip` : support-du-support, docking transitif/cyclique, leader, edges incohérents et racines équipe/proximité/cible/capteurs/parent/event/cargo | `CoreGate` reste `{joueur}` sans catalogue de l’extension ; seuls edges support/dock/leader valides ajoutent un membre à `CompleteShip`; edge non réciproque/signature incohérente refuse seulement ce profil ; racines interdites ne l’élargissent jamais ; K=65/N=4097 refusés |

### 5.4 Données métier

| ID | Scénario | Résultat attendu |
|---|---|---|
| `P2-TST-030` | coque overkill négative, protections, guardian threshold 0/100/>100, sim hull normal/training | coque canonisée à zéro ; guardian absent à 0, vaut exactement max à 100, et >100 refuse `OutOfRange` sans clamp ; SIM_HULL et provenance dégâts absents |
| `P2-TST-031` | K ships aux segment counts distincts 0/1/4/64 ; 65 sur un sujet | chaque record lit son propre sujet, états exacts, rejet du sujet à 65 |
| `P2-TST-032` | (a) `No_shields` avec max physique positif, (b) bouclier autorisé/max positif avec `max_shield_recharge=0`, (c) conteneur quadrants non vide malgré absence physique | (a) absent ; (b) présent avec `RECHARGE_MAX=0` mais segments/max issus de l’overload `true` ; (c) count zéro et conteneur ignoré ; tout groupe optionnel sur absence est `InvalidAbsence` |
| `P2-TST-033` | `ets_properties` 0/1/plusieurs et `No_ets` | `ABSENT/LOCKED/AVAILABLE`, indices et taux instantanés exacts ; `No_ets` donne ABSENT/zéros |
| `P2-TST-034` | afterburner absent/disponible/lock/actif/demandé, jamais engagé puis arrêté | groupes/flags exacts ; ENGAGEMENT absent avant premier arrêt ; cohérence exigée seulement même sample/keyframe, delta à cadence indépendante accepté |
| `P2-TST-035` | six axes, cinq modes, chaque flag, cruise, compteurs, curseur | mapping bit-à-bit exact, latch VIEW, round-trip et rejet hors borne ; aucun cast des enums/flags moteur |
| `P2-TST-036` | sous-types moteur, chaque `WeaponClassFlags`/`WeaponEffectFlags`, combinaisons homing/swarm, cas `shots>1` avec `swarm_count=-1`, et armes énergie/balistiques/`SecondaryNoAmmo`, burst/substitution/FOF | mapping bit-à-bit et priorités exacts, neutre SWARM `(1,shots)` sans cast de `-1`, `SPECIAL/HOMING/SCRIPTED` jamais émis, présences ammo/rearm exactes, animation absente, timestamps/formules exacts |
| `P2-TST-037` | banques absentes, 64/famille, selectors/previous/laser/swarm/remote/per-burst | listes/groupes globaux exacts, zéro seulement pour absence autorisée, rejet à 65 |
| `P2-TST-038` | tertiaire T=0/1/64/65, sélecteur courant invalide pour T>0, et contre-mesures avec/sans capacity mode/lock/cooldown | T définitions manifeste `TERTIARY` sans `WEAPON_CLASS`, points vides et IDs distincts ; groupe état présent iff T>0, réutilise le bon ID et seulement les six champs autorisés ; sélecteur invalide ou T=65 refusé ; count/capacité, classe et flags CM exacts |
| `P2-TST-039` | 0,1,1024 sous-systèmes ; max hits nul ; pointeur `system_info` absent/étranger/dupliqué ; ensemble/définition modifié ; 1025 | bijection instance↔canonical_index/ID exacte, max nul impose current nul, état toujours upsert complet ; changement impose manifeste+keyframe, jamais CREATE/DELETE ; source ambiguë ou 1025 refusé |
| `P2-TST-040` | mappings subsystem, transform/tourelle/banks/animation ; `turret_num_firing_points=0/1/64/65` | mécanique exacte ; `NEXT_FIRE_POINT` absent à 0, présent avec modulo à 1 et 64, et 65 refuse transactionnellement l’échantillon sans troncature ; NAME_OVERRIDES, ANIMATIONS, turret SWARM/cible/AWACS absents ; IDs turret bank sans collision |
| `P2-TST-041` | cargo caché/révélé, `<range`, `>=dot`, `>required`, capteurs HS, resets et changement cible | phases/flags exacts, LOS géométrique, progression/reset historiques identiques off/on, aucune double avance |
| `P2-TST-042` | docking NONE/APPROACH/DOCKING/DOCKED/UNDOCKING, cycles, leader 0/1/2, 64 relations | priorité/liste inverse exactes ; deux leaders, incohérence inverse ou 65e relation refusés |
| `P2-TST-043` | support toutes phases ; plafonds/taux réparation 0/bornes ; `disallow_rearm`, arme interdite et pools -1/0/positif ; self-repair ; `QUEUE(0)→ONWAY(support)` ; COMPLETE→END même/nouvel épisode ; deux clients ACK différents ; ring/table 64/65 | prédicats REPAIRING/REARMING exacts ; séquence portée par assisted_signature malgré changement de support ; coalescing par episode_sequence ; APPLIED efface seulement le slot rapide ; terminal plus récent va au successeur ; overflow ferme selon contrat |
| `P2-TST-044` | `-0`, NaN, infini, timer négatif/débordant | `-0→+0` bit-exact ; NaN/infini=`NonFinite`; durée négative/hors borne=`OutOfRange`; aucun clamp alternatif |
| `P2-TST-045` | ratios avec dénominateur zéro | « indisponible », aucune division invalide |
| `P2-TST-046` | tous cleanup modes, red-alert, mode inconnu/combiné, membre non joueur | mapping Destroyed/Departed/Vanished exact ; mode invalide ferme la session ; cleanup hors closure ignoré ; événement/fence avant purge |
| `P2-TST-047` | classe density!=1, registres auxiliaires, angles table 0/180/360° donnant les cosinus 1/0/-1, source cosinus hors `[-1,1]`/NaN, contre-mesure capacity | masse/inertie effectives, aucun index+1, FOV wire exacts `{0, π/2, π}` via `acos` sans clamp, source invalide refusée, counts et cooldown ms→µs exacts |
| `P2-TST-048` | toutes formules dashboard, indices ETS avec applicabilité ambiguë, filtre d’offset valide/invalide et frontières stale/converged | inventaire A/C/D 100 %, résultats double exacts dans tolérance, parts ETS toujours indisponibles sans masque wire, stale indisponible sans estimation producteur valide, aucune formule implicite |
| `P2-TST-049` | mémoire shared/client/process aux caps et `+1`, croissance topologique sans catalogue | 64/80/384 Mio acceptés, chaque `+1` refuse avant bind ; zéro allocation/reprovisionnement après Ready |

### 5.5 Snapshot, delta et baseline

| ID | Scénario | Résultat attendu |
|---|---|---|
| `P2-TST-050` | snapshot initial puis keyframes périodique/lifecycle/catalogue/resync | flags exacts `Initial`, `PeriodicKeyframe`, `Resync`, un seul bit ; commit atomique et `APPLIED` |
| `P2-TST-051` | modification, ajout logique d’un membre et retrait logique pendant ACK | candidate immuable ; modification au premier delta, ajout/retrait dans candidate keyframe successeur sans CREATE/DELETE filaire |
| `P2-TST-052` | perte du delta intermédiaire | delta suivant cumulatif suffit |
| `P2-TST-053` | retour exact à baseline | mutation retirée du delta cumulatif |
| `P2-TST-054` | listes shield/banks/docking modifiées ; valeur subsystem modifiée | atome complet remplacé, aucun `PARTIAL`; ensemble subsystem suit TST-039 manifeste+keyframe |
| `P2-TST-055` | ACK perdu, doublon, ancien/futur | retransmission/idempotence/rejet exacts |
| `P2-TST-056` | baseline inconnue ou stale | pas d’application, resync borné |
| `P2-TST-057` | keyframe périodique pendant delta ; delta cumulatif sérialisé >1 048 576 octets puis keyframe >16 777 216 octets | baselines distinctes sans course ; delta abandonné au profit d’une keyframe ; si elle dépasse aussi sa borne, `SourceLimitExceeded` puis `SESSION_END(Restart,RECONNECT_ALLOWED)`, sans troncature |
| `P2-TST-058` | resync répété/client lent ; late join d’un second client avec `manifest_id`, IDs et ACK divergents | DTO/capture pré-ID partagés mais manifeste, image, baseline, candidats et latches distincts par slot ; coalescing, quotas, plafond 384 Mio et aucune troisième candidate/génération |
| `P2-TST-059` | ACK tardifs avec active N, staged N+1, rebuild-intent N+2 | N/N+1 restent cohérents ; paquets tardifs stale/idempotents ; promotion/libération exactes sans troisième génération |

### 5.6 Cycle de vie

| ID | Scénario | Résultat attendu |
|---|---|---|
| `P2-TST-060` | connexion avant/pendant/après chargement | session seulement quand profil matérialisable |
| `P2-TST-061` | apparition joueur | nouvel ID, manifeste, snapshot, event fiable |
| `P2-TST-062` | apparition `ACTIVE`, front disabled tandis que la phase lifecycle reste `ACTIVE`, puis `DYING→DESTROYED→REMOVED→absence` | phases lifecycle et flag/état disabled de `ENTITY_LIFECYCLE` exacts, cinq événements kinds 1/17/18/19/3 dans cet ordre, fences et keyframes exacts |
| `P2-TST-063` | transition moteur sautée | aucun événement exact inventé, état final correct |
| `P2-TST-064` | respawn même classe/signature interne réutilisée | nouvel `entity_id`, baseline neuve |
| `P2-TST-065` | respawn nouvelle classe/loadout | manifeste N+1 avant keyframe |
| `P2-TST-066` | pause/compression/menu/observer | états et sessions conformes, aucune déréférence invalide |
| `P2-TST-067` | sortie mission pendant manifeste/snapshot | drainage et purge sans callback tardif |
| `P2-TST-068` | relance mission et processus | zéro fuite, identité/session conformes |
| `P2-TST-069` | disparition/réentrée d’un support ou docké avec même signature | événement/cascade puis nouvel `entity_id`, jamais réactivation de l’ancien |
| `P2-TST-070` | K=1/2/64 avec omission successive des 10 records par ship et N agrégé 4096/4097 | formule exacte ; chaque omission `InvalidAbsence`; K=65 et N=4097 refusés |

## 6. Goldens et propriétés protocolaires

### 6.1 Nouveaux goldens obligatoires

Le corpus doit ajouter, sans modifier les goldens gelés :

- `phase2-complete-minimal` : joueur sans bouclier, ETS, banques ni support, profil `0x0583` ;
- `phase2-complete-complex` : segments dynamiques, banques mixtes, tourelle, animations et support actif ;
- `phase2-control-state` : tous groupes conditionnels et bornes ;
- `phase2-support-state` : phases et présence support ;
- `phase2-manifest-class-weapon` : transaction atomique multi-part ;
- `phase2-lifecycle-respawn` : événements + snapshots avant/après nouvel ID ;
- une fixture invalide pour chaque domaine ou record obligatoire manquant ;
- une fixture invalide pour chaque contradiction de présence/cohérence Phase 2.

Chaque golden comporte binaire, JSON canonique, version, résultat de validation et hash. Le générateur et le décodeur de contrôle sont indépendants.

### 6.2 Propriétés

Les tests property-based vérifient :

- `decode(encode(x)) = canonicalize(x)` ;
- déterminisme des octets pour tout ordre moteur équivalent ;
- idempotence du commit d’une transaction identique ;
- application de n’importe quel delta cumulatif valide directement à sa baseline ;
- impossibilité de faire croître un ensemble au-delà de sa borne ;
- aucune mutation partielle après erreur ;
- une suppression cascade élimine tous les atomes du sujet ;
- tout ID référencé appartient au bon registre/génération ; aucune exception opaque n’existe, y compris pour support, docking, cargo et tourelles.

## 7. Scénario normatif de perte artificielle

### 7.1 Matrice `P2-LOSS-GATE`

Le harness utilise la seed correcte `1345474380` (`0x50324F4C`) et des streams PRNG séparés nommés par `(profile,rate,mode,direction)`. Pour chacun des profils `CoreGate/0x0401` et `CompleteShip/0x0583`, il exécute un smoke déterministe de 30 secondes pour les taux `1 %`, `5 %`, `20 %`, chacun en mode `iid` puis `burst`, soit douze cellules.

Après qualification de la matrice courte, quatre cas représentatifs sont exécutés pendant 300 secondes après 30 secondes de warm-up :

1. `CoreGate`, 1 %, `iid` ;
2. `CoreGate`, 20 %, `burst` ;
3. `CompleteShip`, 5 %, `iid` ;
4. `CompleteShip`, 20 %, `burst`.

- `iid` : chaque datagramme est perdu par Bernoulli au taux du run ;
- `burst` : chaque bloc directionnel de 100 datagrammes perd un segment contigu de longueur 1, 5 ou 20, dont le départ modulo 100 vient du stream seedé ;
- pour les deux modes : duplication 2 %, jitter uniforme `[0,100] ms`, réordonnement borné à 8 datagrammes et deux blackouts complets de 500 ms placés à 40 % et 75 % de la durée mesurée ;
- les directions producteur→client et client→producteur ont des streams indépendants ; aucune corruption silencieuse, les tests CRC étant séparés.

Le script modifie au moins une valeur de chaque bloc couvert par le profil pendant les perturbations. `CompleteShip` déclenche en plus changement de banque, transition support, changement topologique et respawn avec éventuel manifeste N+1. Les critères vidéo Phase 7 ne s’appliquent pas. Chaque run conserve son rapport et son empreinte ; seul le journal de décisions d’un échec doit être conservé pour rejeu bit-à-bit.

### 7.2 Critères de réussite

Avec le défaut `keyframeSeconds=2` :

- l’origine `t0` est le plus tardif de : fin de perturbation, source+closure stables et `APPLIED` du dernier manifeste requis ; elle est enregistrée avec l’ID correspondant. Après `t0`, la réplique complète converge en `2×keyframeSeconds+1s`, soit 5 secondes au défaut ;
- chaque atome du client égale l’état oracle correspondant ;
- aucun delta dépendant d’un paquet perdu n’est nécessaire à la convergence ;
- aucun manifeste/snapshot partiel n’est exposé ;
- lifecycle et support terminal finissent dans l’état exact ;
- mémoire, files et retransmissions restent sous leurs bornes ;
- zéro crash, deadlock, allocation non bornée ou commande simulation ;
- le rapport contient temps de convergence par bloc, maximum global, paquets injectés et hashes finaux.

Les douze smokes et les quatre runs représentatifs sont bloquants et chacun applique la formule générale `2 × keyframeSeconds + 1 s`, soit 3 à 11 secondes dans le domaine configuré. Un résultat agrégé ne masque jamais un run en échec.

### 7.3 Qualification et arrêt

Avant la matrice, le testeur qualifie l’oracle sur deux scénarios courts, dont `core-gate-20-burst`, et démontre qu’un rapport volontairement incomplet est refusé. Un run rouge s’arrête, reste `failed`, place ses consommateurs en `deferred-by-dependency` et programme le reproducer ciblé comme prochaine opération de son cône. Les scénarios et preuves indépendantes continuent. Si l’oracle loss, le harness, le seed, l’environnement qualifié ou le fingerprint partagé est invalide, tous leurs consommateurs s’arrêtent jusqu’à restauration de cette racine de confiance.

## 8. Fuzzing et corpus hostile

Le reader et les validateurs sont fuzzés sur :

- en-têtes, longueurs, offsets, counts, CRC, SHA et part indices ;
- records 3–14 et 20–22, presence bits, enums et floats ;
- UTF-8, longueurs de chaînes et listes imbriquées ;
- cardinalités de boucliers, banques, sous-systèmes, animations, tourelles et docking ;
- références cross-manifest, IDs nuls/dupliqués/stale ;
- matrices `0x0400`, `0x0401`, `0x0583` et bits réservés ;
- ACK/NACK/resync, duplication et désordre ;
- séquences d’apparition/suppression/respawn et manifeste changé.

Les deux exécutables obligatoires sont `telemetry_phase2_packet_reader_fuzz` (datagramme, fragments, transactions, ACK) et `telemetry_phase2_state_validator_fuzz` (records 3–14/20–22/28, manifestes, matrice et mutations). Ils sont construits sur le job CI Linux Clang avec `-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer`; les corpus initiaux vivent dans `test/telemetry/producer/phase2/fuzz/corpus/{packet_reader,state_validator}` et les artefacts dans `test/telemetry/producer/phase2/reports/wp09-security/fuzz-artifacts/`.

Chaque target est exécutée avec `-max_total_time=60 -seed=1345474380 -timeout=5 -rss_limit_mb=384`; le runner archive corpus avant/après, commande, sanitizer, couverture, crashers et SHA-256. Les campagnes longues restent recommandées hors gate. Critère : code retour zéro, zéro crasher/timeout/OOB/UAF/UB, zéro allocation au-delà des quotas et aucun état partiellement committé. Le corpus hostile déterministe est en outre rejoué par `ctest` sans fuzzer afin que MSVC Debug/Release couvre les mêmes entrées.

## 9. Sécurité opérationnelle

### 9.1 Menaces couvertes

| Menace | Contrôle |
|---|---|
| écoute involontaire | module off, bind loopback, discovery off |
| source non autorisée | allowlist avant allocation/réponse |
| amplification | budget 3× avant session validée |
| fragmentation hostile | 1200 octets, quotas, timeouts, hash/CRC |
| manifeste géant | 16 Mio, 64 parts, 2 candidates et validation avant commit |
| CPU flood | rate limits, budget datagrammes et raisons agrégées |
| fuite d’information | moindre point fixe support/docking/leader, filtre Cockpit avant diff, logs sans contenu |
| commande simulation | aucun `MessageType`/handler de commande, test négatif |
| IDs réutilisés | session imprévisible et registres monotones |
| pair lent/silencieux | timeout, files bornées et fermeture |

L’usage Internet, l’authentification, le chiffrement, la gestion de clés et un service multi-utilisateur sont hors périmètre. Une allowlist n’est pas une authentification.

### 9.2 Tests de confidentialité

Les fixtures incluent des classes/armes/cargos non référencés et des cibles cachées. Le flux, les manifestes, logs et métriques ne doivent contenir aucun de leurs noms, IDs ou champs. La cible d’une tourelle et AWACS restent absents. Un scan `HIDDEN` ne contient pas `cargo_text`.

Les tests recherchent aussi pointeurs plausibles, `objnum`, `instance`, index négatifs castés, adresses IP, chemins absolus, callsigns et payloads dans les logs.

## 10. Performance, endurance et ressources

### 10.1 Performance

Les scénarios et seuils de [05-integration-configuration-et-observabilite.md](05-integration-configuration-et-observabilite.md) sont exécutés en Release. La preuve utilise les quatre runtimes persistants et les dix époques de 60 s dans l’ordre rotatif normatif : 600 s de contrôle et trois mesures actives de 600 s, soit 1 800 s actives agrégées et 2 400 s physiques mesurées. Les quatre warm-ups de 60 s sont exclus des percentiles et médianes, mais pas des oracles d’allocation/croissance après `Ready`. L’union brute active doit satisfaire les minima de 100 000 frames, 54 000 ticks flight, 18 000 ticks systems et 900 keyframes. Le rapport inclut tous les échantillons, l’ordre des tranches et les high-water. Un seul dépassement de max keyframe, une allocation après `Ready`, un ordre ou runtime non prouvé, ou un p99 au-dessus du seuil laisse la gate ouverte.

### 10.2 Endurance

Le soak final est un scénario composite unique de 3600 secondes avec la seed 4242. Il combine une session nominale avec changements continus de tous les blocs, trois sorties/entrées mission et respawns, trois arrêts/restarts du client, des segments `P2-LOSS-GATE` avec resync et quatre clients dont un lent. Les transitions sont planifiées de façon déterministe et consignées dans le rapport.

Critères : zéro fuite détectée, zéro croissance steady-state, zéro deadlock, sockets/slots/gauges à zéro après purge, IDs non réutilisés, état final exact et shutdown borné. Les rapports Phase 1 ne sont pas réutilisés comme preuve Phase 2, mais servent de baseline.

## 11. Matrice exigences-vers-preuves

| Groupe d’exigences | Preuves minimales |
|---|---|
| `P2-REQ-001..008` | build, freeze, négociation, profils positifs/négatifs, exclusions |
| `P2-REQ-009..014` | tests thread/gardes/ownership, allocation et cadence/keyframe |
| `P2-REQ-015..020` | manifestes, closure, IDs, changement génération, confidentialité |
| `P2-REQ-021..033` | matrice métier, goldens, oracle et propriétés |
| `P2-REQ-034..040` | lifecycle, events, snapshot/delta et `P2-LOSS-GATE` |
| `P2-REQ-041..046` | config, quotas, métriques, logs et performance |
| `P2-REQ-047..050` | client indépendant, inventaire de provenance, suites complètes, CMake et fail-closed |

La matrice détaillée par exigence et lot est dans [07-livraison-et-tracabilite.md](07-livraison-et-tracabilite.md).

## 12. Gate de conformité

La gate Phase 2 est `PASS` seulement si :

1. toutes les cibles de test obligatoires passent sur la matrice build ;
2. le freeze FSTL 1.0 et les goldens Phase 1 passent ;
3. les profils `0x0401` et `0x0583` passent leurs cas positifs et négatifs ;
4. l’oracle couvre 100 % des champs affichés avec zéro mismatch ;
5. `P2-LOSS-GATE` converge dans la borne ;
6. lifecycle, support terminal, changement manifeste et resync sont exacts ;
7. fuzz/security produisent zéro défaut et zéro fuite ;
8. performance, allocations et soak respectent leurs critères ;
9. tous les rapports sont liés à la révision testée et reproductibles ;
10. la checklist du document 07 est appuyée par ces preuves.

Toute preuve absente, obsolète, exécutée sur une autre révision ou non reproductible laisse la gate ouverte. Ce document couvre `P2-REQ-001` à `P2-REQ-050` du point de vue validation et sécurité.
