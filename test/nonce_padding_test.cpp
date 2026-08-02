// Host test for the truncated nonce in tx_mining_submit() -- BitMaker-hub/NerdMiner_v2#750
// Stratum's mining.submit nonce field must be exactly 8 hex chars. Arduino's
// String(nonce, HEX) drops leading zeros, so every nonce below 0x10000000 goes
// out short and the pool rebuilds a different block header from it.
//
//   g++ -O0 -o t test/nonce_padding_test.cpp && ./t
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

// ---------- what Arduino's String(value, HEX) produces ----------
// utoa() semantics: no leading zeros, lowercase, variable length.
static std::string arduino_string_hex(unsigned long value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lx", value);
    return std::string(buf);
}

// ---------- the fix ----------
static std::string fixed_hex(unsigned long value) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", value & 0xFFFFFFFFUL);
    return std::string(buf);
}

int main() {
    struct Case { unsigned long nonce; const char *note; };
    const Case cases[] = {
        {0x1a9a342aUL, "typical nonce, no leading zero"},
        {0x0a9a049cUL, "one leading zero"},
        {0x00bcc3e1UL, "two leading zeros"},
        {0x000000ffUL, "small nonce"},
        {0x00000000UL, "zero"},
        {0xffffffffUL, "maximum"},
    };

    printf("%-12s | %-10s | %-10s | %s\n", "nonce", "current", "fixed", "note");
    printf("-------------+------------+------------+----------------------------\n");
    int broken = 0;
    for (const Case &c : cases) {
        std::string cur = arduino_string_hex(c.nonce);
        std::string fix = fixed_hex(c.nonce);
        bool bad = cur.size() != 8;
        if (bad) broken++;
        printf("0x%08lx   | %-10s | %-10s | %s%s\n", c.nonce, cur.c_str(), fix.c_str(),
               c.note, bad ? "  <-- SHORT FIELD" : "");
        if (fix.size() != 8) { printf("FAIL: fixed output is not 8 chars\n"); return 1; }
    }

    printf("\n%d of %zu sample nonces are submitted with a short field.\n",
           broken, sizeof(cases) / sizeof(cases[0]));
    printf("Overall rate: nonces below 0x10000000 are %.2f%% of the 32-bit space.\n",
           100.0 * 0x10000000UL / 4294967296.0);
    return 0;
}
