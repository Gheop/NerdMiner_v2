/************************************************************************************
 * race/gheop8 — SHA-256d logiciel en machine à états (voir race_sw_interleave.h).
 * Crypto reprise telle quelle de ShaTests/nerdSHA256plus.cpp (Bitmaker / Blockstream
 * Jade) : mêmes constantes K, mêmes fonctions de round. Seul l'ordonnancement change.
 ************************************************************************************/
#include <Arduino.h>
#include <string.h>
#include "race_sw_interleave.h"
#include "../ShaTests/nerdSHA256plus.h"
#include "mbedtls/sha256.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n) ((x) >> (n))
#define S0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define S1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define S2(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define S3(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define F0(x, y, z) (((x) & (y)) | ((z) & ((x) | (y))))
#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))

DRAM_ATTR static const uint32_t RK[64] = {
    0x428A2F98L, 0x71374491L, 0xB5C0FBCFL, 0xE9B5DBA5L, 0x3956C25BL,
    0x59F111F1L, 0x923F82A4L, 0xAB1C5ED5L, 0xD807AA98L, 0x12835B01L,
    0x243185BEL, 0x550C7DC3L, 0x72BE5D74L, 0x80DEB1FEL, 0x9BDC06A7L,
    0xC19BF174L, 0xE49B69C1L, 0xEFBE4786L, 0x0FC19DC6L, 0x240CA1CCL,
    0x2DE92C6FL, 0x4A7484AAL, 0x5CB0A9DCL, 0x76F988DAL, 0x983E5152L,
    0xA831C66DL, 0xB00327C8L, 0xBF597FC7L, 0xC6E00BF3L, 0xD5A79147L,
    0x06CA6351L, 0x14292967L, 0x27B70A85L, 0x2E1B2138L, 0x4D2C6DFCL,
    0x53380D13L, 0x650A7354L, 0x766A0ABBL, 0x81C2C92EL, 0x92722C85L,
    0xA2BFE8A1L, 0xA81A664BL, 0xC24B8B70L, 0xC76C51A3L, 0xD192E819L,
    0xD6990624L, 0xF40E3585L, 0x106AA070L, 0x19A4C116L, 0x1E376C08L,
    0x2748774CL, 0x34B0BCB5L, 0x391C0CB3L, 0x4ED8AA4AL, 0x5B9CCA4FL,
    0x682E6FF3L, 0x748F82EEL, 0x78A5636FL, 0x84C87814L, 0x8CC70208L,
    0x90BEFFFAL, 0xA4506CEBL, 0xBEF9A3F7L, 0xC67178F2L};

DRAM_ATTR static const uint32_t SHA256_IV[8] = {
    0x6A09E667L, 0xBB67AE85L, 0x3C6EF372L, 0xA54FF53AL,
    0x510E527FL, 0x9B05688CL, 0x1F83D9ABL, 0x5BE0CD19L};

// phase 0 = compression du bloc 2 de l'en-tête, phase 1 = second SHA (double hash)
struct RaceSwState {
    uint32_t W[64];
    uint32_t W0[16];    // bloc 2 d'origine : W[0..15] est écrasé par la phase 1
    uint32_t A[8];      // état de travail a..h
    uint32_t Hin[8];    // état d'entrée de la compression courante (pour l'addition finale)
    uint32_t mid[8];    // midstate du job (pour recharger un nonce sans reconvertir)
    uint8_t  hash[32];  // digest final, big-endian
    uint8_t  round;
    uint8_t  phase;
    bool     busy;
};

static RaceSwState s;

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Prépare la compression courante : W[0..15] déjà remplis par l'appelant.
static IRAM_ATTR void race_sw_begin_compression(const uint32_t *iv)
{
    for (int i = 0; i < 8; ++i) {
        s.Hin[i] = iv[i];
        s.A[i] = iv[i];
    }
    s.round = 0;
}

void race_sw_load(const uint32_t *midstate, const uint8_t *block2, uint32_t nonce)
{
    // Bloc 2 de l'en-tête : 16 mots big-endian, le nonce occupe les octets 12..15.
    for (int i = 0; i < 16; ++i)
        s.W0[i] = be32(block2 + i * 4);
    s.W0[3] = nonce;   // le nonce est déjà en ordre "moteur" chez l'appelant
    for (int i = 0; i < 8; ++i)
        s.mid[i] = midstate[i];
    memcpy(s.W, s.W0, sizeof(s.W0));

    race_sw_begin_compression(midstate);
    s.phase = 0;
    s.busy = true;
}

IRAM_ATTR void race_sw_reload_nonce(uint32_t nonce)
{
    memcpy(s.W, s.W0, sizeof(s.W0));
    s.W[3] = nonce;
    race_sw_begin_compression(s.mid);
    s.phase = 0;
    s.busy = true;
}

bool race_sw_busy(void) { return s.busy; }
const uint8_t *race_sw_hash(void) { return s.hash; }

// Exécute au plus RACE_SW_ROUNDS rounds de la compression en cours ; enchaîne les
// phases quand une compression se termine. Retourne true sur digest final prêt.
IRAM_ATTR bool race_sw_step(void)
{
    if (!s.busy)
        return false;

    uint32_t a = s.A[0], b = s.A[1], c = s.A[2], d = s.A[3];
    uint32_t e = s.A[4], f = s.A[5], g = s.A[6], h = s.A[7];
    uint32_t *W = s.W;
    int t = s.round;
    const int tend = (t + RACE_SW_ROUNDS > 64) ? 64 : t + RACE_SW_ROUNDS;

    for (; t < tend; ++t) {
        if (t >= 16)
            W[t] = S1(W[t - 2]) + W[t - 7] + S0(W[t - 15]) + W[t - 16];
        const uint32_t temp1 = h + S3(e) + F1(e, f, g) + RK[t] + W[t];
        const uint32_t temp2 = S2(a) + F0(a, b, c);
        h = g; g = f; f = e;
        e = d + temp1;
        d = c; c = b; b = a;
        a = temp1 + temp2;
    }

    s.A[0] = a; s.A[1] = b; s.A[2] = c; s.A[3] = d;
    s.A[4] = e; s.A[5] = f; s.A[6] = g; s.A[7] = h;
    s.round = (uint8_t)t;

    if (t < 64)
        return false;   // compression en cours, on reprendra au prochain appel

    // Compression terminée : addition finale avec l'état d'entrée.
    uint32_t out[8];
    for (int i = 0; i < 8; ++i)
        out[i] = s.Hin[i] + s.A[i];

    if (s.phase == 0) {
        // Le digest du 1er SHA devient le message du 2nd, avec padding 256 bits.
        for (int i = 0; i < 8; ++i)
            s.W[i] = out[i];
        s.W[8] = 0x80000000u;
        for (int i = 9; i < 15; ++i)
            s.W[i] = 0;
        s.W[15] = 256;
        race_sw_begin_compression(SHA256_IV);
        s.phase = 1;
        return false;
    }

    // Phase 1 terminée → digest final en big-endian.
    for (int i = 0; i < 8; ++i) {
        s.hash[i * 4 + 0] = (uint8_t)(out[i] >> 24);
        s.hash[i * 4 + 1] = (uint8_t)(out[i] >> 16);
        s.hash[i * 4 + 2] = (uint8_t)(out[i] >> 8);
        s.hash[i * 4 + 3] = (uint8_t)(out[i]);
    }
    s.busy = false;
    return true;
}

bool race_sw_selftest(void)
{
    // En-tête de 80 octets (comme un vrai bloc) + padding SHA-256 jusqu'à 128.
    // Référence indépendante : mbedtls. nerd_sha256d_baked ne convient PAS comme
    // référence — elle sort en avance sur les non-candidats (a7 & 0xFFFF) et ne
    // remplit alors pas doubleHash.
    uint8_t header[128];
    memset(header, 0, sizeof(header));
    for (int i = 0; i < 80; ++i)
        header[i] = (uint8_t)(i * 7 + 13);
    header[80] = 0x80;
    header[126] = 0x02;   // longueur = 640 bits = 0x0280
    header[127] = 0x80;

    uint32_t midstate[8];
    nerd_mids(midstate, header);

    bool ok = true;
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t nonce = 0x12345678u + k * 0x01010101u;
        header[76] = (uint8_t)(nonce >> 24);
        header[77] = (uint8_t)(nonce >> 16);
        header[78] = (uint8_t)(nonce >> 8);
        header[79] = (uint8_t)(nonce);

        uint8_t h1[32], ref[32];
        mbedtls_sha256(header, 80, h1, 0);
        mbedtls_sha256(h1, 32, ref, 0);

        race_sw_load(midstate, header + 64, nonce);
        int guard = 0;
        while (!race_sw_step() && ++guard < 64) { }

        if (memcmp(ref, race_sw_hash(), 32) != 0) {
            ok = false;
            Serial.printf("race_sw selftest MISMATCH nonce=%08x\n", (unsigned)nonce);
            Serial.printf("  ref="); for (int i = 0; i < 32; ++i) Serial.printf("%02x", ref[i]);
            Serial.printf("\n  got="); for (int i = 0; i < 32; ++i) Serial.printf("%02x", race_sw_hash()[i]);
            Serial.printf("\n");
        }
    }
    Serial.printf("race_sw selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
