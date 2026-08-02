// Host test for the checkValid() bugs reported in issue #797.
// Runs the original and the fixed implementation against known vectors.
//   g++ -O0 -g -fsanitize=address -o t checkvalid_test.cpp && ./t
#include <cstdio>
#include <cstring>
#include <cstdint>

static void reverse_bytes(unsigned char *b, size_t n) {
    for (size_t i = 0; i < n / 2; i++) { unsigned char t = b[i]; b[i] = b[n-1-i]; b[n-1-i] = t; }
}

// ---------- original ----------
static bool checkValid_orig(unsigned char* hash, unsigned char* target) {
    bool valid = true;
    unsigned char diff_target[32];
    memcpy(diff_target, &target, 32);        // BUG 1: copies the pointer's address
    reverse_bytes(diff_target, 32);
    long iters = 0;
    for (uint8_t i = 31; i >= 0; i--) {      // BUG 2: unsigned index wraps to 255 after 0
        if (++iters > 100000) { printf("    [INFINITE LOOP: >100000 iterations, bailing out]\n"); return valid; }
        if (hash[i] > diff_target[i]) { valid = false; break; }
    }
    printf("    [%ld iterations]\n", iters);
    return valid;
}

// ---------- fixed ----------
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
    // Little-endian convention: index 31 holds the most significant byte.
    // Difficulty-1 target is 00000000FFFF0000..00 big-endian -> 0xFF lands on 27/26.
    unsigned char target[32]; memset(target, 0, 32);
    target[27] = 0xFF; target[26] = 0xFF;

    struct Case { const char *name; unsigned char hash[32]; bool expect; };
    Case cases[4] = {
        {"all-zero hash (well below)",   {0}, true},
        {"high byte set (clearly above)",{0}, false},
        {"exactly equal to target",      {0}, true},
        {"above on a high byte",         {0}, false},
    };
    cases[1].hash[31] = 0x01;
    memcpy(cases[2].hash, target, 32);
    memcpy(cases[3].hash, target, 32); cases[3].hash[28] = 0x01;

    printf("%-32s | expected | fixed | original\n", "case");
    printf("---------------------------------+----------+-------+---------\n");
    int ko_fixed = 0, ko_orig = 0;
    for (auto &c : cases) {
        bool f = checkValid_fixed(c.hash, target);
        // The original reads hash[32..255] out of bounds. Give it an oversized
        // buffer so the test can observe its verdict instead of crashing -- the
        // overflow is still real in the firmware.
        unsigned char big[256]; memset(big, 0, sizeof(big)); memcpy(big, c.hash, 32);
        bool o = checkValid_orig(big, target);
        if (f != c.expect) ko_fixed++;
        if (o != c.expect) ko_orig++;
        printf("%-32s |  %-6s  | %-5s | %-5s\n", c.name,
               c.expect ? "true" : "false", f ? "true" : "false", o ? "true" : "false");
    }
    printf("\nfixed:    %d/4 correct\noriginal: %d/4 correct\n", 4 - ko_fixed, 4 - ko_orig);
    return ko_fixed ? 1 : 0;
}
