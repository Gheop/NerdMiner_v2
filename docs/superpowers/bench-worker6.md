# Bench worker6 — race/gheop8

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
