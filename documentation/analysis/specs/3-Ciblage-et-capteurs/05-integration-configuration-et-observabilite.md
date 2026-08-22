# 05 — Intégration, configuration et observabilité

## 1. Objet

Ce document fixe le profil producteur Phase 3, ses ressources, ses métriques
et ses logs. Les règles réseau, allowlist, dual-stack, socket non bloquant et
anti-amplification restent celles des phases précédentes.

## 2. Configuration

### 2.1 Version 4

Une configuration `schemaVersion=4` ne contient aucune clé de profil. Un
producteur activé et éligible utilise toujours :

| Profil | Couverture |
|---|---:|
| `CockpitSensors` | `0x07CB` |

Les clés historiques `profile` et `phase2Profile` sont inconnues. Les versions
1 à 3 sont refusées avant bind ; il n’existe ni migration automatique ni
fallback vers une couverture plus petite.

### 2.2 Cadences

La Phase 3 n’ajoute aucune fréquence :

- cible, locks et voyants HUD suivent `flightHz`, `1..60`, défaut 30 ;
- radar, contacts, menace, cargo et navigation suivent `systemsHz`, `1..20`,
  défaut 10 ;
- les deux schedulers restent indépendants ;
- une keyframe ignore l’échéancier et capture tous les blocs.

### 2.3 Mode

`CockpitSensors` est éligible uniquement si :

- autorité `SOLO` ;
- visibilité `COCKPIT` ;
- joueur local observable ;
- projection capteurs complète et bornée ;
- catalogues requis constructibles ;
- budget mémoire accepté.

Un refus d’éligibilité empêche le démarrage du producteur avant bind. Il
n’existe aucun fallback vers un profil historique dans le même process ou le
même handshake.

## 3. Bornes produit

| Ressource | Plafond Phase 3 |
|---|---:|
| contacts simultanés | 4096 |
| identités capteurs allouées par session | 65 536 |
| locks | 64 |
| missiles entrants | 256 |
| navpoints | 1024 |
| waypoints de route | 2048 |
| taille d’un record | 65 535 octets |
| delta logique | 1 048 576 octets |
| transaction snapshot/manifeste | 16 777 216 octets |
| parts d’une transaction | 64 |
| clients | 4 |

Les limites FSTL plus strictes prévalent. Une source dépassant une liste
atomique non paginable n’est jamais tronquée.

## 4. Budget mémoire

### 4.1 Plafonds

La Phase 3 fixe :

| Scope | Plafond |
|---|---:|
| partagé process/mission | 167 772 160 octets (160 Mio) |
| par client | 92 274 688 octets (88 Mio) |
| process total | 536 870 912 octets (512 Mio) |

Avec `maxClients <= 4`, le calcul vérifié avant bind est :

```text
shared + maxClients * perClient <= 536870912
```

Ces plafonds incluent toutes les allocations héritées. Ils ne sont ni une
estimation RSS, ni un objectif moyen.

### 4.2 Préallocation

Avant `Ready`, le runtime provisionne :

- projection et scratch pour 4096 contacts ;
- registre de 65 536 identités ;
- listes maximales de locks, missiles, navpoints et waypoints ;
- deux catalogues sémantiques au plus, actif et staged ;
- image, baseline, candidate, delta et rétention fiable par client ;
- index et tables de diff ;
- métriques et logs bornés.

Chaque capacité publie ses octets possédés. Tout dépassement du budget produit
`StartupBudgetExceeded`, zéro bind et zéro état `Ready`.

Après `Ready`, aucune croissance de vector, string, map, arène ou heap fallback
n’est autorisée. Une limite rencontrée ferme le profil avec une cause observable
et conserve les ressources bornées.

## 5. Travail et réseau

- aucune attente réseau sur la frame ;
- aucun DNS ou allocation socket par tick ;
- datagrammes limités à 1200 octets ;
- sérialisation et diff dans des buffers préalloués ;
- keyframe utilisée si le delta dépasse 1 Mio ;
- trafic d’état prioritaire sur toute fonction future ;
- aucune mesure de performance longue exigée pour la livraison.

Le module désactivé conserve zéro socket, zéro allocation Phase 3 persistante et
zéro log récurrent.

## 6. Métriques

### 6.1 Profil et capture

| Nom | Type | Scope | Sémantique |
|---|---|---|---|
| `telemetry_phase3_profile` | gauge enum | session | `None`, `CockpitSensors` |
| `telemetry_phase3_profile_rejections_total{reason}` | counter | process | refus primaire fermé |
| `telemetry_phase3_capture_duration_us{block}` | histogram | mission/process | durée de capture |
| `telemetry_phase3_capture_failures_total{block,reason}` | counter | mission/process | échec primaire |
| `telemetry_phase3_sample_age_us{block}` | gauge | session | âge du dernier bloc installé |

`block={Targeting,Locks,HudAlerts,Radar,Contacts,Threat,Cargo,Navigation}`.

### 6.2 Visibilité et contacts

| Nom | Type | Scope | Sémantique |
|---|---|---|---|
| `telemetry_phase3_contacts{visibility}` | gauge | session | pistes courantes par visibilité |
| `telemetry_phase3_contact_changes_total{kind}` | counter | session/process | create, replace, delete |
| `telemetry_phase3_visibility_filtered_total{reason}` | counter | mission/process | objet ou champ retiré avant exposition |
| `telemetry_phase3_public_ids` / `_high_water` | gauge | session | IDs alloués, max 65 536 |
| `telemetry_phase3_manifest_expansions_total{reason}` | counter | session/process | nouvelle classe révélée |
| `telemetry_phase3_incoming_missiles` | gauge | session | taille de liste courante |
| `telemetry_phase3_navpoints` / `_route_waypoints` | gauge | session | cardinalités courantes |

Enums fermés :

- `visibility={NotVisible,Visible,Distorted}` ;
- `kind={Create,Replace,Delete}` ;
- `reason={UnknownObject,SensorHidden,IdentityHidden,ClassHidden,IffHidden,CargoHidden,NavigationHidden,InvalidReference}` ;
- expansion `{ShipClass,WeaponClass,RadarIcon}`.

### 6.3 Réplication et ressources

| Nom | Type | Scope | Sémantique |
|---|---|---|---|
| `telemetry_phase3_image_records` | gauge | session | atomes courants |
| `telemetry_phase3_image_bytes` | gauge | session | taille logique courante |
| `telemetry_phase3_dirty_atoms` | gauge | session | changements cumulatifs |
| `telemetry_phase3_forced_keyframes_total{reason}` | counter | session/process | cause primaire |
| `telemetry_phase3_source_limit_total{kind}` | counter | mission/process | limite dépassée |
| `telemetry_phase3_allocations_after_ready_total{kind}` | counter | process | croissance interdite |
| `telemetry_phase3_memory_bytes{scope}` / `_high_water` | gauge | process | octets possédés |

Les gauges reviennent à zéro à la purge de session. Les high-water ne diminuent
qu’à la réinitialisation de leur scope.

## 7. Logs

| Événement | Niveau | Fréquence maximale | Contenu permis |
|---|---|---:|---|
| profil sélectionné/refusé | info/warning | une fois/session | profil, masque, raison fermée |
| bilan projection | debug | une fois/s | comptes agrégés par visibilité |
| expansion manifeste | info | une fois/génération | ID, classes/armes comptées, octets |
| limite source | warning | une fois puis résumé | catégorie, limite, compte |
| perte de couverture | warning | une fois/session | domaine et raison fermée |
| fermeture Phase 3 | error | une fois/fermeture | bloc, statut et compteur fermés |
| bilan Phase 3 | info | fin session | high-water, captures, contacts, drops, resync |

Les erreurs terminales sont recopiées dans un miroir borné indépendant de la
file de livraison ordinaire. Le bloc et le statut de la fermeture Phase 3
restent donc inspectables après saturation de cette file et après la purge du
runtime natif. Le chemin de sélection du manifeste est couvert par le bloc
fermé `precondition` ; aucune identité, classe ou donnée gameplay n’est ajoutée
au diagnostic.

Les logs ne contiennent jamais :

- nom de cible, callsign, classe ou texte cargo ;
- nom ou position de navpoint ;
- position/vitesse de contact ou payload ;
- entity ID, signature moteur, pointeur ou index ;
- IP complète, chemin absolu ou dump de datagramme ;
- liste par frame.

## 8. Matrice configuration/résultat

| Cas | Résultat |
|---|---|
| v1, v2 ou v3 | invalide, zéro bind |
| v4 avec `profile` ou `phase2Profile` | invalide, zéro bind |
| v4 éligible | `CockpitSensors`, `0x07CB` |
| profil source incomplet | session refusée, aucune couverture partielle |
| limite atteinte après démarrage | `SESSION_END(Restart,RECONNECT_ALLOWED)` |
| budget exact | accepté |
| budget exact + 1 | refusé avant bind |

## 9. Traçabilité

Ce document couvre `P3-REQ-012`, `P3-REQ-040` à `P3-REQ-044`.
