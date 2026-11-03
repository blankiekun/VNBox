// Self-check of lazyfs.js: ranged reads (whole range plus read-ahead in one read, the block cache,
// a file shorter than listed) and what it changes in IDBFS for saves. Run: node lazyfs.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
globalThis.window = globalThis;
const L = new Function(fs.readFileSync(new URL('./lazyfs.js', import.meta.url), 'utf8') +
    '\nreturn { lazyCopy, lazyCache, LAZY_BLOCK, LAZY_READAHEAD, LAZY_RUN_MAX, LAZY_EAGER_MAX, LAZY_EAGER_BUDGET, lazyEagerTake, lazyEscapeName, lazyGameName, lazyOverlayPath, lazyPatchIdbfs };')();
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

// A different spelling than Fs::localNameToRemote (fs.cpp) hides the game's safe-saved file behind the shipped one.
assert.equal(L.lazyEscapeName('\u30bb@a. b.'), '(_xE3_)(_x82_)(_xBB_)(_at_)a. b(_dot_)');
assert.equal(L.lazyEscapeName('a '), 'a(_space_)');
assert.equal(L.lazyEscapeName('plain.dat'), 'plain.dat');
// The overlay's spelling wins at every path part; plain names cost no lookup (40,000 game files).
const overlay = new Set(['/g/(_at_)d', '/g/(_at_)d/(_at_)f', '/g/@raw', '/g/(_at_)raw']);
const lookups = [];
globalThis.FS = { analyzePath: p => (lookups.push(p), { exists: overlay.has(p) }), isDir: mode => mode === 1, ErrnoError: class extends Error { name = 'ErrnoError'; } };
assert.equal(L.lazyOverlayPath('/g', '@d/@f'), '/g/(_at_)d/(_at_)f');
assert.equal(L.lazyOverlayPath('/g', '@d/@new'), '/g/(_at_)d/@new');
assert.equal(L.lazyOverlayPath('/g', '@raw'), '/g/@raw');
lookups.length = 0;
assert.equal(L.lazyOverlayPath('/g', 'a/b.dat'), '/g/a/b.dat');
assert.equal(lookups.length, 0);

// A game asks for the NFC name, from Shift-JIS; a folder from macOS has NFD (te + U+3099), which Wine does not match.
const nfd = '\u30c6\u3099', nfc = '\u30c7';
overlay.clear();
assert.equal(L.lazyOverlayPath('/g', `${nfd}/${nfd}.dat`), `/g/${nfc}/${nfc}.dat`);
// A compatibility ideograph is not NFD: Wine's CP932 asks for U+FA10 itself, and NFC would turn it into U+585A.
assert.equal(L.lazyGameName('\ufa10.dat'), '\ufa10.dat');
// A save made under the folder's NFD spelling stays visible: pointing past it would bring the shipped file back.
overlay.add(`/g/${nfd}`);
assert.equal(L.lazyOverlayPath('/g', `${nfd}/a`), `/g/${nfd}/a`);
// The NFC spelling wins once a game made it: the escaped one is how Fs::localNameToRemote spells a created name.
overlay.add(`/g/${L.lazyEscapeName(nfc)}`);
assert.equal(L.lazyOverlayPath('/g', `${nfd}/a`), `/g/${L.lazyEscapeName(nfc)}/a`);

// The budget must stop eager reads for good: 40,000 small files all held in memory is the bug.
assert.ok(!L.lazyEagerTake(L.LAZY_EAGER_MAX + 1));
for (let left = L.LAZY_EAGER_BUDGET; left > 0; left -= L.LAZY_EAGER_MAX) assert.ok(L.lazyEagerTake(L.LAZY_EAGER_MAX));
assert.ok(!L.lazyEagerTake(1));

// IDBFS with its IndexedDB side stubbed: persists and syncs are recorded, the callbacks run by hand.
const persists = [], syncs = [], reports = [], timers = [];
const realError = console.error, realWarn = console.warn, realTimeout = globalThis.setTimeout;
console.error = m => reports.push(m); console.warn = () => { };
globalThis.setTimeout = (f, ms) => (timers.push([f, ms]), timers.length);
let closed = 0;
const memSetattr = (n, a) => { if (a.size !== undefined) n.size = a.size; }, renames = [];
const memRoot = { node_ops: { setattr: memSetattr, rename: (n, d, name) => renames.push([n, name]) } };
globalThis.IDBFS = {
    dbs: {}, queuePersist: m => persists.push(m), syncfs: (m, populate, done) => syncs.push(done),
    reconcile() { throw new Error('closed'); }, mount: () => memRoot,
};
L.lazyPatchIdbfs();
const m = { mountpoint: '/m', opts: { autoPersist: true } };
IDBFS.mount(m);
// A persist during a restore deletes the files not restored yet; a failed restore must not persist at all.
let result = 'none';
IDBFS.syncfs(m, true, e => { result = e; });
IDBFS.queuePersist(m);
assert.equal(persists.length, 0);
syncs.pop()(new Error('bad value'));
IDBFS.queuePersist(m);
assert.equal(persists.length, 0);
IDBFS.syncfs(m, true, e => { result = e; });
syncs.pop()(null);
IDBFS.queuePersist(m);
assert.equal(persists.length, 1);
assert.equal(result, null);
// A reconcile that throws must still answer, or the persist state waits forever.
let answered = null;
IDBFS.reconcile({}, {}, e => { answered = e; });
assert.equal(answered.message, 'closed');
// A failed persist is retried on a new connection; warned from the second failure, once, and not for a file that just went away.
const fail = err => { IDBFS.dbs['/m'] = { close() { closed++; } }; IDBFS.syncfs(m, false, () => { }); syncs.pop()(err); };
fail(Object.assign(new Error('gone'), { name: 'ErrnoError' }));
assert.equal(reports.length, 0);
assert.equal(closed, 1);
assert.equal(IDBFS.dbs['/m'], undefined);
assert.equal(timers.length, 1);
timers.pop()[0]();
assert.equal(persists.length, 2);
fail(new Error('QuotaExceededError'));
assert.equal(reports.length, 0);
fail(new Error('QuotaExceededError'));
fail(new Error('QuotaExceededError'));
assert.equal(reports.length, 1);
assert.match(reports[0], /QuotaExceededError/);
assert.equal(timers.length, 1);
timers.pop();
m.lazyRetry = 0;
IDBFS.syncfs(m, false, () => { }); syncs.pop()(null);
fail(new Error('again')); fail(new Error('again'));
assert.equal(reports.length, 2);
// The player's warning clears on this message, so it must wait for every store that failed: saves are still only in memory otherwise.
const posted = [];
globalThis.parent = { postMessage: p => posted.push(p) };
globalThis.location = { origin: 'o' };
const other = { mountpoint: '/other', lazyPersistFails: 0 };
const persistOther = err => { IDBFS.syncfs(other, false, () => { }); syncs.pop()(err); }, persistM = () => { IDBFS.syncfs(m, false, () => { }); syncs.pop()(null); };
persistOther(new Error('full')); persistOther(new Error('full'));
assert.equal(posted.at(-1).source, 'saves');
const reported = posted.length;
persistM();
assert.equal(posted.length, reported);
persistOther(null);
assert.deepEqual(posted.at(-1), { type: 'vnbox-saves-ok' });
assert.equal(posted.length, reported + 1);
persistM(); // nothing was failing: no message
assert.equal(posted.length, reported + 1);
delete globalThis.parent; delete globalThis.location;
// A truncate must change the time even within the same millisecond, or the persist skips the file.
const ops = memRoot.node_ops, future = Date.now() + 1e6, cut = { mtime: future };
const before = persists.length;
ops.setattr(cut, { size: 3, timestamp: 0 });
assert.equal(cut.mtime, future + 1);
assert.equal(cut.size, 3);
assert.equal(persists.length, before + 1);
ops.setattr(cut, { mode: 1 });
assert.equal(persists.length, before + 1);
// A renamed pristine file stays out of the persist, so it is gone after a reload: renames copy it first, also inside a folder.
const pristine = { lazyPristine: true, lazySize: 2, lazyBytes: new Uint8Array([7, 8]), usedBytes: 2, mode: 0 };
const dir = { mode: 1, contents: { sub: { mode: 1, contents: { f: pristine } } } };
ops.rename(dir, {}, 'new');
assert.equal(pristine.lazyPristine, undefined);
assert.deepEqual(pristine.contents, new Uint8Array([7, 8]));
assert.deepEqual(renames.at(-1), [dir, 'new']);
// A copy that fails (too big for memory) fails the rename instead of moving a file that would be lost.
const big = { name: 'big', lazyPristine: true, lazySize: 2, lazyBytes: null, lazyReadSync() { throw new RangeError('no memory'); }, mode: 0 };
const count = renames.length;
assert.throws(() => ops.rename(big, {}, 'x'), { name: 'ErrnoError' });
assert.equal(renames.length, count);
console.error = realError; console.warn = realWarn; globalThis.setTimeout = realTimeout;
console.log('lazyfs self-check ok');
