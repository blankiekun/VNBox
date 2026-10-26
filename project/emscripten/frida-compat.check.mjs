// Self-check of frida-compat.js's string reads, pattern parsing and trans joining, over a fake
// guest memory (the C scan is checked live at attach). Run: node frida-compat.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
new Function(fs.readFileSync(new URL('./frida-compat.js', import.meta.url), 'utf8'))();
const F = globalThis.boxedwineFrida;
const { ptr, trans } = F;
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
assert.throws(() => ptr(0x2000).readU8(), /access violation/);
assert.equal(ptr('0x10').add(8).sub(ptr(4)).toString(), '0x14');
const { bytes, masks } = F.test.parsePattern('e8 ?? 4? : ff ff fe');
assert.deepEqual([...bytes, ...masks], [0xe8, 0, 0x40, 0xff, 0, 0xf0 & 0xfe]);
// "200+": fragments within 200 ms join with \n, a repeat of the previous one is dropped
const handler = trans.send((s) => s, '200+');
handler('a'); handler('a'); handler('b');
await new Promise((r) => setTimeout(r, 300));
assert.deepEqual(F.lines, ['a\nb']);
console.log('frida-compat self-check ok');
