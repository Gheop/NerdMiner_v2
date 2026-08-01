# Bench worker6 — race/gheop8

Protocole : télémétrie (fenêtres 60 s) ou serial `Bench:`, worker6 sur jobs réels
public-pool. Gate : gain chiffré ET `shaMismatch == 0`, sinon revert.

| date | commit fw | env | khs_hw | khs_sw | total | note |
|------|-----------|-----|--------|--------|-------|------|
| 2026-08-01 | df35dbc | Race | 259.4 | 38.0 | 297.3 | **baseline** (algo stock, split mesuré sur fenêtre 60 s propre, mismatch=0) |

Cycles/nonce chemin HW à 240 MHz : 240e6 / 259.4e3 ≈ **925 cyc/nonce** (moteur ≈ 140 → ~785 d'overhead à attaquer).

## Incident noté (2026-08-01, hors perf)
Télémétrie figée à uptime 357 s (mining intact à 297 KH/s) : la tâche `healthWatchdog`
(Health + POST + protection anti-gel) s'est tue, très probablement `http.POST` bloqué
pendant le rollout du dashboard (timing exact). Gel non reproduit en conditions stables.
Durcissement prévu : sortir le POST réseau de la tâche watchdog (tâche télémétrie dédiée).
