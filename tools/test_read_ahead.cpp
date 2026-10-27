// Self-test for source/io/readahead.h (the read-ahead behind read-only FsFileOpenNode reads).
// g++ -std=c++17 -I source/io tools/test_read_ahead.cpp -o test_read_ahead && ./test_read_ahead
#include "readahead.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

struct FakeFile { // the host file: bytes, a generation and a count of host reads
    std::vector<uint8_t> bytes;
    uint32_t generation = 0;
    int reads = 0;
    int32_t readAt(uint8_t* dst, int64_t offset, uint32_t len) {
        reads++;
        if (offset >= (int64_t)bytes.size()) return 0;
        uint32_t n = (uint32_t)std::min<int64_t>(len, (int64_t)bytes.size() - offset);
        memcpy(dst, bytes.data() + offset, n);
        return (int32_t)n;
    }
};

struct Open { // one open file: its position over the shared FakeFile, like FsFileOpenNode
    FakeFile& f;
    ReadAheadBuffer ra;
    int64_t pos = 0;
    int32_t read(uint8_t* dst, uint32_t len) {
        return ra.read(dst, len, pos, f.generation, [this](uint8_t* d, int64_t o, uint32_t n) { return f.readAt(d, o, n); });
    }
    bool expect(uint32_t len, int64_t at, uint32_t want) { // reads len bytes, expects want bytes of the file from at
        std::vector<uint8_t> got(len + 1);
        int32_t n = read(got.data(), len);
        return n == (int32_t)want && !memcmp(got.data(), f.bytes.data() + at, want);
    }
};

int main() {
    FakeFile f;
    f.bytes.resize(200000);
    for (size_t i = 0; i < f.bytes.size(); i++) f.bytes[i] = (uint8_t)(i * 7 + i / 251);
    Open a{f};

    // tiny sequential reads: one host read per 64 KiB, exact data
    for (int i = 0; i < 1000; i++) CHECK(a.expect(16, i * 16, 16));
    CHECK(f.reads == 1);
    CHECK(a.pos == 16000);

    // a read across the block end is not short
    a.pos = ReadAheadBuffer::SIZE - 5;
    CHECK(a.expect(10, ReadAheadBuffer::SIZE - 5, 10));
    CHECK(a.pos == ReadAheadBuffer::SIZE + 5);

    // seek back into the block: no host read
    int before = f.reads;
    a.pos = ReadAheadBuffer::SIZE;
    CHECK(a.expect(100, ReadAheadBuffer::SIZE, 100));
    CHECK(f.reads == before);

    // pread pattern: save, seek elsewhere, read, restore, continue
    int64_t saved = a.pos;
    a.pos = 150000;
    CHECK(a.expect(8, 150000, 8));
    a.pos = saved;
    CHECK(a.expect(8, saved, 8));

    // big reads go straight to the host
    before = f.reads;
    a.pos = 1000;
    CHECK(a.expect(100000, 1000, 100000));
    CHECK(f.reads == before + 1);
    CHECK(a.pos == 101000);

    // end of file: short read, then 0
    a.pos = 199990;
    CHECK(a.expect(100, 199990, 10));
    CHECK(a.pos == 200000);
    uint8_t b[4];
    CHECK(a.read(b, 4) == 0);
    a.pos = 300000; // past the end
    CHECK(a.read(b, 4) == 0);

    // a write through another open file (generation bump) is seen
    a.pos = 0;
    CHECK(a.expect(4, 0, 4));
    f.bytes[4] ^= 0xff;
    f.generation++;
    CHECK(a.expect(4, 4, 4));
    // without the bump the old block would still be served: proves the check matters
    f.bytes[8] ^= 0xff;
    std::vector<uint8_t> got(4);
    CHECK(a.read(got.data(), 4) == 4 && got[0] != f.bytes[8]);
    f.generation++;
    a.pos = 8;
    CHECK(a.expect(4, 8, 4));

    // the file grows past a short last block (another fd appended)
    Open c{f};
    c.pos = 199995;
    CHECK(c.expect(10, 199995, 5));
    f.bytes.resize(200010, 0x5a);
    f.generation++;
    c.pos = 199995;
    CHECK(c.expect(20, 199995, 15));

    // host read errors: returned when nothing was read yet
    ReadAheadBuffer ra;
    int64_t p = 0;
    CHECK(ra.read(b, 4, p, 0, [](uint8_t*, int64_t, uint32_t) { return -1; }) == -1);
    CHECK(p == 0);

    printf(failures ? "read-ahead self-test: %d failures\n" : "read-ahead self-test: ok\n", failures);
    return failures ? 1 : 0;
}
