// Test hôte des bugs de checkValid() — BitMaker-hub/NerdMiner_v2#797
// Compare l'implémentation d'origine et la version corrigée sur des vecteurs connus.
//   g++ -O0 -g -fsanitize=address -o t checkvalid_test.cpp && ./t
#include <cstdio>
#include <cstring>
#include <cstdint>

static void reverse_bytes(unsigned char *b, size_t n) {
    for (size_t i = 0; i < n / 2; i++) { unsigned char t = b[i]; b[i] = b[n-1-i]; b[n-1-i] = t; }
}

// ---------- version d'origine (upstream) ----------
static bool checkValid_orig(unsigned char* hash, unsigned char* target) {
    bool valid = true;
    unsigned char diff_target[32];
    memcpy(diff_target, &target, 32);        // BUG 1 : adresse du pointeur
    reverse_bytes(diff_target, 32);
    long iters = 0;
    for (uint8_t i = 31; i >= 0; i--) {      // BUG 2 : i unsigned -> 255 après 0
        if (++iters > 100000) { printf("    [BOUCLE INFINIE: >100000 iterations, arret force]\n"); return valid; }
        if (hash[i] > diff_target[i]) { valid = false; break; }
    }
    printf("    [%ld iterations]\n", iters);
    return valid;
}

// ---------- version corrigée ----------
static bool checkValid_fixed(unsigned char* hash, unsigned char* target) {
    bool valid = true;
    unsigned char diff_target[32];
    memcpy(diff_target, target, 32);
    for (int i = 31; i >= 0; i--) {
        if (hash[i] > diff_target[i]) { valid = false; break; }
        if (hash[i] < diff_target[i]) { valid = true;  break; }
    }
    return valid;
}

int main() {
    // Convention little-endian : index 31 = octet de poids fort.
    // Cible difficulté 1 : big-endian 00000000FFFF0000..00 -> inversée, 0xFF en 27/26.
    unsigned char target[32]; memset(target, 0, 32);
    target[27] = 0xFF; target[26] = 0xFF;

    struct Case { const char *name; unsigned char hash[32]; bool expect; };
    Case cases[4] = {
        {"hash nul (bien en dessous)",  {0}, true},
        {"octet de poids fort a 1",     {0}, false},
        {"exactement egal a la cible",  {0}, true},
        {"depasse sur un octet haut",   {0}, false},
    };
    cases[1].hash[31] = 0x01;
    memcpy(cases[2].hash, target, 32);
    memcpy(cases[3].hash, target, 32); cases[3].hash[28] = 0x01;

    printf("%-32s | attendu | corrige | origine\n", "cas");
    printf("---------------------------------+---------+---------+--------\n");
    int ko_fixed = 0, ko_orig = 0;
    for (auto &c : cases) {
        bool f = checkValid_fixed(c.hash, target);
        // NOTE: la version d'origine lit hash[32..255] hors limites.
        // On lui donne un buffer surdimensionné pour éviter le crash et
        // observer son verdict — le débordement reste réel dans le firmware.
        unsigned char big[256]; memset(big, 0, sizeof(big)); memcpy(big, c.hash, 32);
        bool o = checkValid_orig(big, target);
        if (f != c.expect) ko_fixed++;
        if (o != c.expect) ko_orig++;
        printf("%-32s |  %-5s  |  %-5s  |  %-5s\n", c.name,
               c.expect ? "vrai" : "faux", f ? "vrai" : "faux", o ? "vrai" : "faux");
    }
    printf("\ncorrige : %d/4 corrects\norigine : %d/4 corrects\n", 4 - ko_fixed, 4 - ko_orig);
    return ko_fixed ? 1 : 0;
}
