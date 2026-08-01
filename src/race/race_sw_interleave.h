/************************************************************************************
 * race/gheop8 — SHA-256d logiciel découpé en machine à états.
 *
 * Pourquoi : la boucle de minage matérielle passe ~340 cyc/nonce (37 %) à attendre
 * le moteur SHA dans un busy-poll. Ce module hache un flux INDÉPENDANT par petits
 * paquets de rounds, appelés depuis ces fenêtres d'attente : le CPU du cœur HW
 * produit des hashs au lieu de sonder un registre.
 *
 * Contraintes de conception :
 *  - un seul appelant (la tâche MinerHw) → état statique, pas de verrou ;
 *  - un pas doit tenir dans la fenêtre la plus courte (~163 cyc) → RACE_SW_ROUNDS
 *    rounds par appel, sans poll intermédiaire ;
 *  - crypto identique à ShaTests/nerdSHA256plus.cpp (mêmes K, mêmes fonctions de
 *    round), validée contre nerd_sha256d_baked par un auto-test au démarrage.
 ************************************************************************************/
#ifndef RACE_SW_INTERLEAVE_H_
#define RACE_SW_INTERLEAVE_H_

#include <stdint.h>
#include <stdbool.h>

// Rounds exécutés par appel. 8 rounds ≈ 100 cyc, sous la plus courte fenêtre
// d'attente mesurée (163 cyc) → le poll du moteur n'est jamais retardé.
#ifndef RACE_SW_ROUNDS
#define RACE_SW_ROUNDS 2
#endif

// Charge un nouveau nonce à hacher. midstate = digest du bloc 1 (8 mots),
// block2 = les 64 octets du second bloc d'en-tête (nonce inclus, big-endian).
void race_sw_load(const uint32_t *midstate, const uint8_t *block2, uint32_t nonce);

// Recharge le même bloc avec un nouveau nonce (évite de reconvertir les 16 mots).
void race_sw_reload_nonce(uint32_t nonce);

// Avance d'au plus RACE_SW_ROUNDS rounds. Retourne true quand le SHA-256d du
// nonce courant est terminé : le digest est alors disponible via race_sw_hash().
bool race_sw_step(void);

// true si un nonce est en cours (sinon il faut appeler race_sw_load).
bool race_sw_busy(void);

// Digest final du dernier nonce terminé, 32 octets big-endian.
const uint8_t *race_sw_hash(void);

// Auto-test : compare la machine à états à nerd_sha256d_baked sur des vecteurs
// dérivés d'un en-tête réel. Retourne true si tout concorde. À appeler au boot.
bool race_sw_selftest(void);

#endif /* RACE_SW_INTERLEAVE_H_ */
