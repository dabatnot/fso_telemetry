# Contrat CAN AV CORE v1

Le bus utilise exclusivement CAN 2.0 standard, des identifiants 11 bits, des
trames de huit octets et un débit de 1 Mbit/s. Tous les octets réservés valent
zéro et tous les entiers multi-octets sont little-endian. L'octet 0 des trames
fonctionnelles vaut `1`.

| ID | Trame | Producteur | Consommateur |
|---:|---|---|---|
| `0x180` | `WARNING_STATE` | AV CORE | WARN CTRL |
| `0x181` | `CAUTION_STATE` | AV CORE | WARN CTRL |
| `0x182` | `LIGHTING_COMMAND` | AV CORE | WARN CTRL, THREAT PROC |
| `0x183` | `LIGHTING_STATE` | WARN CTRL | autres nœuds |
| `0x184` | `THREAT_STATE` | AV CORE | THREAT PROC |
| `0x700`–`0x703` | `NODE_STATUS` | rôle correspondant | AV CORE |

`WARNING_STATE` transporte dans l'octet 1 les bits `FIRE`, `MISSILE`, `BLAST`,
`COLLISION` et `EMP`. `CAUTION_STATE` transporte dans les octets 1–2 les neuf
cautions cockpit, puis dans les octets 3–4 sept diagnostics codés sur deux bits :
`0=clair`, `1=dégradé fixe`, `2=absent/périmé clignotant`.

| Trame | Octets utilisés |
|---|---|
| `WARNING_STATE` | `0=version`, `1=warning_mask` |
| `CAUTION_STATE` | `0=version`, `1..2=cockpit_mask:u16`, `3..4=diagnostic_mask:u16` |
| `THREAT_STATE` | `0=version`, `1=sector_mask`, `2=lock_state` |
| `LIGHTING_STATE` | `0=version`, `1=brightness_percent`, `2=test_target`, `3=lamp_id/0xFF`, `4=physical_test` |
| `NODE_STATUS` | `0=version`, `1=health`, `2..4=firmware`, `5..7=uid24` |

`LIGHTING_COMMAND` est une trame discriminée par l'octet 1 : couleur warning,
couleur caution, limites/cadences ou impulsion de test. Les tests portent sur
`ALL`, un calculateur ou l'un des 32 identifiants de voyant et durent 2 000 ms.
AV CORE répète la configuration chaque seconde ; les tests restent des commandes
ponctuelles. `WARN CTRL` est l'unique producteur de `LIGHTING_STATE` et y publie
la luminosité effective, la cible de test et l'état du poussoir physique.
`THREAT PROC` reçoit la couleur warning et les cadences depuis
`LIGHTING_COMMAND`, puis la luminosité et les tests effectifs depuis
`LIGHTING_STATE`.

Les opcodes `LIGHTING_COMMAND` sont `1=warning RGB` (`2..4=RGB`),
`2=caution RGB`, `3=limites` (`2=max %`, `3..4=slow_cHz`, `5..6=fast_cHz`) et
`4=test` (`2=target`, `3=lamp_id/0xFF`, `4..5=duration_ms`). Les cibles valent
`0=ALL`, `1=WARN_CTRL`, `2=LAMP` et `3=THREAT_PROC`. Dans `LIGHTING_STATE`, `0xFF` indique
qu'aucun test n'est actif.

Les identifiants de voyant `0..22` appartiennent à `WARN CTRL`. Les identifiants
`23..31` appartiennent à `THREAT PROC`, dans l'ordre avant, avant-droite,
droite, arrière-droite, arrière, arrière-gauche, gauche, avant-gauche et
`LOCK`. Un test destiné à l'autre module est relayé sans allumer les voyants de
`WARN CTRL`.

Dans `THREAT_STATE`, les bits `0..7` suivent ce même ordre horaire à partir de
l'avant. `lock_state` vaut `0=NONE`, `1=ATTEMPT` ou `2=ACQUIRED`; les octets
`3..7` restent nuls. AV CORE publie une trame vide hors état télémétrique
`LIVE`.

Les heartbeats utilisent respectivement `0x700 WARN_CTRL`, `0x701 THREAT_PROC`,
`0x702 SENS_PROC` et `0x703 INST_PROC`. Ils contiennent l'état, la version du
firmware et les 24 bits bas de l'identité matérielle. Ils sont publiés une fois
par seconde ; AV CORE déclare un rôle installé absent après le délai configuré.

Une trame périodique invalide est ignorée et comptée. Une perte de
`WARNING_STATE` ou `CAUTION_STATE` pendant 500 ms efface les indications
distantes sur `WARN CTRL` et fait clignoter `AV BUS`. Une perte de
`THREAT_STATE` pendant 500 ms efface l'indicateur de menace ; une perte de
`LIGHTING_STATE` éteint `THREAT PROC` et dégrade son heartbeat.
