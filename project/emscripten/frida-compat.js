// Frida-compat layer: runs dc00 text-hook scripts (github.com/0xDC00/scripts, MIT) unmodified
// against the emulated guest, over the host hook table in normalCPU.cpp (bw_* exports).
// Single-threaded builds only: hook callbacks run synchronously on the main thread with the
// guest paused, as Frida's do.
//
// URL: frida=<script url> loads that script (and its require()d libs) into the game process,
// the one whose first loaded module is the exe named by p=. Lines the script sends go to
// window.boxedwineText(line, thread) when defined (see "Runtime hooks"), and to the console.
// Only same-origin blob: URLs and same-origin files under the folder above frida-compat.js load (XSS guard).
//
// Covered: Interceptor.attach (onEnter), Breakpoint.add/remove, this.context.<x86 regs>,
// args[i], ptr(), NativePointer arithmetic and reads (U8..U64, Pointer, ByteArray, CString,
// Utf8/Utf16/ShiftJis/Ansi strings), Memory.scanSync/scan, Process.enumerateModules and
// module lookups, Module.findExportByName, hexdump, trans.send/replace (port of dc00's
// libLoader.js, MIT). Not covered: memory writes, Memory.alloc, onLeave, Instruction,
// NativeFunction, Stalker.
//
// Runtime hooks (contract version 1, boxedwineFrida.version): the game process is attached even
// without frida=, so hooks can be added and removed while the game runs. Lines go to
// window.boxedwineText(line, thread), thread = { id, label, kind: 'script' | 'hcode' | 'search',
// address, module }; the frida= script's thread id is "script".
//   load(source, name?) → id: runs a script now (no require()); throws on its error.
//   loadHcode(code) → id: one Textractor hook code. An H-code's first caller context is thread <id>,
//     later ones <id>#2, #3, … (context = caller module+offset unless N, plus the split value).
//   unload(id) / unloadAll() (also stops the search), keep(threadId) → id: keeps a search thread.
//   search.start() / stop() / running: hooks gdi32 GetGlyphOutlineA/W, TextOutA/W, ExtTextOutA/W
//     and user32 DrawText(Ex)A/W; one thread per export and caller, characters joined by thread
//     (400 ms gap), thread id "<export>@<caller address>".
//   scriptFor(threadId) → a script with no require() reproducing that thread (for frida= blob:).
// H-codes: /H{A,B,W,S,Q,V}[F][N][codepage#][padding+]data_offset[*deref][:split[*deref]]@addr[:module[:export]]
//   A/B: DBCS char (A lead byte high, B low), W: UTF-16 char, S/Q/V: cp932 / UTF-16 / UTF-8 string.
//   Offsets: hex, >= 0 [esp + offset] at the hooked instruction (4 = first argument), -4 eax, -8 ecx,
//   -C edx, -10 ebx, -14 esp, -18 ebp, -1C esi, -20 edi. Codepages 932 and 65001. @addr is absolute,
//   or relative to the module (empty: the exe) or to the module's export.
//   Refused with a message: /R read codes, types H/M/U (hex dump, UTF-32), null_length n<, other codepages.
(function () {
    'use strict';
    const ANSI = 'shift_jis'; // shortcut: the guest's ANSI code page is cp932 (LC_ALL ja_JP); read it from the locale when other locales matter
    const decoders = {};
    const decoder = (enc) => decoders[enc] || (decoders[enc] = new TextDecoder(enc));

    const F = globalThis.boxedwineFrida = {
        api: null, // set by the first poll from C (normalCPU.cpp)
        pid: 0, tid: 0, teb: 0,
        lines: [],
        poll, attach, hit,
    };

    // ---- guest memory --------------------------------------------------------------------
    let scratch = 0, scratchSize = 0;
    function buffer(len) {
        if (len > scratchSize) {
            if (scratch) F.api.free(scratch);
            scratchSize = Math.max(len, 65536);
            scratch = F.api.malloc(scratchSize);
        }
        return scratch;
    }
    // The readable prefix of [addr, addr + len) as a copy.
    function readPrefix(addr, len) {
        const buf = buffer(len);
        const n = F.api.read(F.pid, addr >>> 0, buf, len);
        return F.api.heap().slice(buf, buf + n);
    }
    function readBytes(addr, len) {
        const bytes = readPrefix(addr, len);
        if (bytes.length < len) throw new Error('access violation accessing ' + ptr(addr + bytes.length));
        return bytes;
    }
    // Units of `unit` bytes up to a zero unit (or max units), without the terminator.
    function readTerminated(addr, unit, max) {
        const parts = [];
        let total = 0;
        for (let step = 256; ; step = 4096) { // most strings (export names) fit the first, small read
            const want = Math.min(step, max < 0 ? step : (max - total / unit) * unit);
            if (want <= 0) break;
            const chunk = readPrefix(addr + total, want);
            if (!chunk.length && !total) throw new Error('access violation accessing ' + ptr(addr));
            let end = 0;
            while (end + unit <= chunk.length && !(chunk[end] === 0 && (unit === 1 || chunk[end + 1] === 0))) end += unit;
            parts.push(chunk.subarray(0, end));
            total += end;
            if (end < chunk.length || chunk.length < want) break; // terminator found, or unreadable from here
        }
        const out = new Uint8Array(total);
        let o = 0;
        for (const p of parts) { out.set(p, o); o += p.length; }
        return out;
    }
    const view = (addr, len) => new DataView(readBytes(addr, len).buffer);

    // ---- NativePointer --------------------------------------------------------------------
    function toU32(v) {
        if (v instanceof NativePointer) return v.v;
        if (typeof v === 'number') return v >>> 0;
        if (typeof v === 'bigint') return Number(BigInt.asUintN(32, v));
        if (typeof v === 'string') return (v.trim().toLowerCase().startsWith('0x') ? parseInt(v, 16) : parseInt(v, 10)) >>> 0;
        if (v && typeof v.toNumber === 'function') return v.toNumber() >>> 0;
        if (v && v.handle !== undefined) return toU32(v.handle);
        throw new TypeError('expected a pointer value: ' + v);
    }
    class NativePointer {
        constructor(v) { this.v = toU32(v); }
        add(x) { return new NativePointer(this.v + toU32(x)); }
        sub(x) { return new NativePointer(this.v - toU32(x)); }
        and(x) { return new NativePointer(this.v & toU32(x)); }
        or(x) { return new NativePointer(this.v | toU32(x)); }
        xor(x) { return new NativePointer(this.v ^ toU32(x)); }
        shl(n) { return new NativePointer(this.v << toU32(n)); }
        shr(n) { return new NativePointer(this.v >>> toU32(n)); }
        not() { return new NativePointer(~this.v); }
        isNull() { return this.v === 0; }
        equals(x) { return this.v === toU32(x); }
        compare(x) { const o = toU32(x); return this.v === o ? 0 : this.v < o ? -1 : 1; }
        toInt32() { return this.v | 0; }
        toUInt32() { return this.v; }
        toNumber() { return this.v; }
        toString(radix) { return radix === undefined || radix === 16 ? '0x' + this.v.toString(16) : this.v.toString(radix); }
        toJSON() { return this.toString(); }
        toMatchPattern() { return [0, 8, 16, 24].map(s => ((this.v >>> s) & 0xff).toString(16).padStart(2, '0')).join(' '); }
        readU8() { return readBytes(this.v, 1)[0]; }
        readS8() { return view(this.v, 1).getInt8(0); }
        readU16() { return view(this.v, 2).getUint16(0, true); }
        readS16() { return view(this.v, 2).getInt16(0, true); }
        readU32() { return view(this.v, 4).getUint32(0, true); }
        readS32() { return view(this.v, 4).getInt32(0, true); }
        readU64() { const d = view(this.v, 8); return d.getUint32(0, true) + d.getUint32(4, true) * 2 ** 32; } // shortcut: a Number, not Frida's UInt64
        readS64() { const d = view(this.v, 8); return d.getUint32(0, true) + d.getInt32(4, true) * 2 ** 32; }
        readFloat() { return view(this.v, 4).getFloat32(0, true); }
        readDouble() { return view(this.v, 8).getFloat64(0, true); }
        readPointer() { return new NativePointer(this.readU32()); }
        readByteArray(n) { return readBytes(this.v, toU32(n)).buffer; }
        readCString(size = -1) { return decoder('utf-8').decode(readTerminated(this.v, 1, size)); }
        readUtf8String(size = -1) { return decoder('utf-8').decode(readTerminated(this.v, 1, size)); }
        readUtf16String(length = -1) { return decoder('utf-16le').decode(readTerminated(this.v, 2, length)); }
        readAnsiString(size = -1) { return decoder(ANSI).decode(readTerminated(this.v, 1, size)); }
        readShiftJisString(size = -1) { return decoder('shift_jis').decode(readTerminated(this.v, 1, size)); }
    }
    const ptr = (v) => new NativePointer(v);

    // ---- Memory ----------------------------------------------------------------------------
    // "e8 ?? 4? : ff ff f0" → bytes and masks (Frida's pattern syntax, nibble wildcards included).
    function parsePattern(pattern) {
        const [hex, maskHex] = pattern.split(':');
        const nibbles = (s) => s.replace(/\s+/g, '');
        const p = nibbles(hex), m = maskHex ? nibbles(maskHex) : null;
        if (p.length % 2) throw new Error('invalid match pattern: ' + pattern);
        const bytes = new Uint8Array(p.length / 2), masks = new Uint8Array(p.length / 2);
        for (let i = 0; i < bytes.length; i++) {
            let b = 0, k = 0;
            for (const c of p.substr(i * 2, 2)) {
                b <<= 4; k <<= 4;
                if (c !== '?') { b |= parseInt(c, 16); k |= 0xf; }
            }
            if (m) k &= parseInt(m.substr(i * 2, 2), 16);
            bytes[i] = b & k; masks[i] = k;
        }
        return { bytes, masks };
    }
    function scanSync(address, size, pattern) {
        const { bytes, masks } = parsePattern(pattern);
        const max = 4096;
        const buf = buffer(bytes.length * 2 + max * 4);
        F.api.heap().set(bytes, buf);
        F.api.heap().set(masks, buf + bytes.length);
        const out = (buf + bytes.length * 2 + 3) & ~3;
        const n = Math.min(max, F.api.scan(F.pid, toU32(address), toU32(size), buf, buf + bytes.length, bytes.length, out, max));
        const found = new Uint32Array(F.api.heap().buffer, out, n).slice();
        return Array.from(found, (a) => ({ address: ptr(a), size: bytes.length }));
    }
    const Memory = {
        scanSync,
        scan(address, size, pattern, callbacks = {}) {
            return Promise.resolve().then(() => {
                try {
                    for (const m of scanSync(address, size, pattern)) {
                        if (callbacks.onMatch && callbacks.onMatch(m.address, m.size) === 'stop') break;
                    }
                } catch (e) {
                    if (callbacks.onError) return callbacks.onError(e.message);
                    throw e;
                }
                if (callbacks.onComplete) callbacks.onComplete();
            });
        },
        readByteArray: (p, n) => ptr(p).readByteArray(n),
    };

    // ---- Process / Module -------------------------------------------------------------------
    const u32 = (a) => ptr(a).readU32();
    function unicodeString(at) { // UNICODE_STRING { u16 Length; u16 MaximumLength; WCHAR* Buffer }
        const len = ptr(at).readU16();
        return len ? decoder('utf-16le').decode(readBytes(u32(at + 4), len)) : '';
    }
    // The loader's InLoadOrderModuleList, as Frida reads it: TEB → PEB → Ldr; the exe comes first.
    function modulesOf(teb) {
        const ldr = u32(u32(teb + 0x30) + 0x0c), head = ldr + 0x0c, out = [];
        for (let e = u32(head); e !== head && out.length < 1024; e = u32(e)) {
            const base = u32(e + 0x18);
            if (base) out.push(makeModule(unicodeString(e + 0x2c), base, u32(e + 0x20), unicodeString(e + 0x24)));
        }
        return out;
    }
    function makeModule(name, base, size, path) {
        return {
            name, base: ptr(base), size, path,
            enumerateExports() { return exportsOf(base).map(([n, a]) => ({ type: 'function', name: n, address: ptr(a) })); },
            findExportByName(n) { const e = exportsOf(base).find(([x]) => x === n); return e ? ptr(e[1]) : null; },
            getExportByName(n) { const p = this.findExportByName(n); if (!p) throw new Error(name + ': export not found: ' + n); return p; },
        };
    }
    // PE32 export directory → [[name, address]]. Forwarders and ordinal-only exports left out.
    // Kept per module (base + PE timestamp, so another DLL loaded at the same base is read anew):
    // findExportByName(null, …) walks every module's table.
    const exportsCache = new Map();
    function exportsOf(base) {
        try {
            const pe = base + u32(base + 0x3c), key = base + ':' + u32(pe + 8);
            const cached = exportsCache.get(key);
            if (cached) return cached;
            const rva = u32(pe + 0x78), dirSize = u32(pe + 0x7c);
            if (!rva) return [];
            const d = view(base + rva, 0x28);
            const count = d.getUint32(0x18, true), funcs = base + d.getUint32(0x1c, true);
            const names = view(base + d.getUint32(0x20, true), count * 4), ords = view(base + d.getUint32(0x24, true), count * 2);
            const out = [];
            for (let i = 0; i < count; i++) {
                const f = u32(funcs + 4 * ords.getUint16(2 * i, true));
                if (f >= rva && f < rva + dirSize) continue;
                out.push([ptr(base + names.getUint32(4 * i, true)).readCString(), base + f]);
            }
            exportsCache.set(key, out);
            return out;
        } catch (e) {
            return []; // a page not loaded (bw_read never loads file pages)
        }
    }
    const lower = (s) => (s || '').toLowerCase();
    const Process = {
        arch: 'ia32', platform: 'windows', pointerSize: 4, pageSize: 4096,
        get id() { return F.pid; },
        getCurrentThreadId: () => F.tid,
        enumerateModules: () => modulesOf(F.teb),
        get mainModule() { return modulesOf(F.teb)[0]; },
        findModuleByName: (n) => modulesOf(F.teb).find((m) => lower(m.name) === lower(n)) || null,
        getModuleByName(n) { const m = Process.findModuleByName(n); if (!m) throw new Error('unable to find module ' + n); return m; },
        findModuleByAddress(a) { const v = toU32(a); return modulesOf(F.teb).find((m) => v >= m.base.v && v - m.base.v < m.size) || null; },
        getModuleByAddress(a) { const m = Process.findModuleByAddress(a); if (!m) throw new Error('unable to find module containing ' + ptr(a)); return m; },
    };
    const Module = {
        findExportByName(mod, name) {
            const mods = mod === null || mod === undefined ? modulesOf(F.teb) : [Process.findModuleByName(mod)].filter(Boolean);
            for (const m of mods) { const p = m.findExportByName(name); if (p) return p; }
            return null;
        },
        getExportByName(mod, name) { const p = Module.findExportByName(mod, name); if (!p) throw new Error('export not found: ' + name); return p; },
        findBaseAddress: (n) => { const m = Process.findModuleByName(n); return m ? m.base : null; },
        getBaseAddress: (n) => Process.getModuleByName(n).base,
    };

    // ---- hooks -------------------------------------------------------------------------------
    const REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'];
    const context = {};
    REGS.forEach((r, i) => Object.defineProperty(context, r, {
        enumerable: true, get: () => ptr(F.api.reg(i)), set: (v) => F.api.setReg(i, toU32(v)),
    }));
    Object.defineProperty(context, 'sp', { get: () => context.esp, set: (v) => { context.esp = v; } });
    for (const pc of ['eip', 'pc']) Object.defineProperty(context, pc, { enumerable: pc === 'eip', get: () => ptr(F.api.reg(8)) });
    // args[i] at a function's first instruction (cdecl/stdcall: [esp + 4 + 4i]).
    const args = new Proxy({}, {
        get: (_, k) => (typeof k === 'string' && /^\d+$/.test(k) ? ptr(u32(F.api.reg(4) + 4 + 4 * k)) : undefined),
    });

    const listeners = new Map(); // hook id → { address, onEnter, handle }
    function removeListeners(test) {
        for (const [id, l] of listeners) if (test(l)) { F.api.hookRemove(id); listeners.delete(id); }
    }
    // Interceptor/Breakpoint for one handle: its hooks are detached together with it.
    function makeInterceptor(handle) {
        function addHook(target, callbacks) {
            const onEnter = typeof callbacks === 'function' ? callbacks : callbacks && callbacks.onEnter;
            if (callbacks && callbacks.onLeave) console.warn('[frida] onLeave is not supported; only onEnter runs');
            const address = toU32(target);
            const id = F.api.hookAdd(F.pid, address);
            listeners.set(id, { address, onEnter, handle });
            return { detach() { removeListeners((l) => l === listeners.get(id)); } };
        }
        const Interceptor = {
            attach: addHook,
            detachAll() { removeListeners((l) => l.handle === handle); },
            flush() {},
        };
        const Breakpoint = {
            add: addHook,
            remove(target) {
                if (target && typeof target.detach === 'function') target.detach();
                else { const a = toU32(target); removeListeners((l) => l.handle === handle && l.address === a); }
            },
        };
        return { Interceptor, Breakpoint };
    }

    function hit(id, pid, tid) {
        const l = listeners.get(id);
        if (!l || !l.onEnter || pid !== F.pid) return;
        F.tid = tid;
        const invocation = { context, threadId: tid, depth: 0, get returnAddress() { return ptr(u32(F.api.reg(4))); } };
        try {
            l.onEnter.call(invocation, args);
        } catch (e) {
            const message = 'hook ' + ptr(l.address) + ': ' + (e && e.stack || e);
            if (l.reported) console.error('[frida] ' + message); else report(message);
            l.reported = true;
        }
    }

    // console, and the parent page when lazyfs.js (vnboxReport) is loaded
    function report(message) {
        if (typeof vnboxReport === 'function') vnboxReport('frida', message); else console.error('[frida] ' + message);
    }

    // ---- lines ------------------------------------------------------------------------------------
    // Every line goes to window.boxedwineText(line, thread), thread = { id, label, kind, address, module }
    // (the runtime hook contract, version 1). Script lines keep the old `[frida] "…"` console form.
    function emit(text, thread) {
        if (text === null || text === undefined || text === '') return;
        if (!handles.has(thread.handle)) return; // unloaded while a join timer ran
        const t = { id: thread.id, label: thread.label, kind: thread.kind, address: thread.address, module: thread.module };
        if (t.kind === 'script') {
            F.lines.push(text);
            console.log('[frida] ' + JSON.stringify(text));
        } else {
            console.log('[frida ' + t.id + '] ' + JSON.stringify(text));
        }
        if (typeof globalThis.boxedwineText === 'function') globalThis.boxedwineText(text, t);
    }

    // ---- trans (dc00 libLoader.js, MIT: joins a hook's fragments into lines) ----------------------
    function makeTrans(send) {
        return {
            _copy(s) { send(s); },
            _send(s) { if (s !== null) this._copy(s); },
            replace(fn, override) {
                if (override === true) {
                    this._copy = (s) => send(fn(s));
                } else {
                    const prev = this._copy;
                    this._copy = function (s) { prev.call(this, fn(s)); };
                }
            },
            // delay: 0 sends each call; N ms debounces to the last call; "N" sends the first call's
            // text and appends the last one's; "N+" / "N-" collect calls (appended / prepended, a
            // repeat of the previous one dropped) and join them with "\n", or with the separator
            // between "N+…+" when given.
            send(fn, delay = 0) {
                if (!(fn instanceof Function)) return this._send(fn);
                const self = this;
                const direct = function () { self._send(fn.apply(this, arguments)); };
                if (delay === 0) return direct;
                let timer = null;
                if (typeof delay === 'string') {
                    const ms = parseInt(delay), last = delay[delay.length - 1], plus = last === '+';
                    if (plus || last === '-') {
                        const m = delay.match(plus ? /(^\d+\+)(.*)(\+$)/s : /(^\d+-)(.*)(-$)/s);
                        const sep = m !== null ? m[2] : '\n', dedupe = sep === '\n';
                        let lines = [], previous;
                        return function () {
                            const s = fn.apply(this, arguments);
                            if (!dedupe) lines[plus ? 'push' : 'unshift'](s);
                            else if (s !== null && s !== previous) { previous = s; lines[plus ? 'push' : 'unshift'](s); }
                            clearTimeout(timer);
                            timer = setTimeout(() => { self._send(lines.join(sep)); lines = []; previous = null; }, ms);
                        };
                    }
                    let first, calls = 0;
                    return function () {
                        const a = arguments, that = this;
                        calls++;
                        if (timer === null) first = fn.apply(that, a);
                        clearTimeout(timer);
                        timer = setTimeout(() => {
                            timer = null;
                            if (calls > 1) {
                                calls = 0;
                                const s = fn.apply(that, a);
                                if (first !== null) { if (s !== null) first = first + '\n' + s; self._send(first); } else self._send(s);
                            } else {
                                calls = 0;
                                self._send(first);
                            }
                        }, ms);
                    };
                }
                if (delay > 0) {
                    return function () {
                        const a = arguments, that = this;
                        clearTimeout(timer);
                        timer = setTimeout(() => direct.apply(that, a), delay);
                    };
                }
                const quiet = -delay; // negative: the first call of a burst only
                return function () {
                    if (timer === null) direct.apply(this, arguments);
                    clearTimeout(timer);
                    timer = setTimeout(() => { timer = null; }, quiet);
                };
            },
        };
    }

    function hexdump(target, options = {}) {
        const len = options.length || 256;
        const bytes = target instanceof ArrayBuffer ? new Uint8Array(target) : readPrefix(toU32(target), len);
        const start = target instanceof ArrayBuffer ? 0 : toU32(target), rows = [];
        for (let i = 0; i < bytes.length; i += 16) {
            const row = Array.from(bytes.subarray(i, i + 16));
            rows.push((start + i).toString(16).padStart(8, '0') + '  ' + row.map((b) => b.toString(16).padStart(2, '0')).join(' ').padEnd(48) +
                '  ' + row.map((b) => (b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '.')).join(''));
        }
        return rows.join('\n');
    }

    // ---- text hooks shared by H-codes, the search and the scripts scriptFor() writes -------------
    // These three functions are copied by source into generated scripts (Function.prototype.toString),
    // so they use only their parameters and browser globals (TextDecoder, setTimeout).

    // Joins a hook's pieces into lines, per key: a piece of one character is appended, a longer one
    // starts a new line of the same text (a repeat of the previous longer piece, a redraw, is
    // dropped). The text goes out after 400 ms without a piece, after 3 s, or at 2000 characters.
    // Games that redraw the previous text as it fades out start a line with its end: that part
    // (3+ characters) is dropped, unless nothing would be left.
    function makeJoiner(send) {
        const open = new Map(), sent = new Map();
        function flush(key) {
            const b = open.get(key);
            if (!b) return;
            open.delete(key);
            clearTimeout(b.timer);
            const prev = sent.get(key) || '';
            let text = b.text;
            for (let k = Math.min(prev.length, text.length - 1, 500); k >= 3; k--) {
                if (prev.endsWith(text.slice(0, k))) { text = text.slice(k).replace(/^\n/, ''); break; }
            }
            sent.set(key, b.text);
            if (text) send(key, text);
        }
        return function push(key, piece, whole) {
            if (!piece) return;
            if (whole) { flush(key); send(key, piece); return; }
            const now = Date.now();
            let b = open.get(key);
            if (b && now - b.started > 3000) { flush(key); b = null; }
            if (!b) open.set(key, b = { text: '', last: null, started: now, timer: 0 });
            const single = [...piece].length === 1;
            if (single || piece !== b.last) {
                b.text += b.text && !single ? '\n' + piece : piece;
                if (!single) b.last = piece;
            }
            clearTimeout(b.timer);
            b.timer = setTimeout(() => flush(key), 400);
            if (b.text.length > 2000) flush(key);
        };
    }

    // Hooks Wine's GDI/USER text exports in the game process. g: { Interceptor, Process, Module, ptr }.
    // only: null for all, or { api, module, offset } for one export called from one place.
    // push(api, returnAddress, text) for every call that draws text.
    function hookTextApis(g, only, push) {
        const A = new TextDecoder('shift_jis');
        const dbcs = (c) => A.decode(new Uint8Array(c > 0xff ? [c >> 8, c & 0xff] : [c]));
        const str = (p, n, wide) => {
            n = Math.min(n, 4096);
            if (n === 0 || p.isNull()) return null;
            return wide ? p.readUtf16String(n < 0 ? 4096 : n) : p.readAnsiString(n < 0 ? 4096 : n);
        };
        const ETO_GLYPH_INDEX = 0x10, DT_CALCRECT = 0x400;
        const apis = {
            GetGlyphOutlineA: ['gdi32.dll', (a) => dbcs(a[1].toUInt32() & 0xffff)],
            GetGlyphOutlineW: ['gdi32.dll', (a) => String.fromCharCode(a[1].toUInt32() & 0xffff)],
            TextOutA: ['gdi32.dll', (a) => str(a[3], a[4].toInt32(), false)],
            TextOutW: ['gdi32.dll', (a) => str(a[3], a[4].toInt32(), true)],
            ExtTextOutA: ['gdi32.dll', (a) => (a[3].toUInt32() & ETO_GLYPH_INDEX ? null : str(a[5], a[6].toInt32(), false))],
            ExtTextOutW: ['gdi32.dll', (a) => (a[3].toUInt32() & ETO_GLYPH_INDEX ? null : str(a[5], a[6].toInt32(), true))],
            DrawTextA: ['user32.dll', (a) => (a[4].toUInt32() & DT_CALCRECT ? null : str(a[1], a[2].toInt32(), false))],
            DrawTextW: ['user32.dll', (a) => (a[4].toUInt32() & DT_CALCRECT ? null : str(a[1], a[2].toInt32(), true))],
            DrawTextExA: ['user32.dll', (a) => (a[4].toUInt32() & DT_CALCRECT ? null : str(a[1], a[2].toInt32(), false))],
            DrawTextExW: ['user32.dll', (a) => (a[4].toUInt32() & DT_CALCRECT ? null : str(a[1], a[2].toInt32(), true))],
        };
        let caller = null;
        if (only) {
            const m = only.module ? g.Process.findModuleByName(only.module) : null;
            caller = m ? m.base.add(only.offset) : g.ptr(only.address);
        }
        let hooked = 0;
        for (const name of Object.keys(apis)) {
            if (only && only.api !== name) continue;
            const [module, read] = apis[name];
            const at = g.Module.findExportByName(module, name);
            if (!at) continue;
            g.Interceptor.attach(at, function (args) {
                const ret = this.returnAddress;
                if (caller && !ret.equals(caller)) return;
                let text;
                try { text = read(args); } catch (e) { return; } // a bad pointer: the game's own error
                if (text) push(name, ret, text);
            });
            hooked++;
        }
        return hooked;
    }

    // Hooks one parsed H-code (parseHcode). push(context, text, whole): context is '' with N,
    // else the caller "module+0xoffset", plus ":0x<split>" with a split. only: hook that context alone.
    function hookHcode(hp, g, push, only) {
        const regs = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'];
        let at = g.ptr(hp.address);
        if (hp.module !== null) {
            const m = hp.module === '' ? g.Process.enumerateModules()[0] : g.Process.findModuleByName(hp.module);
            if (!m) throw new Error('H-code: module not loaded: ' + hp.module);
            if (hp.func !== null) {
                const f = m.findExportByName(hp.func);
                if (!f) throw new Error('H-code: ' + m.name + ' has no export ' + hp.func);
                at = f.add(hp.address);
            } else {
                at = m.base.add(hp.address);
            }
        }
        const dec = new TextDecoder(hp.encoding);
        const value = (ctx, off) => (off < 0 ? ctx[regs[-off / 4 - 1]] : ctx.esp.add(off).readPointer());
        const places = new Map();
        const place = (ret) => {
            let s = places.get(ret.toUInt32());
            if (s === undefined) {
                const m = g.Process.findModuleByAddress(ret);
                s = m ? m.name + '+0x' + ret.sub(m.base).toUInt32().toString(16) : ret.toString();
                places.set(ret.toUInt32(), s);
            }
            return s;
        };
        g.Interceptor.attach(at, function () {
            let key = hp.noContext ? '' : place(this.returnAddress);
            if (hp.split !== null) {
                let s = value(this.context, hp.split);
                if (hp.splitDeref !== null) s = s.add(hp.splitDeref).readPointer();
                key += ':' + s.toString();
            }
            if (only !== undefined && only !== null && key !== only) return;
            let v = value(this.context, hp.offset);
            if (hp.deref !== null) v = v.add(hp.deref).readPointer();
            let text;
            if (hp.string) {
                const p = v.add(hp.padding);
                text = hp.encoding === 'utf-16le' ? p.readUtf16String() : hp.encoding === 'utf-8' ? p.readUtf8String() : p.readShiftJisString();
            } else {
                const c = v.toUInt32() & 0xffff;
                text = hp.encoding === 'utf-16le' ? String.fromCharCode(c)
                    : dec.decode(new Uint8Array(c > 0xff ? (hp.type === 'A' ? [c >> 8, c & 0xff] : [c & 0xff, c >> 8]) : [c]));
            }
            push(key, text, hp.full);
        });
        return at;
    }

    // Textractor's /H hook code, the subset hookHcode runs (README "H-codes" at the top).
    function parseHcode(code) {
        const src = String(code).trim();
        const fail = (why) => { throw new Error('H-code ' + JSON.stringify(src) + ': ' + why); };
        let s = src.replace(/^\//, ''), m;
        if (s[0] === 'R') fail('read codes (/R) are not supported, only hook codes (/H)');
        if (s[0] !== 'H') fail('expected /H at the start');
        const type = s[1] || '';
        if (!type || !'ABWSQV'.includes(type)) fail('type ' + JSON.stringify(type) + ' is not supported (A, B, W, S, Q, V)');
        s = s.slice(2);
        const hp = {
            type, string: 'SQV'.includes(type), full: false, noContext: false, encoding: 'shift_jis', padding: 0,
            offset: 0, deref: null, split: null, splitDeref: null, address: 0, module: null, func: null,
        };
        if (hp.string && s[0] === 'F') { hp.full = true; s = s.slice(1); }
        if (/^\d+</.test(s)) fail('null_length (n<) is not supported');
        if (s[0] === 'N') { hp.noContext = true; s = s.slice(1); }
        let codepage = 932;
        if ((m = s.match(/^(\d+)#/))) { codepage = +m[1]; s = s.slice(m[0].length); }
        if ((m = s.match(/^([0-9a-f]+)\+/i))) { hp.padding = parseInt(m[1], 16); s = s.slice(m[0].length); }
        const hex = (what) => {
            const h = s.match(/^-?[0-9a-f]+/i);
            if (!h) fail('expected ' + what + ' (hex) at ' + JSON.stringify(s));
            s = s.slice(h[0].length);
            return parseInt(h[0], 16);
        };
        const offset = (what) => {
            const v = hex(what);
            if (v < 0 && (v < -0x20 || v % 4)) fail(what + ' ' + v.toString(16) + ': registers are -4 (eax) to -20 (edi) in steps of 4');
            return v;
        };
        hp.offset = offset('data offset');
        if (s[0] === '*') { s = s.slice(1); hp.deref = hex('deref offset'); }
        if (s[0] === ':') {
            s = s.slice(1);
            hp.split = offset('split offset');
            if (s[0] === '*') { s = s.slice(1); hp.splitDeref = hex('split deref offset'); }
        }
        if (!(m = s.match(/^@([0-9a-f]+)(?::([^:]*))?(?::(.+))?$/i))) fail('expected @address[:module[:function]] at ' + JSON.stringify(s));
        hp.address = parseInt(m[1], 16);
        if (m[2] !== undefined) hp.module = m[2];
        if (m[3] !== undefined) hp.func = m[3];
        if (type === 'W' || type === 'Q') hp.encoding = 'utf-16le';
        else if (type === 'V' || codepage === 65001) hp.encoding = 'utf-8';
        else if (codepage !== 932) fail('codepage ' + codepage + ' is not supported (932, 65001)');
        return hp;
    }

    // ---- handles: the boot script, runtime scripts, H-codes, kept search threads ------------------
    // A handle's id is also its thread id (H-code contexts after the first: "<id>#2", "#3", …).
    // Each runs in its own scope (Interceptor, Breakpoint, trans) when the game process is attached,
    // again after the game restarts, and is detached as a whole by unload().
    const handles = new Map();
    let nextHandle = 1;
    function addHandle(h) {
        h.thread = Object.assign({ handle: h.id, id: h.id }, h.thread);
        handles.set(h.id, h);
        if (attachedPid) runHandle(h);
        return h;
    }
    const GLOBAL_NAMES = ['NativePointer', 'ptr', 'NULL', 'Memory', 'Process', 'Module', 'Interceptor', 'Breakpoint', 'trans', 'hexdump'];
    function runHandle(h) {
        const { Interceptor, Breakpoint } = makeInterceptor(h);
        const trans = makeTrans((s) => emit(s, h.thread));
        h.globals = { NativePointer, ptr, NULL: ptr(0), Memory, Process, Module, Interceptor, Breakpoint, trans, hexdump };
        h.run(h.globals);
    }
    function dropHandle(id) {
        handles.delete(id);
        removeListeners((l) => l.handle.id === id);
    }
    function runtimeScript(id, label, source, name) {
        const names = GLOBAL_NAMES;
        const fn = new Function('require', 'module', 'exports', ...names, source + '\n//# sourceURL=' + (name || id)); // throws on a syntax error
        const require = (n) => { throw new Error('require(' + JSON.stringify(n) + ') is not available in scripts loaded at run time'); };
        return {
            id, kind: 'script', source, thread: { label, kind: 'script' },
            run(g) { const module = { exports: {}, parent: null, id }; fn(require, module, module.exports, ...names.map((n) => g[n])); },
        };
    }
    // Adds the handle; when the game is attached its script runs now and an error unloads it and throws.
    function start(h) {
        try {
            addHandle(h);
        } catch (e) {
            dropHandle(h.id);
            throw new Error((e && e.message) || String(e));
        }
        return h.id;
    }
    function load(source, name) {
        if (typeof source !== 'string') throw new Error('load: source must be a string');
        const id = 'script:' + nextHandle++;
        return start(runtimeScript(id, name ? String(name) : 'Hook script', source, name));
    }
    function loadHcode(code) {
        const hp = parseHcode(code);
        const id = 'hcode:' + nextHandle++;
        const contexts = new Map(); // context → thread
        const h = {
            id, kind: 'hcode', hp, code: String(code).trim(), contexts, thread: { label: String(code).trim(), kind: 'hcode' },
            run(g) {
                const join = makeJoiner((key, text) => emit(text, contexts.get(key)));
                hookHcode(hp, g, (key, text, whole) => {
                    if (!contexts.has(key)) {
                        const n = contexts.size;
                        contexts.set(key, n === 0 ? h.thread : {
                            handle: id, id: id + '#' + (n + 1), kind: 'hcode', label: h.code + (key ? ' ' + key : ''), module: key.split('+')[0] || undefined,
                        });
                        if (n === 0 && key) h.thread.label = h.code + ' ' + key;
                    }
                    join(key, text, whole);
                });
            },
        };
        return start(h);
    }
    function unload(id) {
        const h = handles.get(String(id).split('#')[0]);
        if (!h) return false;
        dropHandle(h.id);
        return true;
    }
    function unloadAll() {
        search.stop();
        for (const id of [...handles.keys()]) if (id !== SEARCH.id) dropHandle(id);
    }

    // ---- auto search (H4 step a): every text export, one thread per export and caller ----------
    const SEARCH = { id: '\0search', kind: 'search' };
    const searchThreads = new Map(); // thread id → thread (with api, module, offset)
    let searching = false;
    function runSearch() {
        const g = Object.assign({}, SEARCH.globalsBase, makeInterceptor(SEARCH));
        const join = makeJoiner((id, text) => { if (searching && !handles.has(id)) emit(text, searchThreads.get(id)); });
        const n = hookTextApis(g, null, (api, ret, text) => {
            const id = api + '@' + ret.toString();
            if (!searchThreads.has(id)) {
                const m = Process.findModuleByAddress(ret);
                const offset = m ? ret.sub(m.base).toUInt32() : ret.toUInt32();
                searchThreads.set(id, {
                    handle: SEARCH.id, id, kind: 'search', address: ret.toString(), module: m ? m.name : undefined, api, offset,
                    label: api + ' ← ' + (m ? m.name + '+0x' + offset.toString(16) : ret.toString()),
                });
            }
            join(id, text);
        });
        console.log('[frida] search: ' + n + ' text exports hooked');
    }
    handles.set(SEARCH.id, SEARCH); // emit() checks handles; search lines are gated by `searching`
    SEARCH.globalsBase = { ptr, Process, Module };
    SEARCH.run = runSearch;
    const search = {
        start() {
            if (searching) return;
            searching = true;
            if (attachedPid) runSearch();
        },
        stop() {
            searching = false;
            removeListeners((l) => l.handle === SEARCH);
        },
        get running() { return searching; },
    };
    function keep(threadId) {
        const t = searchThreads.get(threadId);
        if (!t) throw new Error('keep: no search thread ' + threadId);
        if (handles.has(t.id)) return t.id;
        const only = { api: t.api, module: t.module || null, offset: t.offset, address: t.address };
        return start({
            id: t.id, kind: 'search', only, thread: Object.assign({}, t, { handle: t.id }),
            run(g) {
                const h = this, join = makeJoiner((key, text) => emit(text, h.thread));
                hookTextApis(g, only, (api, ret, text) => join('', text));
            },
        });
    }

    // A script reproducing one thread, for the book's `hook` field (loaded with frida= at the next boot).
    function scriptFor(threadId) {
        threadId = String(threadId);
        const base = threadId.split('#')[0];
        const h = handles.get(base);
        const head = (what) => '// ' + what + '\n// Written by VNBox frida-compat.js scriptFor(): runs as a frida= script on its own (no libraries).\n' +
            'const g = { Interceptor, Process, Module, ptr };\nconst makeJoiner = ' + makeJoiner + ';\n';
        if (h && h.kind === 'script') return h.source;
        if (h && h.kind === 'hcode') {
            let context = null;
            for (const [key, t] of h.contexts) if (t.id === threadId) context = key;
            return head('H-code ' + JSON.stringify(h.code) + (context ? ', context ' + JSON.stringify(context) : '')) +
                'const hookHcode = ' + hookHcode + ';\nconst push = makeJoiner((key, text) => trans.send(text));\n' +
                'hookHcode(' + JSON.stringify(h.hp) + ', g, push, ' + JSON.stringify(context) + ');\n';
        }
        const t = (h && h.kind === 'search' && h.thread) || searchThreads.get(threadId);
        if (t && t.api) {
            const only = { api: t.api, module: t.module || null, offset: t.offset, address: t.address };
            return head(t.label) + 'const hookTextApis = ' + hookTextApis + ';\nconst push = makeJoiner((key, text) => trans.send(text));\n' +
                'hookTextApis(g, ' + JSON.stringify(only) + ', (api, ret, text) => push(\'\', text));\n';
        }
        throw new Error('scriptFor: no thread ' + threadId);
    }

    Object.assign(F, {
        version: 1, NativePointer, ptr, NULL: ptr(0), Memory, Process, Module, hexdump,
        load, loadHcode, unload, unloadAll, search, keep, scriptFor,
    });
    F.test = { parsePattern, readTerminated, toU32, scriptAllowed, parseHcode, makeJoiner, makeTrans, hookHcode };

    // ---- the boot script (frida= URL) ------------------------------------------------------------
    // require() shim: the script's relative requires are fetched up front, then evaluated
    // synchronously as CommonJS modules when the script runs.
    const sources = new Map(), cache = new Map();
    // Scripts run with the page's origin, so only same-origin blob: URLs (made by the parent page)
    // and files under the folder above frida-compat.js (the embedding site's /vn/) are allowed.
    // The folder is taken from this file's URL, not the page's: an embedding page may sit elsewhere
    // (/vn/vnbox.html loading /vn/vnbox/frida-compat.js allows /vn/).
    const selfUrl = (typeof document !== 'undefined' && document.currentScript && document.currentScript.src) ||
        (typeof location !== 'undefined' ? location.href : '');
    function scriptAllowed(url, page, script = page) {
        let u;
        try { u = new URL(url); } catch (e) { return false; }
        const pageUrl = new URL(page);
        if (u.protocol === 'blob:') return u.origin === pageUrl.origin;
        // servers decode %2F/%5C and then resolve .., so an encoded separator could leave the folder
        if (/%2f|%5c/i.test(u.pathname)) return false;
        return (u.protocol === 'https:' || u.protocol === 'http:') && u.origin === pageUrl.origin &&
            u.href.startsWith(new URL('../', script).href);
    }
    async function fetchTree(url) {
        if (sources.has(url)) return;
        if (!scriptAllowed(url, location.href, selfUrl)) throw new Error('frida: script URL refused (not same-origin blob: or under the folder above frida-compat.js): ' + url);
        const res = await fetch(url);
        if (!res.ok) throw new Error('frida: ' + url + ': HTTP ' + res.status);
        if (res.redirected && !scriptAllowed(res.url, location.href, selfUrl)) throw new Error('frida: script redirected off the allowed paths: ' + res.url);
        const text = await res.text();
        sources.set(url, text);
        const deps = [...text.matchAll(/require\(\s*['"](\.{1,2}\/[^'"]+)['"]\s*\)/g)].map((m) => new URL(m[1], url).href);
        await Promise.all(deps.map(fetchTree));
    }
    function evaluate(url, globals, parent = null) {
        if (cache.has(url)) return cache.get(url).exports;
        const module = { exports: {}, parent, id: url }; // libs check module.parent to refuse running alone
        cache.set(url, module);
        const require = (name) => {
            if (!/^\.{1,2}\//.test(name)) throw new Error('frida: require of a non-relative module is not supported: ' + name);
            const dep = new URL(name, url).href;
            if (!sources.has(dep)) throw new Error('frida: module not preloaded: ' + dep);
            return evaluate(dep, globals, module);
        };
        const names = Object.keys(globals);
        new Function('require', 'module', 'exports', ...names, sources.get(url) + '\n//# sourceURL=' + url)(require, module, module.exports, ...names.map((n) => globals[n]));
        return module.exports;
    }

    const params = typeof location === 'undefined' ? null : new URLSearchParams(location.search);
    const scriptUrl = params && params.get('frida') ? new URL(params.get('frida'), location.href).href : null;
    const exe = lower(params && (params.get('p') || '').split(/[\\/]/).pop());
    // attachedPid: the game process (the exe's), attached with or without a boot script so hooks can be
    // loaded at run time; a new process of the exe (the game restarted) gets every handle again.
    // candidate: the process being prepared, otherPids: processes of other exes.
    let loaded = !scriptUrl, attachedPid = 0, candidate = 0, phase = 0, seenAt = 0;
    const otherPids = new Set();
    if (scriptUrl) {
        fetchTree(scriptUrl).then(() => {
            loaded = true;
            handles.set('script', {
                id: 'script', kind: 'script', thread: { handle: 'script', id: 'script', label: 'Hook script', kind: 'script' },
                get source() { return sources.get(scriptUrl); },
                run(g) { cache.clear(); evaluate(scriptUrl, g); },
            });
        }, (e) => report('loading ' + scriptUrl + ': ' + (e && e.message || e)));
    }

    // From C every 1024 syscalls, on the guest thread of process pid. 0: nothing, 2: load the
    // ranges asked for with prefetch and poll again, 1: load them and attach.
    function poll(pid, tid, teb) {
        if (!loaded || pid === attachedPid || otherPids.has(pid) || !teb) return 0;
        F.pid = pid; // for the reads below; hits keep going to attachedPid's hooks
        try {
            return pollProcess(pid, teb);
        } finally {
            F.pid = attachedPid;
        }
    }
    function pollProcess(pid, teb) {
        let mods;
        try { mods = modulesOf(teb); } catch (e) { return 0; }
        if (!mods.length) return 0;
        if (lower(mods[0].name) !== exe) { otherPids.add(pid); return 0; }
        // shortcut: attach 2 s after the exe shows up, like attaching to a running process; scripts
        // that hook a DLL loaded later need a wait or a retry (dc00's do it themselves where needed)
        const now = performance.now();
        if (candidate !== pid) { candidate = pid; seenAt = now; phase = 0; }
        if (now - seenAt < 2000) return 0;
        if (phase === 0) { // the main module whole and every module's headers
            phase = 1;
            F.api.prefetch(mods[0].base.v, mods[0].size);
            for (const m of mods.slice(1)) F.api.prefetch(m.base.v, 0x1000);
            return 2;
        }
        for (const m of mods.slice(1)) { // export directories, for Module.findExportByName
            try {
                const pe = m.base.v + u32(m.base.v + 0x3c);
                const rva = u32(pe + 0x78), size = u32(pe + 0x7c);
                if (rva) F.api.prefetch(m.base.v + rva, size);
            } catch (e) { /* header unreadable */ }
        }
        return 1;
    }

    function attach(pid, tid, teb) {
        if (attachedPid) { // a new process of the exe: drop the old one's hooks and run every handle afresh
            console.log('[frida] re-attach: pid ' + attachedPid + ' -> ' + pid);
            removeListeners(() => true);
        }
        attachedPid = pid;
        candidate = 0;
        Object.assign(F, { pid, tid, teb });
        const main = modulesOf(teb)[0];
        // self-check of the C scan and pattern parsing on the exe's DOS header
        const ok = ['4d 5a', '4? 5a', '?? 5a 90', '4d 5a : ff ff'].every((p) => {
            const r = scanSync(main.base, 0x40, p);
            return r.length === 1 && r[0].address.equals(main.base);
        }) && scanSync(main.base, 0x40, '4d 5a 4d').length === 0;
        console.log('[frida] attach pid ' + pid + ' ' + main.name + ' @ ' + main.base + ' size 0x' + main.size.toString(16) + ', scan self-check ' + (ok ? 'ok' : 'FAILED'));
        for (const h of handles.values()) {
            if (h === SEARCH) { if (searching) runSearch(); continue; }
            try {
                runHandle(h);
            } catch (e) {
                report((h.id === 'script' ? 'script' : h.id) + ' failed: ' + (e && e.stack || e));
            }
        }
    }
})();
