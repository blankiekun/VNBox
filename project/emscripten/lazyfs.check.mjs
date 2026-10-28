// Self-check of lazyfs.js's ranged reads: whole range plus read-ahead in one read, the block
// cache, and a file shorter than listed. Run: node lazyfs.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
globalThis.window = globalThis;
const L = new Function(fs.readFileSync(new URL('./lazyfs.js', import.meta.url), 'utf8') +
    '\nreturn { lazyCopy, lazyCache, lazyFiles, lazyReadvAsync, lazyStats, LAZY_BLOCK, LAZY_READAHEAD, LAZY_RUN_MAX };')();
const B = L.LAZY_BLOCK, size = 10 * (1 << 20) + 123;
const file = Uint8Array.from({ length: size }, (_, i) => (i * 7) & 255);
const reads = [];
const readRun = (f, start, length) => { reads.push([start, length]); return file.slice(start, start + length); };
const node = { lazyFile: 0, lazySize: size };
const get = (pos, len) => { const out = new Uint8Array(len); return [L.lazyCopy(node, out, 0, len, pos, readRun), out]; };

let [n, out] = get(B + 5, 3 * B); // one read: block 1 to the end of block 4, no read-ahead yet
assert.equal(n, 3 * B);
assert.deepEqual(out, file.slice(B + 5, 4 * B + 5));
assert.deepEqual(reads, [[B, 4 * B]]);
get(5 * B, 10); // sequential: read-ahead
assert.deepEqual(reads[1], [5 * B, B + L.LAZY_READAHEAD]);
[n] = get(2 * B, 8 * B); // cached, nothing read
assert.equal(n, 8 * B);
assert.equal(reads.length, 2);
reads.length = 0; L.lazyCache.clear();
[n, out] = get(0, 8 << 20); // longer than LAZY_RUN_MAX: several runs, no gaps
assert.equal(n, 8 << 20);
assert.deepEqual(out, file.slice(0, 8 << 20));
assert.ok(reads.every(([, len]) => len <= L.LAZY_RUN_MAX) && reads.length === 2, JSON.stringify(reads));
[n] = get(size - 100, 1000); // past the end
assert.equal(n, 100);
reads.length = 0; L.lazyCache.clear();
const short = { lazyFile: 1, lazySize: size }; // the file shrank to 1.5 blocks: stops, no loop
const shortRun = (f, start, length) => { reads.push([start, length]); return file.slice(start, Math.min(start + length, B * 1.5)); };
assert.equal(L.lazyCopy(short, new Uint8Array(4 * B), 0, 4 * B, 0, shortRun), B * 1.5);
assert.equal(reads.length, 1);
const pre = { lazySize: 10, lazyBytes: file.slice(0, 10) }; // read whole at mount
const o = new Uint8Array(4);
assert.equal(L.lazyCopy(pre, o, 0, 4, 8, null), 2);
assert.deepEqual(o.subarray(0, 2), file.slice(8, 10));
// jspi readv: a promise (a suspension) only on a miss, the count itself on cached blocks
globalThis.HEAPU8 = new Uint8Array(1 << 16); globalThis.HEAPU32 = new Uint32Array(HEAPU8.buffer);
L.lazyFiles[2] = new Blob([file.slice(0, 4 * B)]);
const stream = { node: { lazyFile: 2, lazySize: 4 * B }, position: 0 };
HEAPU32.set([1024, 100, 2048, 50]); // two iovecs
const p = L.lazyReadvAsync(stream, 0, 2);
assert.ok(p instanceof Promise);
assert.equal(await p, 150);
assert.equal(L.lazyReadvAsync(stream, 0, 2), 150);
assert.equal(stream.position, 300);
assert.deepEqual(HEAPU8.subarray(2048, 2098), file.slice(250, 300));
assert.equal(L.lazyStats.suspends, 1);
console.log('lazyfs self-check ok');
