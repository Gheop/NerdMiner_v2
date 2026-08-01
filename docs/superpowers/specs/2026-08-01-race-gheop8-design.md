# race/gheop8 — firmware de course NerdMiner (ESP32-S3)

Date : 2026-08-01
Statut : validé (design), prêt pour plan d'implémentation
Branche : `race/gheop8` (depuis `perf/hashrate` @ e1c44d4, gheop7)

## Objectif

Maximiser le hashrate SHA256d d'un NerdMiner T-Display-S3, sans sacrifier la
fiabilité acquise (gheop2-7 : WiFi durci, watchdogs, OTA, télémétrie). Banc
d'essai unique : **worker6** (192.168.0.129, `nerdminer-34a0.local`, écran HS,
USB possible). Les 5 autres mineurs ne sont jamais touchés par cette branche
tant qu'une validation de 24 h sur worker6 n'a pas eu lieu.

Cadrage honnête : à ~127 T de difficulté réseau, même 450 KH/s ne rend pas un
bloc probable (espérance en milliers de milliards d'années). On maximise les
tickets/s, l'uptime, et le « best difficulty » (share 32 bits ≈ 40 min de
flotte). C'est une course de perf mesurée, pas un plan de retraite.

## État de départ (mesuré dans le code, à confirmer au bench)

- Deux chemins de hash simultanés existent déjà :
  - `minerWorkerHw` (prio 3) : moteur SHA matériel, jobs de 16k nonces,
    SHA256d en 2 ops moteur/nonce, early-exit 16 bits, fast-fill gheop
    (16→10 writes), zéro de SHA_TEXT[9..14] une fois par job.
  - `minerWorkerSw` (prio 1) : nerdSHA256plus (midstate + bake), jobs 4k nonces.
- ~300 KH/s cumulés. À 240 MHz, chemin HW ≈ 960 cycles/nonce dont ~140 pour le
  moteur → **~800 cycles/nonce d'overhead** (writes APB, polling, midstate).
- Interdits connus : `-O3` et `#pragma O2` régressent (testés, ne pas refaire).
  `CONFIG_ESP_INT_WDT_TIMEOUT_MS` non modifiable (libs précompilées arduino-esp32).

## Décisions de cadrage (validées)

- **Ambition palier 3** : bench + quick wins + chirurgie boucle HW + pari
  SIMD/PIE assembleur.
- **Branche de course** : liberté totale, pas de contrainte PR-able/multi-board.
  `perf/hashrate` reste propre pour l'upstream.
- **Validation au bench instantané** entre itérations. Garde-fou : une nuit de
  tenue sur worker6 avant tout déploiement flotte (hors périmètre de ce lot).
- **Approche A** : chirurgie incrémentale du cœur chaud, infraréseau intouchée.

## Workstreams (dans l'ordre)

### W1 — Harnais de bench (prérequis absolu)

- Compteurs de cycles `esp_cpu_get_ccount()` par phase de la boucle HW :
  écriture midstate / fill / attente moteur op1 / inter / attente op2 /
  lecture-test / vérif SW. Agrégés par job, moyennés, imprimés sur serial.
- Hashrate **séparé par chemin** : `khs_hw` et `khs_sw` calculés là où les
  compteurs `hashes` sont incrémentés, exposés :
  - sur serial (ligne `Bench:` toutes les 30 s),
  - dans la télémétrie dashboard (champs `khsHw`, `khsSw` ajoutés au JSON de
    `postTelemetry`, additifs donc sans casser l'ingestion existante).
- Baseline chiffrée de worker6 committée dans le repo (fichier
  `docs/superpowers/bench-worker6.md`, une ligne par mesure : commit, config,
  khs_hw, khs_sw, cycles/nonce par phase).
- Impact du harnais lui-même < 1 % (compteurs conditionnés par flag de build
  `RACE_BENCH`, échantillonnage 1 job sur N si besoin).

### W2 — Chirurgie de la boucle HW (~800 cycles/nonce à attaquer)

Dans l'ordre de rendement attendu, chaque étape = un commit + un chiffre :

1. **Pipeline** : restructurer le per-nonce pour recouvrir l'attente moteur
   (~70 cycles × 2) avec du travail utile : préparer le fill du nonce suivant,
   faire la comparaison/lecture du résultat précédent pendant l'op en cours.
2. **Writes minimaux** : par nonce d'un même job, seul le mot du nonce change
   dans le bloc 2. Audit du fast-fill pour ne réécrire que : nonce (+ time si
   ntime bouge), et vérifier si des mots de SHA_TEXT survivent entre ops
   (padding, longueurs). Idem pour la séquence midstate : chercher un ordre
   d'ops qui évite de réécrire les 8 mots à chaque nonce si le HW le permet.
3. **Audit des ops moteur** : `sha_ll_load` doubles, REG_WRITE redondants,
   barrières DPORT superflues.
4. **Test DMA chaîné** (scaffolding `nerdSHA_HWTest` existant) : bench 1 h max,
   on garde seulement si le chiffre gagne. Attendu perdant sur blocs de 64 o.

Cible W2 : chemin HW 250 → 300-330 KH/s.

### W3 — Mode headless (worker6, écran mort)

- Flag de build `RACE_HEADLESS` : pas d'init TFT, pas de redraw, pas
  d'animations dans `runMonitor` (garder la logique stats/watchdog/télémétrie,
  supprimer uniquement le rendu). Cycles + bus SPI rendus au chemin SW.
- Éventuel ajustement de priorité de `MinerSw` mesuré au bench (attention à ne
  pas affamer stratum/WiFi : toute hausse de prio doit passer le test « le
  mineur reçoit toujours ses jobs et la télémétrie part toujours »).

Cible W3 : +5-20 KH/s sur le chemin SW.

### W4 — Le pari : hasher SW en assembleur PIE (SIMD 128 bits du S3)

SHA-256 non vectorisable sur un flux ; l'angle est **N nonces entrelacés**
(2 ou 4 lanes 32 bits). Étapes avec porte de sortie à chaque palier :

1. **Microbench PIE** : inventorier les instructions réellement utilisables
   (add/xor/and/or/shifts 32 bits par lane ; les rotations n'existent pas →
   émulation 2 shifts + or, coût à mesurer). Verdict chiffré : cycles pour un
   round SHA-256 4-way vs 4× le round scalaire de nerdSHA256plus.
2. **Message schedule SIMD seul** (W16..W63), rounds scalaires : gain partiel
   plus simple à valider.
3. **Rounds complets 4-way** si (1) et (2) sont gagnants.
4. À tout moment : si le chiffre perd contre nerdSHA256plus, on jette, on le
   note dans le bench log, et le firmware garde l'ancien chemin.

Cible W4 (incertaine par nature) : chemin SW 50 → 60-120 KH/s.

## Protocole de mesure

- Bench = 60 s minimum de `Bench:` sur serial ou télémétrie, worker6, WiFi
  connecté et jobs réels de public-pool (pas de jobs synthétiques : la vraie
  boucle inclut le coût des jobs/queues).
- Chaque commit d'optim porte son avant/après dans le message de commit et une
  ligne dans `bench-worker6.md`.
- La vérification de correction reste active en continu : le double-check SW
  des candidats 16 bits (`***HW sha256 esp32s3 bug detected***`) doit rester à
  zéro occurrence ; toute optim qui produit un mismatch est rejetée quel que
  soit son gain.
- Flash : OTA de préférence (`NerdminerV2-OTA`, cible `192.168.0.129`), USB en
  secours (worker6 à 3 m).

## Hors périmètre

- Déploiement sur worker1-5 (nécessitera : nuit de tenue sur worker6, puis
  décision explicite de sib).
- Refonte réseau/stratum (déjà durci ; le staleness des queues ≈ 0,25 s est
  négligeable).
- Overclock au-delà de 240 MHz (non supporté par IDF), ntime rolling, ULP
  (< 1 KH/s), firmware ESP-IDF from scratch.
- Toute info privée dans les commits (adresse BTC, mots de passe, tokens) :
  la branche peut finir sur le fork public.

## Risques

- Le pari PIE peut coûter des jours pour zéro gain : accepté, portes de sortie
  chiffrées à chaque étape.
- Une optim HW peut casser la correction silencieusement : couvert par le
  double-check SW permanent + rejet au moindre mismatch.
- worker6 peut se briquer logiciellement : récupérable en USB (3 m), procédure
  erase_flash + reflash documentée dans le CLAUDE.md projet.
