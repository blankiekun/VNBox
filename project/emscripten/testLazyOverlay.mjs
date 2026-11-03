// Real-browser test of the game folder and its saves (lazyfs.js) on real OPFS, IndexedDB, IDBFS and MEMFS, with no
// Wine and no game: it loads a built engine's boxedwine.js without running it. lazyfs.check.mjs covers the logic with
// IDBFS stubbed; this catches what only the real parts show, such as an Emscripten bump changing IDBFS.
//   node testLazyOverlay.mjs <chromium|firefox> <Build/JitJspi folder>      (a jitJspi build; PLAYWRIGHT = where to find playwright)
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';

const [browser = 'chromium', build = new URL('./Build/JitJspi', import.meta.url).pathname] = process.argv.slice(2);
const { [browser]: engine } = createRequire(process.env.PLAYWRIGHT ? path.resolve(process.env.PLAYWRIGHT) + '/' : import.meta.url)('playwright');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const GOOD = Buffer.from('a good zip'), BAD = Buffer.from('<html>a proxy said hello</html>');
const MOUNT = '/lazyapp/test', GAME = MOUNT + '/g';
const PAGE = `<!doctype html><meta charset=utf-8><canvas id=canvas></canvas>
<script>var Module = { noInitialRun: true, canvas: document.getElementById('canvas'), print() { }, printErr() { }, onRuntimeInitialized() { window.ready = true; } };</script>
<script src=lazyfs.js></script><script src=boxedwine.js></script>`;

// What the server answers: fresh = the request bypassed the HTTP cache (cache: "reload").
const site = { sums: () => sha(GOOD) + '  z.zip\n', zip: () => GOOD, zipRequests: 0 };
const server = http.createServer((req, res) => {
    const name = decodeURIComponent(new URL(req.url, 'http://x').pathname.slice(1)), fresh = req.headers['cache-control'] === 'no-cache';
    if (name === 'test.html') return res.writeHead(200, { 'content-type': 'text/html' }).end(PAGE);
    if (name === 'vnbox-zips.sha256') return res.writeHead(200).end(site.sums(fresh));
    if (name === 'z.zip') { site.zipRequests++; return res.writeHead(200).end(site.zip(fresh)); }
    const file = path.join(build, name);
    if (!fs.existsSync(file)) return res.writeHead(404).end();
    res.writeHead(200, { 'content-type': name.endsWith('.js') ? 'text/javascript' : name.endsWith('.wasm') ? 'application/wasm' : 'text/plain' }).end(fs.readFileSync(file));
});
await new Promise(ok => server.listen(0, '127.0.0.1', ok));
const origin = 'http://127.0.0.1:' + server.address().port;

const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'lazy-overlay-'));
process.on('exit', () => fs.rmSync(profile, { recursive: true, force: true }));
const context = await engine.launchPersistentContext(profile, {
    headless: true,
    firefoxUserPrefs: browser === 'firefox' ? { 'javascript.options.wasm_js_promise_integration': true } : undefined,
});
await context.addInitScript(() => {
    window.idbDeletes = [];
    const remove = IDBObjectStore.prototype.delete;
    IDBObjectStore.prototype.delete = function (key) { window.idbDeletes.push(String(key)); return remove.call(this, key); };
    window.reports = [];
    const error = console.error;
    console.error = (...a) => { window.reports.push(a.join(' ')); error(...a); };
});

async function newPage() {
    const page = await context.newPage();
    page.on('pageerror', e => console.log('page error: ' + e));
    await page.goto(origin + '/test.html');
    await page.waitForFunction(() => window.ready, null, { polling: 100 });
    await page.evaluate(() => {
        window.ops = {
            mount: () => { lazyPatchIdbfs(); return mountLazyApp(GAME_MOUNT, 'g', lazyOpfsDir('t/g')); },
            persist: () => new Promise((ok, no) => FS.syncfs(false, e => e ? no(e) : ok())),
            text: p => { try { return FS.readFile(p, { encoding: 'utf8' }); } catch (e) { return 'errno ' + e.errno; } },
            exists: p => FS.analyzePath(p).exists,
            cacheNames: async () => { const out = []; for await (const k of (await (await navigator.storage.getDirectory()).getDirectoryHandle('lazyfs-cache', { create: true })).keys()) out.push(k); return out.sort(); },
            lazyText: async (p, n) => { const node = FS.lookupPath(p).node, out = new Uint8Array(n); const got = await lazyCopy(node, out, 0, n, 0, lazyReadRunAsync); return new TextDecoder().decode(out.subarray(0, got)); },
            replace: async (name, text) => { const dir = await lazyOpfsDir('t/g'), w = await (await dir.getFileHandle(name, { create: true })).createWritable(); await w.write(text); await w.close(); },
            remove: async name => (await lazyOpfsDir('t/g')).removeEntry(name),
        };
    });
    await page.evaluate(m => { window.GAME_MOUNT = m; }, MOUNT);
    return page;
}
const closeAndReopen = async page => { await page.close(); return newPage(); };

// The shipped game: small files are held in memory at mount, big ones are lazy nodes read from OPFS.
const BIG = 2 << 20;
let page = await newPage();
await page.evaluate(async big => {
    const root = await navigator.storage.getDirectory(), t = await root.getDirectoryHandle('t', { create: true }), g = await t.getDirectoryHandle('g', { create: true });
    const put = async (dir, name, data) => { const w = await (await dir.getFileHandle(name, { create: true })).createWritable(); await w.write(data); await w.close(); };
    await put(g, 'ship_trunc.dat', 'x'.repeat(1000));
    await put(g, 'ship_ren.dat', 'shipped-ren');
    await put(g, 'セーブ.dat', 'shipped-save');
    await put(g, 'big.dat', 'B'.repeat(big));
    await put(g, 'big_stale.dat', 'S'.repeat(big));
    await put(g, 'big_gone.dat', 'G'.repeat(big));
    await put(g, 'データ.dat', 'nfd-file'); // a name in NFD, as macOS stores it
    await put(await g.getDirectoryHandle('sub', { create: true }), 'inner.dat', 'inner');
}, BIG);

// Saves: what a game does, then a real reload. A persist that deleted or skipped files would lose saves here.
await page.evaluate(() => ops.mount());
await page.evaluate(async game => {
    for (let i = 0; i < 200; i++) FS.writeFile(`${game}/save${i}.dat`, 'save ' + i);
    FS.truncate(game + '/ship_trunc.dat', 3);
    FS.rename(game + '/ship_ren.dat', game + '/ship_ren.bak');
    FS.rename(game + '/sub', game + '/moved');
    FS.writeFile(game + '/big.dat', 'overwritten');
    const escaped = lazyEscapeName('セーブ.dat'); // the spelling Boxedwine gives a name the game creates (Fs::localNameToRemote)
    FS.writeFile(`${game}/tmp`, 'new save'); FS.rename(`${game}/tmp`, `${game}/${escaped}`);
    await ops.persist();
}, GAME);
page = await closeAndReopen(page);
await page.evaluate(() => ops.mount());
let after = await page.evaluate(game => ({
    deletes: window.idbDeletes,
    saves: Array.from({ length: 200 }, (_, i) => ops.text(`${game}/save${i}.dat`) === 'save ' + i).filter(Boolean).length,
    trunc: FS.stat(game + '/ship_trunc.dat').size,
    bak: ops.text(game + '/ship_ren.bak'),
    moved: ops.text(game + '/moved/inner.dat'),
    big: ops.text(game + '/big.dat'),
    save: ops.text(`${game}/${lazyEscapeName('セーブ.dat')}`),
    raw: ops.exists(game + '/セーブ.dat'),
}), GAME);
assert.deepEqual(after.deletes, [], 'a start must not delete saved files from IndexedDB');
assert.equal(after.saves, 200, 'every overlay file reads back');
assert.equal(after.trunc, 3, 'a truncated file stays truncated');
assert.equal(after.bak, 'shipped-ren', 'a renamed pristine file survives');
assert.equal(after.moved, 'inner', 'a file of a renamed pristine folder survives');
assert.equal(after.big, 'overwritten', 'a big lazy file written over keeps the new bytes');
assert.equal(after.save, 'new save', 'a replaced non-ASCII file reads the new bytes');
assert.equal(after.raw, false, 'the shipped spelling must not hide the save');

// Finding 34: a game asks for the NFC name, which comes from Shift-JIS.
assert.equal(await page.evaluate(g => ops.text(g + '/データ.dat'), GAME), 'nfd-file');

// Finding 33: another tab re-copied the game. The next read works, with a fresh File and no report.
assert.equal(await page.evaluate(async g => { await ops.replace('big_stale.dat', 'N'.repeat(1 << 21)); return (await ops.lazyText(g + '/big_stale.dat', 4)); }, GAME), 'NNNN');
assert.deepEqual(await page.evaluate(() => window.reports), []);

// Finding 29: the first write to a lazy file whose File cannot be read is a failed write, not an exception that kills the tick.
const copyUp = await page.evaluate(async g => {
    await ops.remove('big_gone.dat');
    try { const s = FS.open(g + '/big_gone.dat', 'r+'); FS.write(s, new Uint8Array(1), 0, 1, 0); return 'wrote'; } catch (e) { return e.name + ' ' + e.errno; }
}, GAME);
assert.equal(copyUp, 'ErrnoError 29', 'copy-up of an unreadable file');
await page.evaluate(() => { window.reports.length = 0; });

// Saves are still all there before the zip part starts, and it must leave them alone.
const saved = () => page.evaluate(async m => {
    const db = await new Promise((ok, no) => { const r = indexedDB.open(m); r.onsuccess = () => ok(r.result); r.onerror = () => no(r.error); });
    const n = await new Promise(ok => { const c = db.transaction(['FILE_DATA']).objectStore('FILE_DATA').count(); c.onsuccess = () => ok(c.result); });
    db.close(); return n;
}, MOUNT);
await page.evaluate(() => ops.persist());
const savedBefore = await saved();
assert.ok(savedBefore >= 200, 'saves in IndexedDB: ' + savedBefore);

// Finding 28. Every case runs in a new page, as a boot does. add: lazyAddCachedFile; its result: the bytes as read back, or the error.
const add = () => page.evaluate(async () => {
    try {
        await lazyAddCachedFile('/', 'z.zip', 'z.zip');
        return await lazyFiles[FS.lookupPath('/z.zip').node.lazyFile].text();
    } catch (e) { return 'failed: ' + e.message; }
});
const copy = sha(GOOD) + '~z.zip';
// A poisoned copy from before the check, as a proxy left it: replaced, and checked once only.
await page.evaluate(async name => { const c = await (await navigator.storage.getDirectory()).getDirectoryHandle('lazyfs-cache', { create: true }), w = await (await c.getFileHandle(name, { create: true })).createWritable(); await w.write('poison'); await w.close(); }, copy);
assert.equal(await add(), GOOD.toString(), 'a poisoned cached copy is replaced');
const requests = site.zipRequests;
page = await closeAndReopen(page);
assert.equal(await add(), GOOD.toString());
assert.equal(site.zipRequests, requests, 'a good copy is not downloaded again');
// A wrong answer whose HTTP cache entry is what stays wrong: the second try skips that cache.
await page.evaluate(async () => (await navigator.storage.getDirectory()).removeEntry('lazyfs-cache', { recursive: true }));
site.zip = fresh => fresh ? GOOD : BAD;
page = await closeAndReopen(page);
assert.equal(await add(), GOOD.toString(), 'a stale HTTP cache entry is bypassed');
// The list of sums itself out of date: read again before calling the zip bad.
await page.evaluate(async () => (await navigator.storage.getDirectory()).removeEntry('lazyfs-cache', { recursive: true }));
site.zip = () => GOOD;
site.sums = fresh => (fresh ? sha(GOOD) : sha(BAD)) + '  z.zip\n';
page = await closeAndReopen(page);
assert.equal(await add(), GOOD.toString(), 'an out of date list of sums is read again');
site.sums = () => sha(GOOD) + '  z.zip\n';
// Always wrong: the boot fails with a reason, the copy does not stay, and a later good answer heals it with no user action.
await page.evaluate(async () => (await navigator.storage.getDirectory()).removeEntry('lazyfs-cache', { recursive: true }));
site.zip = () => BAD;
page = await closeAndReopen(page);
assert.match(await add(), /^failed: .*(checksum|sha)/i);
assert.deepEqual(await page.evaluate(() => ops.cacheNames()), [], 'a wrong zip must not stay');
site.zip = () => GOOD;
page = await closeAndReopen(page);
assert.equal(await add(), GOOD.toString(), 'the next start downloads it again');
// Finding 33: a new release must not delete the zip another tab still reads; the next start after that tab closed does.
const oldBytes = 'old release zip', oldCopy = sha(Buffer.from(oldBytes)) + '~z.zip';
await page.close();
site.sums = () => sha(Buffer.from(oldBytes)) + '  z.zip\n'; site.zip = () => Buffer.from(oldBytes);
const oldTab = await newPage();
const oldAdd = () => oldTab.evaluate(async () => { await lazyAddCachedFile('/', 'z.zip', 'z.zip'); return lazyFiles[FS.lookupPath('/z.zip').node.lazyFile].text(); });
assert.equal(await oldAdd(), oldBytes);
site.sums = () => sha(GOOD) + '  z.zip\n'; site.zip = () => GOOD;
page = await newPage();
assert.equal(await add(), GOOD.toString(), 'the new release');
assert.ok((await page.evaluate(() => ops.cacheNames())).includes(oldCopy), 'the old copy stays while a tab holds it');
assert.equal(await oldTab.evaluate(() => lazyFiles[FS.lookupPath('/z.zip').node.lazyFile].text()), oldBytes, 'the old tab still reads it');
await oldTab.close();
await new Promise(ok => setTimeout(ok, 1000)); // its lock goes a moment after the page
page = await closeAndReopen(page);
assert.equal(await add(), GOOD.toString());
assert.ok(!(await page.evaluate(() => ops.cacheNames())).some(k => k.includes(oldCopy)), 'the old copy goes once no tab holds it');
// A zip the release does not list, which the engine could not read: forgotten, so the next start downloads it.
site.sums = () => '';
await page.evaluate(async () => (await navigator.storage.getDirectory()).removeEntry('lazyfs-cache', { recursive: true }));
site.zip = () => BAD;
page = await closeAndReopen(page);
assert.equal(await add(), BAD.toString(), 'nothing to compare an unlisted zip with');
await page.evaluate(() => lazyForgetUnlistedZip('z.zip'));
assert.deepEqual(await page.evaluate(() => ops.cacheNames()), []);

assert.equal(await saved(), savedBefore, 'nothing done to cached zips may touch saves');
await context.close();
server.close();
console.log('lazy overlay test ok (' + browser + ')');
