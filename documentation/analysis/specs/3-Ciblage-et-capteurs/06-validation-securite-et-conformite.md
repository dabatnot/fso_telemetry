# 06 — Validation, sécurité et conformité

## 1. Objet

Ce document définit les preuves courtes minimales capables d’observer le produit
Phase 3. Il ne crée ni gate intermédiaire, ni score, ni campagne autonome.

## 2. Hiérarchie de preuve

Les preuves utilisent, dans cet ordre :

1. schémas wire et golden vectors pour les octets ;
2. invariants actifs et contrat Phase 3 ;
3. vrai chemin moteur → projection filtrée → réplication ;
4. décodeur indépendant ;
5. observations Release contrôlées par l’utilisateur.

La présence d’une classe, d’un fichier ou d’un nom de fonction ne prouve aucun
comportement produit.

## 3. Compatibilité wire

Les contrôles courts vérifient :

- gel byte-identical FSTL 1.0 et 1.1 ;
- conservation byte-identique des layouts historiques et sélection explicite
  des nouveaux champs/bits uniquement par les records v4 ;
- masque exact `0x07CB` ;
- matrice complète des records ;
- acceptation de tous les vecteurs existants ;
- rejet des longueurs, bits réservés, enums et références invalides.

Le décodeur utilisé pour la comparaison n’appelle ni sérialiseur ni validateur
du producteur.

## 4. Projection cockpit

### 4.1 Oracle produit

Pour une capture déterministe, la preuve rapproche :

- décision radar/HUD autorisée du moteur ;
- DTO public après filtrage ;
- atomes FSTL décodés indépendamment.

Le résultat exact compare :

- ensemble des IDs de pistes autorisées ;
- visibilité de chaque piste ;
- présence/absence de nom, classe, équipe et IFF ;
- cible et flag `CURRENT_TARGET` lorsqu'ils partagent le même sample time ;
- cardinalités de locks et missiles ;
- phases cargo et navigation.

Une liste synthétique injectée directement dans le producteur n’est pas une
preuve acceptable.

### 4.2 Cas de confidentialité

Les cas courts couvrent :

- contact visible ;
- contact distordu ;
- objet furtif jamais détecté ;
- piste précédemment connue puis oubliée ;
- classe et IFF non révélés ;
- cargo scanné mais caché ;
- navpoint interdit ;
- lien navigation vers entité cachée.

Dans chaque cas, l’absence attendue est vérifiée sur les octets et sur l’état
décodé, pas seulement sur l’interface graphique.

## 5. Exactitude métier

Les tests ciblés vérifient :

- cible nulle, changement de cible et cible précédente ;
- `TARGET_STATE` v2 : distance D et vitesse S visibles, multiplicateurs HUD,
  tendances `+`/`-` et fallback de vaisseau docké ;
- `TARGET_STATE` v5 : libellés HUD exacts des sous-systèmes ciblé et lock,
  y compris lorsque la classe apparue dynamiquement n'est pas installée dans le manifeste ;
- `TARGET_STATE` v6 : force HUD d'une cible vaisseau intacte, endommagée,
  sans bouclier ou au bouclier épuisé, absence pour une cible non-vaisseau et
  rejet des ratios non finis ou hors borne ;
- groupe de lead all-or-nothing et banque résolue ;
- lock absent, tentative, acquis et sous-système ;
- capteurs offline/degraded/online ;
- AWACS, EMP et état de piste `VISIBLE`/`DISTORTED` ;
- contact create/replace/delete ;
- identité HUD exacte d'un vaisseau visible, nom masqué, type alternatif et
  absence d'identité pour une piste distordue ;
- couleur radar exacte pour ami, ennemi, neutre, override observateur,
  accessibilité, warp, tagged, navbuoy/cargo, bombe/LSSM et jump node ;
- cible courante brillante conservant sa teinte et cible hors radar recevant
  sa couleur HUD autoritaire ;
- projection radar v2/v3 devant, droite, haut, gauche/bas et presque derrière ;
- orientation d'œil distincte de l'orientation brute et changement de pose
  entre deux ticks sans mélange avec `FLIGHT_STATE` ;
- contact hors portée absent du collecteur ;
- menace none/dumbfire/attempt/acquired ;
- voyants primaire et lock simultanés, cadences dérivées 180/180/90 ms ;
- avertissements Launch, Evaded, Collision, Blast, Engine Wash, EMP et Other,
  priorités natives, redéclenchement, expiration et pause ;
- liste de 0, 1, 256 et 257 missiles ;
- scan idle/scanning/completed, hidden/revealed ;
- navpoints autorisés, destination, route et refus d’autopilote ;
- cohérence des IDs entre tous les records.

Chaque test compare un résultat exact : valeur, ID, masque, cardinalité, cause
ou transition.

## 6. Réplication et récupération

Une intégration loopback courte :

1. installe un manifeste et un snapshot `0x07CB` ;
2. modifie une cible, une piste ou un scan ;
3. abandonne exactement un delta ;
4. reçoit un delta cumulatif ultérieur ou une keyframe ;
5. vérifie l’état final et l’absence de référence pendante.

La perturbation est déterministe et unique. Aucun soak, taux de perte
probabiliste ou scénario de plus de cinq minutes n’est requis.

## 7. Bornes et erreurs

Les maxima et `max+1` couvrent :

- 4096/4097 contacts ;
- 65 536/65 537 IDs ;
- 64/65 locks ;
- 256/257 missiles ;
- 1024/1025 navpoints ;
- 2048/2049 waypoints ;
- record 65 535/65 536 octets ;
- delta 1 Mio/+1 ;
- transaction 16 Mio/+1 ;
- budget mémoire exact/+1.

Sont également rejetés avant cast, allocation ou publication :

- NaN, infini et `-0` non canonisé ;
- quaternion invalide ;
- ID zéro interdit ou réutilisé ;
- classe absente du manifeste ;
- doublon de clé ;
- temps décroissant ;
- enum ou bit inconnu ;
- cible et flag contact incohérents ;
- texte cargo sous `HIDDEN`.

Le produit conserve la dernière baseline cohérente ou termine la session selon
la cause. Il ne publie jamais une couverture partielle sous `0x07CB`.

Une cible et un contact de sample times différents sont validés séparément et
leur décalage n'est pas une incohérence. Toute fermeture Phase 3 restante
incrémente un compteur et consigne, avant `PermanentCaptureFailure`, le bloc
fermé (`precondition`, `target-locks`, `radar`, `threat`, `cargo`, `navigation`
ou `state-image`) et son statut fermé exact, sans texte libre ni donnée gameplay.
Ce diagnostic terminal survit à la saturation de la file de logs ordinaire et
à la purge du runtime. Une arme entrante dont la classe dynamique n’est pas
installée est au contraire une omission transitoire normale : elle ne publie
aucune référence de classe et ne ferme pas la session.

## 8. Sécurité réseau et lecture seule

Les contrôles hérités confirment :

- validation avant allocation ;
- endpoint et session validés avant réponse amplifiante ;
- socket non bloquant ;
- messages de commande gameplay absents ;
- aucune mutation de cible, navigation, scan, arme ou mission ;
- logs sans contenu sensible ;
- purge complète à la fin de session.

Un paquet valide peut demander session, ACK, NACK ou resync selon FSTL. Il ne
peut choisir une cible, engager l’autopilote ou déclencher un scan.

## 9. Ressources et impact en jeu

Les preuves déterministes vérifient :

- préallocation avant `Ready` ;
- budget 160 Mio partagé, 88 Mio/client et 512 Mio total ;
- zéro croissance après warm-up ;
- bornes de chaque collection ;
- aucune attente réseau dans la frame ;
- module désactivé inerte.

L’observation Release consigne si le jeu reste normal pendant les transitions
visées. Aucun percentile, seuil matériel universel ou microbenchmark ne décide
automatiquement de la livraison.

## 10. Observations Release

Trois observations courtes, d’une durée combinée inférieure à cinq minutes,
sont prévues :

1. radar et confidentialité ;
2. cible, locks, menace et cargo ;
3. navigation et autopilote.

Chaque relevé contient `attendu`, `observé`, `écart` et `impact`. L’utilisateur
contrôle le jeu et décide de l’acceptation.

## 11. Critères de conformité

| Critère | Résultat attendu |
|---|---|
| profil | FSTL 1.1, masque `0x07CB` |
| wire | artefacts antérieurs byte-identiques |
| snapshot | exactement `10 + 10K + N + C` atomes |
| contacts | ensemble identique à la projection cockpit |
| confidentialité | zéro champ ou objet non autorisé |
| références | zéro ID incohérent ou classe non installée |
| récupération | convergence après une perte contrôlée |
| ressources | toutes les bornes respectées sans croissance |
| gameplay | aucune commande distante et fonctionnement normal |

## 12. Traçabilité

Ce document couvre l’ensemble des exigences, avec un accent sur
`P3-REQ-002`, `P3-REQ-004`, `P3-REQ-014`, `P3-REQ-019` à
`P3-REQ-046` et `P3-REQ-048`.
