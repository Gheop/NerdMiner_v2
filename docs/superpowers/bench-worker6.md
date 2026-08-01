# Bench worker6 — race/gheop8

## Résultat final : 297,4 → **304,2 kH/s (+2,3 %)**, mismatch = 0

| optimisation | verdict | delta |
|---|---|---|
| **Mode headless** (T7) | **gardée** | **+4,0 %** (A/B strict) |
| Audit des ops moteur (T4) | nulle | 0 % — `sha_ll_load` est un inline vide sur S3 |
| Taille des jobs 16k→64k | nulle | +0,13 % (bruit), revertée |
| SIMD/PIE 4-way (T8-T10) | **NO-GO** | `ee.vadds.s32` sature, SHA-256 exige mod 2^32 |
| Interleave SW/HW (T5) | **rejetée** | −11,7 % au meilleur grain |
| Duel DMA (T6) | annulée | T5 prouve que le CPU n'est pas le goulot |

Hors perf mais acquis au passage : le POST de télémétrie vit désormais dans sa propre tâche
(un `http.POST` bloqué ne peut plus geler le watchdog anti-freeze) — **utile à toute la flotte**.

Ce que le profil apprend sur le plafond : sur 915 cyc/nonce, ~550 sont des accès registres APB
(~17 cyc chacun) et ~340 de l'attente moteur non récupérable. Le silicium, pas le code, fixe la
limite. Les gains restants seraient matériels (fréquence APB, autre puce), pas logiciels.

Protocole : télémétrie (fenêtres 60 s) ou serial `Bench:`, worker6 sur jobs réels
public-pool. Gate : gain chiffré ET `shaMismatch == 0`, sinon revert.

| date | commit fw | env | khs_hw | khs_sw | total | note |
|------|-----------|-----|--------|--------|-------|------|
| 2026-08-01 | df35dbc | Race | 259.4 | 38.0 | 297.3 | **baseline** (algo stock, split mesuré sur fenêtre 60 s propre, mismatch=0) |

Cycles/nonce chemin HW à 240 MHz : 240e6 / 259.4e3 ≈ **925 cyc/nonce**.

## Profil de phases (7bbfbc0 + instrumentation, 2026-08-01)

Mesuré sur 5 fenêtres de ~4,19 M nonces, très stable (±3 cyc) :

| phase | cyc/nonce | % | contenu |
|-------|-----------|---|---------|
| `inter` | **304** | 33 % | transfert H→TEXT : 8 `DPORT_SEQUENCE_REG_READ` + 10 `REG_WRITE` |
| `w1`+`w2` | **341** | 37 % | **attente pure du moteur** (178 + 163) |
| `fill` | 171 | 19 % | 10 `REG_WRITE` du bloc nonce |
| `mid` | 75 | 8 % | 8 `REG_WRITE` du midstate |
| `chk` | 24 | 3 % | `read_digest_if` (early-exit 16 bits) |
| **total** | **918** | | (hashrate concomitant 268 KH/s) |

Conséquences directes :
- **T5 (interleave) = gisement n°1** : 341 cyc/nonce d'attente pure, soit 37 % du temps CPU
  jetés à poller. Le seuil d'abandon de la spec était 60 cyc → très largement dépassé.
- `inter` (304) est le write le plus cher : ~17 cyc par accès registre, contre ~9 pour `mid`
  (SHA_H_BASE). Piste T4 : les `DPORT_SEQUENCE_REG_READ` sont-ils nécessaires sur S3 (le
  contournement matériel DPORT est un problème de l'ESP32 classic) ?

### Piège de mesure (à ne pas refaire)
Première instrumentation avec `xthal_get_ccount()` nu → **16 400 cyc/nonce** rapportés, soit 17×
le temps réel écoulé (physiquement impossible). Cause : GCC réordonne les lectures de ccount
(aucun effet de bord déclaré) → delta négatif → ~2^32 en non signé. Fix : `RACE_CC()` avec
`__asm__ __volatile__("rsr.ccount") ::: "memory"` (barrière compilateur complète) + rejet des
échantillons > 10 000 cyc (préemption par Monitor, prio 5 > mineur prio 3 ; ~550 sur 4,19 M).

## Incident noté (2026-08-01, hors perf)
Télémétrie figée à uptime 357 s (mining intact à 297 KH/s) : la tâche `healthWatchdog`
(Health + POST + protection anti-gel) s'est tue, très probablement `http.POST` bloqué
pendant le rollout du dashboard (timing exact). Gel non reproduit en conditions stables.
Durcissement prévu : sortir le POST réseau de la tâche watchdog (tâche télémétrie dédiée).

## T4 — audit des opérations moteur : **aucun gain** (verdict négatif, 2026-08-01)

Mesure après nettoyage : `tot=918 mid=75 fill=171 w1=178 inter=304 w2=163 chk=24`,
hashrate 268,6 KH/s, mismatch=0 → **strictement identique** à avant. Ce que le HAL dit :

- `sha_ll_load()` est un **inline vide** sur S3 (`hal/esp32s3/include/hal/sha_ll.h:77`).
  Les 2 appels par nonce étaient du bruit déjà effacé par le compilateur. Retirés (lisibilité).
- `DPORT_SEQUENCE_REG_READ` = simple lecture volatile sur S3
  (`soc/esp32s3/include/soc/dport_access.h:35`) ; le contournement séquencé est propre à l'ESP32 classic.
- `DPORT_INTERRUPT_DISABLE/RESTORE` = **macros vides** sur S3 (la variante `XTOS_SET_INTLEVEL`
  est sous `soc/esp32/`). Donc pas de coupure d'interruptions par nonce → **aucun lien avec l'int_wdt**.
- `__builtin_expect` sur l'early-exit : 0 cycle gagné (prédicteur déjà correct).

Conclusion : les 550 cyc/nonce de `mid`+`fill`+`inter` sont **18 accès registres APB à ~17 cyc**,
incompressibles côté logiciel. Le seul contournement possible serait le DMA (T6).
Le gisement restant est donc l'attente moteur (341 cyc, T5).

## T7 — mode headless : **GARDÉ, +4,0 %** (2026-08-01)

A/B strict (même build, même instrumentation, seul `RACE_HEADLESS` change) :

| | cyc/nonce | khs_hw | khs_sw | total |
|---|---|---|---|---|
| headless OFF | 901 | 234,5 | 38,0 | **272,5** |
| headless ON | 885 | 241,4 | 41,9 | **283,3** |

**+10,8 kH/s (+4,0 %)**, mismatch=0. Le rendu TFT/SPI ne volait pas que du CPU : `inter`
tombe de 288 à 281 cyc et `fill` de 171 à 165 → moins de contention sur le bus périphérique.

### Mesure de production (RACE_BENCH=0, RACE_RATE=1, headless=1)
`khs_hw=262,3  khs_sw=41,9  **total=304,2 kH/s**`, mismatch=0, stable à ±0,3 sur 7 fenêtres.

### Piège de mesure n°2 : l'instrumentation fausse le gate
Le profilage par phase (7 lectures `ccount` + accumulations par nonce) coûte **~10 % du chemin
HW** (259 → 234 khs_hw). Comparer un build instrumenté à une baseline non instrumentée donne un
faux négatif. D'où la séparation :
- `RACE_RATE=1` : 1 printf / 256 jobs, coût nul → **sert aux gates de perf**
- `RACE_BENCH=1` : profil par phase, coûteux → **profils seulement, jamais un gate**

### Note réseau (hors perf)
`Report -> HTTP -1` intermittent (1 POST sur 2) à -70 dBm : échec de connexion TCP, pas un
crash. La sonde `Rate:` série rend les mesures indépendantes du réseau.

## T8 — pari SIMD/PIE : **NO-GO définitif** (2026-08-01, ~30 min)

L'ISA PIE (128 bits, 4 lanes de 32) est bien reconnue par l'assembleur du toolchain, et
l'essentiel est disponible : `ee.xorq/andq/orq/notq`, `ee.vsl.32`/`ee.vsr.32` (shift par SAR),
`ee.vld.128.ip`/`ee.vst.128.ip`, `ee.movi.32.a/q`.

**Mais la seule addition 32 bits par lane est `ee.vadds.s32`, et elle sature.** Mesuré sur
worker6 (`src/race/pie_probe.S`, sortie `PIE probe:`) :

| lane | opération | résultat | wrap attendu |
|------|-----------|----------|--------------|
| 0 | `0x7FFFFFFF + 1` | `0x7FFFFFFF` | `0x80000000` |
| 2 | `0x80000000 + (-1)` | `0x80000000` | `0x7FFFFFFF` |
| 3 | pas de dépassement | `0xACF13568` | ✔ identique |

SHA-256 est **entièrement bâti sur l'addition mod 2^32** → inutilisable en l'état. Aucune variante
non saturante n'existe (`ee.vadd.32`, `ee.vaddu.s32`, `ee.addq.s32`… tous rejetés à l'assemblage)
et aucun registre de contrôle ne désactive la saturation (`sar_byte` ne concerne que le décalage).

Contournements écartés, chiffrés à la louche :
- émuler l'add 32 bits par 2 adds 16 bits + propagation de carry : ~4-5 ops au lieu d'1 → mange
  tout le gain du 4-way (facteur 4 théorique) ;
- passer par `ee.vmulas.s16.qacc` (MAC vers accumulateur 40 bits) : extraction QACC coûteuse,
  accumulateur unique partagé → pas de 4-way réel.

**Conséquence : T9 (schedule 4-way) et T10 (rounds 4-way) sont annulées.** Le code de la sonde est
conservé (`RACE_PIE_PROBE=0`) pour que personne ne retente le pari sans lire ce verdict.

## Essai express — taille des jobs HW : **nul** (2026-08-01)

`NONCE_PER_JOB_HW` 16k → 64k (divise par 4 le coût de gestion de job : mutex, shared_ptr,
memcpy, `esp_sha_acquire_hardware`) : **304,2 → 304,6 kH/s**, soit +0,13 % = bruit.
Reverté (16k, comme l'upstream). L'écart entre le théorique issu des cycles (271 kH/s) et le
mesuré (262,7) ne vient donc pas de la gestion des jobs.

## T5 — interleave SW dans les attentes moteur : **REJETÉ** (2026-08-01)

Hypothèse : les 341 cyc/nonce passés à sonder `SHA_BUSY_REG` sont du CPU gratuit ;
y exécuter des rounds SHA-256 d'un flux indépendant devait rapporter ~+9 kH/s.

Implémentation : `src/race/race_sw_interleave.{h,cpp}`, SHA-256d découpé en machine à
états (crypto identique à nerdSHA256plus). **Correction validée** : auto-test contre
mbedtls, 4 nonces, `race_sw selftest: PASS`.

Résultats — **négatifs quel que soit le grain** :

| variante | khs_hw | khs_sw | total | delta |
|---|---|---|---|---|
| référence (interleave off) | 262,3 | 41,9 | **304,2** | — |
| 8 rounds/pas, code en flash | 110,8 | 55,7 | 166,4 | **−45,3 %** |
| 2 rounds/pas, `IRAM_ATTR` | 220,0 | 48,6 | 268,6 | **−11,7 %** |

Lecture : le SW produit bien plus (+6,7 kH/s à grain fin), mais le HW perd 42,3 kH/s pour
cela — **rapport 6:1 défavorable**. Chaque pas de 2 rounds ajoute ~88 cyc/nonce qui
**s'ajoutent** au temps de boucle au lieu d'être absorbés par l'attente.

Conclusion, contre-intuitive mais nette : **les cycles d'attente du moteur ne sont pas du
temps CPU récupérable.** Ce qui est mesuré comme « attente » n'est pas une fenêtre où le CPU
serait libre — insérer du travail y retarde le lancement de l'opération suivante presque
cycle pour cycle. Cohérent avec le rapport d'efficacité : le chemin HW hache un nonce en
915 cyc, le SW en 5 728 (6,3× plus cher) — voler des cycles au HW est perdant par nature,
sauf s'ils sont *réellement* gratuits, ce qu'ils ne sont pas.

Le module est **conservé, désactivé** (`RACE_INTERLEAVE=0`) : il est correct et documenté,
pour éviter que quelqu'un refasse le travail sans lire ce résultat. Retour à l'état nominal
vérifié : **304,1 kH/s**, mismatch=0.

## Écran : d'où vient vraiment le coût (2026-08-01)

Le headless donne +3,8 %, mais il n'est applicable qu'à worker6 (dalle HS). Décomposition
du coût pour récupérer le gain sur les mineurs à écran **fonctionnel** :

| variante (écran actif) | total | delta |
|---|---|---|
| référence upstream : `DELAY=100`, redraw 1×/s | 293,1 | — |
| animations à 2 fps (`DELAY=500`), redraw 1×/s | 294,0 | +0,3 % |
| **animations 10 fps, redraw 1×/3 s** | **300,3** | **+2,5 %** |
| headless total (référence worker6) | 304,2 | +3,8 % |

Enseignement : le coût n'est **pas** dans `animateCurrentScreen`/`doLedStuff` (appelés toutes les
`DELAY` ms — les ralentir ne rapporte que 0,3 %) mais dans **`drawCurrentScreen`**, le redraw
plein écran appelé **1×/s** quelle que soit la valeur de `DELAY`. L'espacer à 3 s récupère les
deux tiers du gain maximal en gardant un affichage parfaitement lisible (les chiffres d'un mineur
bougent lentement).

Flags : `RACE_DRAW_EVERY_S` (période de redraw en secondes, défaut 1 = comportement upstream),
`RACE_SCREEN_MS` (période d'animation, défaut 100).

**Applicable à la flotte** : +2,5 % sur les 5 mineurs à écran sain ≈ **+37 kH/s**, sans rien perdre
d'utile. worker6 garde le headless (+3,8 %).

## Correction : le gain du redraw espacé sur un écran SAIN est +0,7 %, pas +2,5 %

Le +2,5 % mesuré plus haut l'a été sur **worker6, dalle morte**, en réactivant le rendu. Écrire
en SPI vers un écran qui ne répond pas coûte bien plus cher que vers un écran sain : ce test
mesurait un cas pathologique, pas la flotte.

A/B sur **worker1 (écran sain, même mineur avant/après)** :

| worker1 | total | hw | sw |
|---|---|---|---|
| gheop7 (redraw 1×/s) | 298,3 | — | — |
| gheop8 Fleet (redraw 1×/3 s) | **300,3** | 259,7 | 40,6 |
| **gain réel** | **+2,0 kH/s (+0,7 %)** | | mismatch=0 |

Sanity check : le chiffre absolu du build redraw/3 s est **identique** sur worker1 (300,3) et
worker6 (300,3) — le firmware se comporte pareil, seule la référence différait.

**Leçon de méthode** : ne comparer que le *même* mineur avant/après. Les comparaisons entre
mineurs sont polluées par le silicium, la température et l'état du matériel (ici une dalle HS).

Gain flotte réel : +0,7 % × 5 ≈ **+10 kH/s**. Marginal — la vraie raison de déployer gheop8 sur
la flotte reste le **durcissement de la télémétrie** (POST isolé du watchdog anti-gel).

## Assembleur à la main sur `fill_fast` : **+0,0 kH/s** (2026-08-01)

Hypothèse testée : les 165 cyc de la phase `fill` (10 writes) contiennent du travail CPU
évitable — chargements de constantes, calcul d'adresse — que l'assembleur supprimerait en
gardant tout en registres.

Version écrite (`RACE_ASM_FILL=1`, `mining.cpp`) : adresse de base et constantes pré-chargées,
puis **10 `s32i` consécutifs** sans une instruction entre eux. Désassemblage vérifié avant flash :

```
2d5: l32r a2, ...      ; base SHA_TEXT
2d8: movi a3, 128      ; seule constante rechargée
2db: s32i.n a7, a2, 0  ; 10 stores d'affilée
...  (a5,a6,a4,a3,a8,a8,a8,a8)
2ed: s32i.n a9, a2, 60
```

Mesure sur worker6 (OTA, 2e report télémétrie) :

| | khsHw | khsSw | total | mismatch |
|---|---|---|---|---|
| version C | 262,4 | 41,9 | 304,3 | 0 |
| version ASM | **262,4** | 41,9 | **304,3** | 0 |

**Écart : 0,0 kH/s.** Identique à la décimale.

Conclusion : les 519 cyc/nonce d'accès registres (36 accès à ~14 cyc) sont de la **latence de bus
APB pure**. Le CPU n'attend pas ses instructions, il attend le bus — et GCC produisait déjà du
code quasi optimal. Corollaire mesuré : les 8 premiers writes coûtent 9,1 cyc (absorbés par le
tampon d'écriture), les suivants 16,5 cyc (tampon saturé) ; moyenne 13,2 = le débit du bus.

**Troisième confirmation indépendante du plafond matériel**, après l'interleave (le CPU « libre »
ne l'est pas) et le SIMD (l'instruction n'existe pas). Version C conservée (lisible, portable,
même perf) ; le code ASM reste sous `RACE_ASM_FILL=0` pour la trace.

## Ménage de fin de lot (2026-08-01)

Suppression du code des expériences **ratées et déjà documentées ci-dessus** — git et ce journal
gardent les chiffres, le désassemblage et le raisonnement ; garder le code mort en plus était
redondant.

| supprimé | lignes | verdict qui le justifie |
|---|---|---|
| `src/race/race_sw_interleave.{h,cpp}` + 5 blocs | 249 | interleave rejeté (−11,7 %) |
| `src/race/pie_probe.S` + 2 blocs | 15 | SIMD NO-GO (`ee.vadds.s32` sature) |
| version ASM de `fill_fast` (`RACE_ASM_FILL`) | ~25 | +0,0 kH/s (bus APB, pas le CPU) |
| `RACE_SW_SELFTEST` | 2 blocs | ne testait que l'interleave |

**~380 lignes en moins.** Flags restants : `RACE_RATE` (sonde de débit, active), `RACE_BENCH`
(profileur par phase, gardé désactivé — c'est l'outil qui a permis tous les diagnostics),
`RACE_HEADLESS` (worker6), `RACE_DRAW_EVERY_S` (flotte).

Vérifié : les 3 envs compilent (stock inchangé), worker6 reflashé → **304,1 kH/s**, mismatch=0
(contre 304,4 avant : identique au bruit près).

### Support des autres cartes : volontairement intact
Les ~30 autres boards du projet ne sont **pas** élaguées. Le linker écarte déjà le code inutilisé
(firmware 1,9 Mo sur une partition de ~6,5 Mo), donc l'élagage ne gagnerait ni octet ni cycle —
et il casserait la synchronisation avec l'upstream BitMaker (PR #801-804 ouvertes).
