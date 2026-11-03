// Self-check of frida-compat.js's string reads, pattern parsing and trans joining, over a fake
// guest memory (the C scan is checked live at attach). Run: node frida-compat.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
new Function(fs.readFileSync(new URL('./frida-compat.js', import.meta.url), 'utf8'))();
const F = globalThis.boxedwineFrida;
const { ptr } = F;
const lines = [];
const trans = F.test.makeTrans((s) => lines.push(s));
const mem = new Uint8Array(0x3000), heap = new Uint8Array(1 << 20);
let top = 16;
const blocks = new Map(), wild = [];
F.api = { // the sizes arrive as the wasm exports get them: an i32, read as unsigned
    malloc: (n) => { n >>>= 0; if (n >= 1 << 24) return 0; const p = top; top += n; blocks.set(p, n); return p; },
    free: (p) => blocks.delete(p), heap: () => heap,
    read: (pid, addr, buf, len) => {
        const n = Math.max(0, Math.min(len >>> 0, 0x2000 - addr)); // 0x2000+ unreadable
        if (!(n <= blocks.get(buf))) { wild.push([buf, n]); return n; }
        heap.set(mem.subarray(addr, addr + n), buf); return n;
    },
};
mem.set([0x93, 0xfa, 0x96, 0x7b, 0x5e, 0x6e, 0], 0x100); // 日本^n in Shift-JIS
mem.set(new TextEncoder().encode('héllo\0'), 0x200);
mem.set([0x42, 0x30, 0x44, 0x30, 0, 0], 0x300); // あい in UTF-16LE
mem.set([0x41, 0x42], 0x1ffe); // runs into the unreadable page with no terminator
assert.equal(ptr(0x100).readShiftJisString(), '日本^n');
assert.equal(ptr(0x100).readShiftJisString(2), '日');
assert.equal(ptr(0x200).readUtf8String(), 'héllo');
assert.equal(ptr(0x300).readUtf16String(), 'あい');
assert.equal(ptr(0x1ffe).readCString(), 'AB');
mem.set(new TextEncoder().encode('x'.repeat(300) + '\0'), 0x400); // longer than the first 256-byte read
assert.equal(ptr(0x400).readCString(), 'x'.repeat(300));
assert.throws(() => ptr(0x2000).readU8(), /access violation/);
assert.equal(ptr('0x10').add(8).sub(ptr(4)).toString(), '0x14');
const { bytes, masks } = F.test.parsePattern('e8 ?? 4? : ff ff fe');
assert.deepEqual([...bytes, ...masks], [0xe8, 0, 0x40, 0xff, 0, 0xf0 & 0xfe]);
// "200+": fragments within 200 ms join with \n, a repeat of the previous one is dropped
const handler = trans.send((s) => s, '200+');
handler('a'); handler('a'); handler('b');
await new Promise((r) => setTimeout(r, 300));
assert.deepEqual(lines, ['a\nb']);
// Bug caught: a bad length (guest data, a script's junk) spoils the scratch buffer's size, and that read or a later one
// writes guest bytes at wasm address 0 or past the block (`wild`). 2^24 stands for a malloc that really fails.
mem.set([1, 2, 3, 4], 0x600);
for (const n of [-1, 2 ** 31, (1 << 24) + 1]) assert.throws(() => ptr(0x600).readByteArray(n), /bad read length/, String(n));
for (const n of [-1, 0x200000004]) assert.throws(() => F.hexdump(ptr(0x600), { length: n }), /bad read length/, String(n)); // exportsOf's count * 4
assert.throws(() => ptr(0x600).readByteArray(1 << 24), /out of memory/);
assert.deepEqual([...new Uint8Array(ptr(0x600).readByteArray(4))], [1, 2, 3, 4]);
assert.deepEqual(wild, []);
console.log('frida-compat self-check ok');
// script URL guard: same-origin blob: or under the page's parent folder, nothing else
const page = 'https://ex.org/vn/vnbox/boxedwine.html?frida=x';
for (const ok of ['blob:https://ex.org/1234', 'https://ex.org/vn/hooks/a.js', 'https://ex.org/vn/vnbox/libPCKiriKiri.js'])
    assert.ok(F.test.scriptAllowed(ok, page), ok);
for (const bad of ['blob:https://evil.org/1234', 'https://evil.org/vn/a.js', 'https://ex.org/a.js', 'https://ex.org/vnx/a.js',
    'https://ex.org/vn/../a.js', 'https://ex.org/vn/%2e%2e/a.js', 'data:text/javascript,alert(1)', 'javascript:alert(1)', 'http://ex.org/vn/a.js', '//evil.org/vn/a.js', 'https://ex.org/vn/..%2Fcalibre/a.js', 'https://ex.org/vn/..%5ca.js'])
    assert.ok(!F.test.scriptAllowed(bad, page), bad);
console.log('frida-compat url guard ok');
// chimimouryou: page /vn/vnbox.html, this file /vn/vnbox/frida-compat.js: the folder is /vn/, not the root
const embed = 'https://ex.org/vn/vnbox.html?frida=x', self = 'https://ex.org/vn/vnbox/frida-compat.js';
assert.ok(F.test.scriptAllowed('https://ex.org/vn/vnbox/hooks/a.js', embed, self));
assert.ok(!F.test.scriptAllowed('https://ex.org/calibre/x.js', embed, self));
assert.ok(!F.test.scriptAllowed('https://ex.org/vnx.js', embed, self));
assert.ok(!F.test.scriptAllowed('https://evil.org/vn/a.js', embed, self));
console.log('frida-compat embed guard ok');
// H-codes: the Textractor subset, and the script scriptFor() writes for one, run against fake Frida globals
const H = F.test.parseHcode;
assert.deepEqual(H('/HS8@6E300:PJADVJP.bin'), { type: 'S', string: true, full: false, noContext: false, encoding: 'shift_jis', padding: 0,
    offset: 8, deref: null, split: null, splitDeref: null, address: 0x6e300, module: 'PJADVJP.bin', func: null });
assert.deepEqual([H('/HW-4*14@1137D0:')].map((h) => [h.offset, h.deref, h.module, h.encoding]), [[-4, 0x14, '', 'utf-16le']]);
const q = H('HQFN-1C:-8*4@10:kernel32.dll:lstrlenW');
assert.deepEqual([q.full, q.noContext, q.offset, q.split, q.splitDeref, q.module, q.func], [true, true, -0x1c, -8, 4, 'kernel32.dll', 'lstrlenW']);
assert.deepEqual([H('/HB65001#10+4@1').padding, H('/HB65001#10+4@1').encoding, H('/HA4@401000').module], [0x10, 'utf-8', null]);
for (const [bad, why] of [['/RS@1', /read codes/], ['/HM4@1', /type "M"/], ['/HS4@', /@address/], ['/HS-3@1', /registers/],
    ['/HS936#4@1', /codepage 936/], ['/HS4<8@1', /null_length/], ['4@1', /expected \/H/], ['/HSx@1', /data offset/]])
    assert.throws(() => H(bad), why, bad);
assert.throws(() => F.loadHcode('/HM4@1'), /type "M"/);
const id = F.loadHcode('/HS4@2000:'); // not attached: kept until the game process is
assert.equal(id, 'hcode:1');
const script = F.scriptFor(id);
assert.ok(!/require\(/.test(script), 'no require()');
let hooked = null;
const fake = {
    Interceptor: { attach: (at, fn) => { hooked = { at, fn }; } }, ptr,
    Process: { enumerateModules: () => [{ name: 'game.exe', base: ptr(0x1000) }], findModuleByAddress: () => null },
    Module: {}, trans,
};
new Function(...Object.keys(fake), script)(...Object.values(fake));
assert.equal(hooked.at.toString(), '0x3000');
mem.set([0x00, 0x01, 0, 0], 0x504); // [esp + 4] = 0x100, the Shift-JIS string above
hooked.fn.call({ context: { esp: ptr(0x500) }, returnAddress: ptr(0x1234) });
hooked.fn.call({ context: { esp: ptr(0x500) }, returnAddress: ptr(0x1234) }); // a redraw: dropped
await new Promise((r) => setTimeout(r, 500));
assert.deepEqual(lines.slice(1), ['日本^n']);
assert.equal(F.unload(id), true);
assert.equal(F.version, 1);
console.log('frida-compat H-code ok');
// search thread ids: module + offset, the same across runs; the absolute address only outside any module
assert.equal(F.test.searchThreadId('GetGlyphOutlineA', 'Farthest2015.exe', 0x9cb10, '0x49cb10'), 'GetGlyphOutlineA@Farthest2015.exe+0x9cb10');
assert.equal(F.test.searchThreadId('TextOutA', undefined, 0x7f001234, '0x7f001234'), 'TextOutA@0x7f001234');
// a line that starts with a redraw of the previous one's end (最果てのイマ fades the old text out) loses that part
const joined = [], join = F.test.makeJoiner((key, text) => joined.push(text));
for (const burst of ['？？「―――」', '「―――」？？遠くから。', '遠くから。「……」', '「……」']) {
    for (const c of burst) join('k', c);
    await new Promise((r) => setTimeout(r, 450));
}
assert.deepEqual(joined, ['？？「―――」', '？？遠くから。', '「……」', '「……」']);
console.log('frida-compat joiner ok');
// Bug caught: scriptFor()'s comment line ends at a line separator (a guest module name, an H-code's), and the rest runs in the site's origin at the next boot.
let injected = false;
const evil = F.scriptFor(F.loadHcode('/HS4@2000:a\u2028injected()//'));
const fakeNamed = { ...fake, Process: { ...fake.Process, findModuleByName: () => ({ base: ptr(0x1000) }) } };
new Function('injected', ...Object.keys(fakeNamed), evil)(() => { injected = true; }, ...Object.values(fakeNamed));
assert.ok(!injected);
console.log('frida-compat scriptFor comment ok');

// A second copy of the file, as the page loads it: p= and frida= in the URL, and a fake game process.
const reports = [];
const loadPage = (url) => {
    const q = new URL(url).searchParams;
    globalThis.location = { href: url };
    globalThis.vnboxParam = (name) => q.get(name);
    globalThis.vnboxReport = (source, message) => reports.push(source + ': ' + message);
    new Function(fs.readFileSync(new URL('./frida-compat.js', import.meta.url), 'utf8'))();
    return globalThis.boxedwineFrida;
};
// Bug caught: a frida= that isn't a URL stopped the file while loading, and the emulator's first poll() threw through its wasm frames.
const badUrl = loadPage('https://ex.org/vn/vnbox/boxedwine.html?p=game.exe&frida=http://');
assert.equal(badUrl.poll(1, 1, 0x1000), 0);
assert.deepEqual(reports.splice(0), ['frida: bad frida= URL: http://']);
console.log('frida-compat bad URL ok');

const P = loadPage('https://ex.org/vn/vnbox/boxedwine.html?p=game.exe');
const TEB = 0x1000, LDR = 0x1200, HEAD = LDR + 0x0c;
const guest = new DataView(new ArrayBuffer(0x400000)), guestBytes = new Uint8Array(guest.buffer);
const heap2 = new Uint8Array(1 << 20);
let top2 = 16, readable = guestBytes.length, fakeScan, prefetched = [];
const scanAll = (pid, addr, size, pat, mask, len, out) => { // what bw_scan does
    let n = 0;
    const end = Math.min(addr + size, readable);
    for (let a = addr; a + len <= end; a++) {
        let i = 0;
        while (i < len && (guestBytes[a + i] & heap2[mask + i]) === heap2[pat + i]) i++;
        if (i === len) new DataView(heap2.buffer).setUint32(out + 4 * n++, a, true);
    }
    return n;
};
const scanWithinPages = (pid, addr, size, pat, mask, len, out) => { // the bug the self-check is there for
    let n = 0;
    const end = Math.min(addr + size, readable);
    for (let a = addr; a + len <= end; a++) {
        if ((a & 0xfff) + len > 0x1000) continue;
        let i = 0;
        while (i < len && (guestBytes[a + i] & heap2[mask + i]) === heap2[pat + i]) i++;
        if (i === len) new DataView(heap2.buffer).setUint32(out + 4 * n++, a, true);
    }
    return n;
};
P.api = {
    malloc: (n) => { const p = top2; top2 += n; return p; }, free: () => {}, heap: () => heap2,
    read: (pid, addr, buf, len) => {
        const n = Math.max(0, Math.min(len, readable - addr));
        heap2.set(guestBytes.subarray(addr, addr + n), buf);
        return n;
    },
    scan: (...a) => fakeScan(...a), prefetch: (addr, size) => prefetched.push([addr, size]), hookAdd: () => 1, hookRemove: () => {},
};
fakeScan = scanAll;
const put16 = (at, s) => { [...s].forEach((c, i) => guest.setUint16(at + 2 * i, c.charCodeAt(0), true)); };
const putString = (at, s, buffer) => { guest.setUint16(at, s.length * 2, true); guest.setUint32(at + 4, buffer, true); put16(buffer, s); };
let entryAt = 0x2000, lastEntry = HEAD;
guest.setUint32(TEB + 0x30, 0x1100, true);
guest.setUint32(0x1100 + 0x0c, LDR, true);
guest.setUint32(HEAD, HEAD, true);
guest.setUint32(HEAD + 4, HEAD, true);
const addModule = (path, base, size, exportRva = 0) => { // an InLoadOrderModuleList entry, at the end as the loader adds it
    const e = entryAt, name = path.split('\\').pop();
    entryAt += 0x400;
    guest.setUint32(e, HEAD, true);
    guest.setUint32(HEAD + 4, e, true);
    guest.setUint32(lastEntry, e, true);
    lastEntry = e;
    guest.setUint32(e + 0x18, base, true);
    guest.setUint32(e + 0x20, size, true);
    putString(e + 0x24, path, e + 0x40);
    putString(e + 0x2c, name, e + 0x200);
    guest.setUint32(base + 0x3c, 0x80, true); // PE header at 0x80, its export directory entry at +0x78
    guest.setUint32(base + 0x80 + 0x78, exportRva, true);
    guest.setUint32(base + 0x80 + 0x7c, exportRva ? 0x500 : 0, true);
};
const EXE = 0x100000, WINEDLL = 0x200000, GAMEDLL = 0x280000, LATE = 0x300000, LATEWINE = 0x340000;
guestBytes.set([0x4d, 0x5a, 0x90], EXE);
guestBytes.set([1, 2, 3, 4, 5, 6, 7, 8], EXE + 0xffc); // the self-check's match across a page boundary
addModule('C:\\launcher\\explorer.exe', EXE, 0x4000);
addModule('C:\\windows\\system32\\kernel32.dll', WINEDLL, 0x50000, 0x7000);
addModule('C:\\games\\windows\\engine.dll', GAMEDLL, 0x40000, 0x2000);
const takePrefetched = () => prefetched.splice(0);
let clock = 1000;
performance.now = () => clock;
P.loadHcode('/HS4@2000:'); // starts attaching
// Bug caught: a pid seen once under another name (a launcher's fork child before its exec) was never looked at again, so that boot had no hook.
assert.equal(P.poll(7, 1, TEB), 0);
putString(0x2000 + 0x2c, 'game.exe', 0x2000 + 0x200);
assert.equal(P.poll(7, 1, TEB), 0);
clock += 2001;
// Bug caught: a game's own DLL loaded only its headers, so scans missed every page it had not run yet. Wine's DLLs stay headers and export directory.
assert.equal(P.poll(7, 1, TEB), 2);
assert.deepEqual(takePrefetched(), [[EXE, 0x4000], [WINEDLL, 0x1000], [GAMEDLL, 0x40000]]);
assert.equal(P.poll(7, 1, TEB), 1);
assert.deepEqual(takePrefetched(), [[WINEDLL + 0x7000, 0x500], [GAMEDLL + 0x2000, 0x500]]);
P.attach(7, 1, TEB);
assert.deepEqual(reports.splice(0), []);
assert.equal(P.poll(7, 1, TEB), 0);
assert.deepEqual(takePrefetched(), []);
// Bug caught: a DLL loaded after attach got nothing, so a script's retry of its export lookup or scan could never work.
addModule('C:\\games\\late.dll', LATE, 0x8000, 0x1000);
addModule('C:\\windows\\system32\\quartz.dll', LATEWINE, 0x20000, 0x3000);
assert.equal(P.poll(7, 1, TEB), 2);
assert.deepEqual(takePrefetched(), [[LATE, 0x8000], [LATEWINE, 0x1000]]);
assert.equal(P.poll(7, 1, TEB), 0);
assert.deepEqual(takePrefetched(), [[LATE + 0x1000, 0x500], [LATEWINE + 0x3000, 0x500]]);
assert.equal(P.poll(7, 1, TEB), 0);
assert.deepEqual(takePrefetched(), []);
console.log('frida-compat attach ok');
// Bug caught: a scan that stops at page ends made every game with a scanning script "have no text", with one console line to say why.
fakeScan = scanWithinPages;
P.attach(7, 1, TEB);
assert.deepEqual(reports.splice(0), ['frida: scan self-check failed: scripts that scan memory will find nothing']);
readable = EXE + 0x1000; // an exe with no second page has no boundary to check
P.attach(7, 1, TEB);
assert.deepEqual(reports.splice(0), []);
console.log('frida-compat scan self-check ok');
