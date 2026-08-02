// Host test for the truncated coinbase in calculateMiningData() (src/utils.cpp).
// A coinbase transaction longer than the static buffer was silently cut, which
// corrupts the coinbase hash, the merkle root and the block header, so every share
// from that job is rejected. The odd character count left behind also made
// to_byte_array write one byte past the end of its output array.
//
//   g++ -O0 -o t test/coinbase_length_test.cpp && ./t
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

static uint8_t hex(char c){ uint8_t r; if(c>='0'&&c<='9')r=c-'0'; else if(c>='a'&&c<='f')r=c-'a'+10;
    else if(c>='A'&&c<='F')r=c-'A'+10; else r=0; return r&0x0F; }

static int to_byte_array(const char *in, size_t in_size, uint8_t *out) {
    int count = 0;
    if (in_size % 2) {
        while (*in && out) {
            *out = hex(*in++);
            if (!*in) return count;
            *out = (*out << 4) | hex(*in++);
            out++; count++;
        }
        return count;
    }
    while (*in && out) {
        uint8_t hi = hex(*in++); uint8_t lo = hex(*in++);
        *out++ = (hi << 4) | lo; count++;
    }
    return count;
}

struct Outcome { size_t assembled, kept, dropped, capacity; int written; };

static Outcome run(size_t bufsize, bool round_len, const std::string &cb) {
    Outcome o{};
    o.assembled = cb.size();
    char *buf = new char[bufsize];
    snprintf(buf, bufsize, "%s", cb.c_str());
    o.kept = strlen(buf);
    o.dropped = o.assembled - o.kept;
    size_t hex_len = round_len ? (o.kept & ~(size_t)1) : (o.kept/2)*2;
    o.capacity = o.kept/2;
    uint8_t *out = new uint8_t[o.capacity + 8];   // slack so the test itself is safe
    o.written = to_byte_array(buf, hex_len, out);
    delete[] out; delete[] buf;
    return o;
}

static void report(const char *name, const Outcome &o) {
    printf("%-10s assembled %4zu | kept %4zu | dropped %3zu bytes | wrote %d into [%zu]%s\n",
           name, o.assembled/2, o.kept/2, o.dropped/2, o.written, o.capacity,
           (size_t)o.written > o.capacity ? "   <-- PAST THE END" : "");
}

int main() {
    // 312-byte coinbase: unremarkable for a pool carrying several outputs.
    std::string cb = std::string(400,'a') + "c068be21" + "0000000000000001" + std::string(200,'b');
    printf("coinbase transaction: %zu bytes (%zu hex chars)\n\n", cb.size()/2, cb.size());

    Outcome before = run(512,  false, cb);
    Outcome after  = run(1024, true,  cb);
    report("original", before);
    report("fixed", after);

    bool ok = after.dropped == 0 && (size_t)after.written == after.capacity;
    printf("\n%s\n", ok ? "PASS: nothing dropped, no write past the end" : "FAIL");
    return ok ? 0 : 1;
}
