// Self-check of lazyfs.js: ranged reads (whole range plus read-ahead in one read, the block cache,
// a file shorter than listed) and what it changes in IDBFS for saves. Run: node lazyfs.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
globalThis.window = globalThis;
const L = new Function(fs.readFileSync(new URL('./lazyfs.js', import.meta.url), 'utf8') +
    '\nreturn { lazyCopy, lazyCache, lazyUncache, LAZY_BLOCK, LAZY_READAHEAD, LAZY_RUN_MAX, LAZY_EAGER_MAX, LAZY_EAGER_BUDGET, lazyEagerTake, lazyEscapeName, lazyGameName, lazyOverlayPath, lazyPatchIdbfs, vnboxStoreSaves, mountLazyAppJspi, lazyMountOverlay, lazyFiles, lazyHandles };')();
const B = L.LAZY_BLOCK, size = 10 * (1 << 20) + 123;
const file = Uint8Array.from({ length: size }, (_, i) => (i * 7) & 255);
const reads = [];
const readRun = (f, start, length) => { reads.push([start, length]); return file.slice(start, start + length); };
const node = { lazyFile: 0, lazySize: size };
const clearCache = () => { reads.length = 0; [...L.lazyCache.keys()].forEach(L.lazyUncache); };
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
clearCache();
[n, out] = get(0, 8 << 20); // longer than LAZY_RUN_MAX: several runs, no gaps
assert.equal(n, 8 << 20);
assert.deepEqual(out, file.slice(0, 8 << 20));
assert.ok(reads.every(([, len]) => len <= L.LAZY_RUN_MAX) && reads.length === 2, JSON.stringify(reads));
[n] = get(size - 100, 1000); // past the end
assert.equal(n, 100);
clearCache();
const short = { lazyFile: 1, lazySize: size }; // the file shrank to 1.5 blocks: stops, no loop
const shortRun = (f, start, length) => { reads.push([start, length]); return file.slice(start, Math.min(start + length, B * 1.5)); };
assert.equal(L.lazyCopy(short, new Uint8Array(4 * B), 0, 4 * B, 0, shortRun), B * 1.5);
assert.equal(reads.length, 1);
const pre = { lazySize: 10, lazyBytes: file.slice(0, 10) }; // read whole at mount
const o = new Uint8Array(4);
assert.equal(L.lazyCopy(pre, o, 0, 4, 8, null), 2);
assert.deepEqual(o.subarray(0, 2), file.slice(8, 10));
// Cached blocks are views of their run, and a long read caches only its last LAZY_RUN_KEEP: the bug copied
// every block of an 8 MiB read and flushed the hot blocks of every other file from the cache.
clearCache();
const hot = { lazyFile: 5, lazySize: size };
L.lazyCopy(hot, new Uint8Array(10), 0, 10, 0, readRun);
L.lazyCopy({ lazyFile: 6, lazySize: size }, new Uint8Array(8 << 20), 0, 8 << 20, 0, readRun);
reads.length = 0;
assert.equal(L.lazyCopy(hot, new Uint8Array(10), 0, 10, 0, readRun), 10);
assert.equal(reads.length, 0);
L.lazyCopy({ lazyFile: 7, lazySize: size }, new Uint8Array(2 * B), 0, 2 * B, 0, readRun);
assert.equal(L.lazyCache.get('7:0').buffer, L.lazyCache.get('7:1').buffer);
// jspi: a sequential run starts reading the next one in the background, and the miss there takes it instead of
// waiting for a new read. A failed one is read again.
reads.length = 0;
let failAt = 16 * B;
const asyncRun = (f, start, length) => {
    reads.push([start, length]);
    return start === failAt ? Promise.reject(new Error('stale')) : Promise.resolve(file.slice(start, start + length));
};
const seq = { lazyFile: 8, lazySize: size };
const getAsync = async (pos, len) => { const out = new Uint8Array(len); return [await L.lazyCopy(seq, out, 0, len, pos, asyncRun), out]; };
await getAsync(0, B); // not sequential yet: nothing more
await getAsync(B, B); // to the end of block 1 plus read-ahead, then the next run of that length
assert.deepEqual(reads, [[0, B], [B, 5 * B], [6 * B, 5 * B]]);
[n, out] = await getAsync(6 * B, 2 * B);
assert.equal(n, 2 * B);
assert.deepEqual(out, file.slice(6 * B, 8 * B));
assert.deepEqual(reads.slice(3), [[11 * B, 5 * B]]);
await getAsync(8 * B, 8 * B); // takes [11, 16) and starts [16, 21), which fails
failAt = -1;
[n, out] = await getAsync(16 * B, B);
assert.deepEqual(out, file.slice(16 * B, 17 * B));
assert.deepEqual(reads.slice(4), [[16 * B, 5 * B], [16 * B, 5 * B], [21 * B, 5 * B]]);

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

// Mount opens the folder's files several at a time, keeping the listing order, the indexes and the eager budget.
// The bug: one await per file, seconds before the first frame for a folder of thousands.
let opening = 0, mostOpen = 0;
const fileHandle = size => ({ kind: 'file', getFile: async () => {
    mostOpen = Math.max(mostOpen, ++opening);
    await new Promise(resolve => setImmediate(resolve));
    opening--;
    return { size, arrayBuffer: async () => new Uint8Array(size).fill(size & 255).buffer };
} });
const dirHandle = list => ({ kind: 'directory', async *entries() { yield* list; } });
const tooBig = L.LAZY_EAGER_MAX + 1, many = Array.from({ length: 40 }, (_, i) => [`f${i}`, fileHandle(i === 1 ? tooBig : 10 + i)]);
const mounted = [], fsStub = globalThis.FS;
globalThis.FS = {
    mkdirTree() { }, analyzePath: () => ({ exists: false }),
    createFile: (parent, name) => { const n = { node_ops: {}, stream_ops: {} }; mounted.push([parent + '/' + name, n]); return n; },
};
await L.mountLazyAppJspi('/lazyapp/j', 'g', dirHandle([['d', dirHandle([['in', fileHandle(5)]])], ...many]));
globalThis.FS = fsStub;
assert.ok(mostOpen > 1, 'files opened one at a time');
assert.deepEqual(mounted.map(([p]) => p), ['/lazyapp/j/g/d/in', ...many.map(([name]) => '/lazyapp/j/g/' + name)]);
assert.deepEqual(mounted.map(([, n]) => [n.lazyFile, n.lazySize]), [[0, 5], ...many.map((_, i) => [i + 1, i === 1 ? tooBig : 10 + i])]);
assert.deepEqual(mounted.map(([, n]) => n.lazyBytes && n.lazyBytes[0]), [5, ...many.map((_, i) => i === 1 ? null : 10 + i)]);
assert.deepEqual(L.lazyFiles.map(f => f.size), mounted.map(([, n]) => n.lazySize));
assert.equal(L.lazyHandles.length, 41);

// The budget must stop eager reads for good: 40,000 small files all held in memory is the bug.
assert.ok(!L.lazyEagerTake(L.LAZY_EAGER_MAX + 1));
let left = L.LAZY_EAGER_BUDGET - mounted.reduce((sum, [, n]) => sum + (n.lazyBytes ? n.lazySize : 0), 0); // less what the mount above took
for (; left >= L.LAZY_EAGER_MAX; left -= L.LAZY_EAGER_MAX) assert.ok(L.lazyEagerTake(L.LAZY_EAGER_MAX));
assert.ok(L.lazyEagerTake(left));
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
// A wrapper removes the page once this says true, so it must not while a save is only in memory.
// The bug: the page went away with a persist running or waiting up to 30 s for its retry, and the save with it.
timers.length = 0;
m.lazyRetry = 0;
m.idbPersistState = 0;
let stored = 'none';
const storeSaves = ms => { stored = 'none'; L.vnboxStoreSaves(ms).then(v => { stored = v; }); };
const settled = () => new Promise(resolve => setImmediate(resolve));
storeSaves(0);
await settled();
assert.equal(stored, true); // nothing pending: no wait
m.idbPersistState = 'idb';
storeSaves(1e6);
await settled();
assert.equal(stored, 'none');
m.idbPersistState = 0;
timers.pop()[0]();
await settled();
assert.equal(stored, true);
// Waiting on a failed persist's retry: it goes at once, and the answer waits for it.
fail(new Error('closed'));
assert.ok(m.lazyRetry);
timers.length = 0;
const asked = persists.length;
const queuePersist = IDBFS.queuePersist;
IDBFS.queuePersist = mount => { persists.push(mount); mount.idbPersistState = 'idb'; };
storeSaves(1e6);
await settled();
assert.equal(persists.length, asked + 1);
assert.equal(m.lazyRetry, 0);
assert.equal(stored, 'none');
m.idbPersistState = 0;
timers.pop()[0]();
await settled();
assert.equal(stored, true);
IDBFS.queuePersist = queuePersist;
// Time out with the save still only in memory: says so.
m.idbPersistState = 'idb';
storeSaves(0);
await settled();
assert.equal(stored, false);
m.idbPersistState = 0;
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
// A persist lists the files the game wrote: IDBFS's own walk less the shipped files, without a path
// lookup per game file (a hitch on every save, tens of thousands of files).
const getattr = n => ({ mtime: new Date(n.mtime) });
const dirNode = (mtime, contents) => ({ mode: 1, mtime, contents, node_ops: { getattr } });
const fileNode = (mtime, pristine) => ({ mode: 0, mtime, lazyPristine: pristine || undefined, node_ops: { getattr } });
const tree = dirNode(1, { g: dirNode(2, { 'ship.dat': fileNode(3, true), 'save.dat': fileNode(4), data: dirNode(5, { 'a.dat': fileNode(6, true) }) }), top: fileNode(7) });
const nodeAt = p => p.split('/').slice(3).reduce((n, part) => n && n.contents && n.contents[part], tree);
const lazyMount = { mountpoint: '/lazyapp/x', root: tree };
let pathLookups = 0;
globalThis.FS = { analyzePath: p => (pathLookups++, { object: nodeAt(p) }), isDir: mode => mode === 1, isLink: () => false, mkdirTree() { }, mount: () => ({ mount: lazyMount }) };
IDBFS.getLocalSet = (mount, callback) => { // libidbfs.js's walk: an lstat per path
    const entries = {}, check = Object.keys(tree.contents).map(n => mount.mountpoint + '/' + n);
    while (check.length) {
        const p = check.pop(), n = nodeAt(p);
        pathLookups++;
        if (n.mode === 1) check.push(...Object.keys(n.contents).map(c => p + '/' + c));
        entries[p] = { timestamp: getattr(n).mtime };
    }
    callback(null, { type: 'local', entries });
};
L.lazyMountOverlay('/lazyapp/x');
let localSet;
IDBFS.getLocalSet(lazyMount, (err, set) => { assert.equal(err, null); localSet = set; });
assert.deepEqual(localSet, { type: 'local', entries: {
    '/lazyapp/x/g': { timestamp: new Date(2) }, '/lazyapp/x/g/save.dat': { timestamp: new Date(4) },
    '/lazyapp/x/g/data': { timestamp: new Date(5) }, '/lazyapp/x/top': { timestamp: new Date(7) },
} });
assert.equal(pathLookups, 0);
console.error = realError; console.warn = realWarn; globalThis.setTimeout = realTimeout;
console.log('lazyfs self-check ok');
