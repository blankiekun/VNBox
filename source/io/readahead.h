/*
 *  Copyright (C) 2026  The BoxedWine Team
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __READAHEAD_H__
#define __READAHEAD_H__

#include <cstdint>
#include <cstring>
#include <memory>

// Read-ahead for a read-only host file: small sequential reads are served from one 64 KiB
// block instead of one host read each. On the web, every host read of a lazy game file is a
// JS call that suspends the wasm stack (lazyfs.js), so games reading a few bytes at a time
// spent most of each second in those round trips.
// The caller owns the file position (pos) and passes the file's data generation, which every
// write, truncate or resize through any open file of the same data bumps: a stale block is
// dropped. readAt(dst, offset, len) reads the host file; returns bytes read or < 0.
// Tested by tools/test_read_ahead.cpp.
class ReadAheadBuffer {
public:
    static constexpr uint32_t SIZE = 64 * 1024;

    template <typename ReadAt>
    int32_t read(uint8_t* dst, uint32_t len, int64_t& pos, uint32_t generation, ReadAt readAt) {
        if (this->len && this->generation != generation) {
            this->len = 0;
        }
        uint32_t done = 0;
        while (done < len) {
            if (pos < start || pos >= start + this->len) {
                if (len - done >= SIZE) { // big read: straight into the caller's memory
                    int32_t n = readAt(dst + done, pos, len - done);
                    if (n < 0) {
                        return done ? (int32_t)done : n;
                    }
                    pos += n;
                    return (int32_t)(done + n);
                }
                if (!data) {
                    data.reset(new uint8_t[SIZE]);
                }
                int32_t n = readAt(data.get(), pos, SIZE);
                if (n <= 0) {
                    this->len = 0;
                    return (n < 0 && !done) ? n : (int32_t)done;
                }
                start = pos;
                this->len = (uint32_t)n;
                this->generation = generation;
            }
            uint32_t from = (uint32_t)(pos - start);
            uint32_t n = this->len - from;
            if (n > len - done) {
                n = len - done;
            }
            memcpy(dst + done, data.get() + from, n);
            done += n;
            pos += n;
            if (this->len < SIZE && pos == start + this->len) {
                break; // a short block ends at the end of the file
            }
        }
        return (int32_t)done;
    }

    void drop() { len = 0; }

private:
    std::unique_ptr<uint8_t[]> data;
    int64_t start = 0;
    uint32_t len = 0;
    uint32_t generation = 0;
};

#endif
