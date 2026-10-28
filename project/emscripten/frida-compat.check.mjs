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
F.api = {
    malloc: (n) => { const p = top; top += n; return p; }, free() {}, heap: () => heap,
    read: (pid, addr, buf, len) => { const n = Math.max(0, Math.min(len, 0x2000 - addr)); heap.set(mem.subarray(addr, addr + n), buf); return n; }, // 0x2000+ unreadable
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
// a line that starts with a redraw of the previous one's end (最果てのイマ fades the old text out) loses that part
const joined = [], join = F.test.makeJoiner((key, text) => joined.push(text));
for (const burst of ['？？「―――」', '「―――」？？遠くから。', '遠くから。「……」', '「……」']) {
    for (const c of burst) join('k', c);
    await new Promise((r) => setTimeout(r, 450));
}
assert.deepEqual(joined, ['？？「―――」', '？？遠くから。', '「……」', '「……」']);
console.log('frida-compat joiner ok');
