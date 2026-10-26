// Frida-compat layer: runs dc00 text-hook scripts (github.com/0xDC00/scripts, MIT) unmodified
// against the emulated guest, over the host hook table in normalCPU.cpp (bw_* exports).
// Single-threaded builds only: hook callbacks run synchronously on the main thread with the
// guest paused, as Frida's do.
//
// URL: frida=<script url> loads that script (and its require()d libs) into the game process,
// the one whose first loaded module is the exe named by p=. Lines the script sends go to
// window.boxedwineText(line) when defined, and to the console.
// Only same-origin blob: URLs and same-origin files under the folder above this page load (XSS guard).
//
// Covered: Interceptor.attach (onEnter), Breakpoint.add/remove, this.context.<x86 regs>,
// args[i], ptr(), NativePointer arithmetic and reads (U8..U64, Pointer, ByteArray, CString,
// Utf8/Utf16/ShiftJis/Ansi strings), Memory.scanSync/scan, Process.enumerateModules and
// module lookups, Module.findExportByName, hexdump, trans.send/replace (port of dc00's
// libLoader.js, MIT). Not covered: memory writes, Memory.alloc, onLeave, Instruction,
// NativeFunction, Stalker.
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
        for (;;) {
            const want = Math.min(4096, max < 0 ? 4096 : (max - total / unit) * unit);
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
    function exportsOf(base) {
        try {
            const pe = base + u32(base + 0x3c), rva = u32(pe + 0x78), dirSize = u32(pe + 0x7c);
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

    const listeners = new Map(); // hook id → { address, onEnter }
    function addHook(target, callbacks) {
        const onEnter = typeof callbacks === 'function' ? callbacks : callbacks && callbacks.onEnter;
        if (callbacks && callbacks.onLeave) console.warn('[frida] onLeave is not supported; only onEnter runs');
        const address = toU32(target);
        const id = F.api.hookAdd(F.pid, address);
        listeners.set(id, { address, onEnter });
        return { detach() { F.api.hookRemove(id); listeners.delete(id); } };
    }
    function removeHooksAt(target) {
        const address = toU32(target);
        for (const [id, l] of listeners) if (l.address === address) { F.api.hookRemove(id); listeners.delete(id); }
    }
    const Interceptor = {
        attach: addHook,
        detachAll() { for (const id of listeners.keys()) F.api.hookRemove(id); listeners.clear(); },
        flush() {},
    };
    const Breakpoint = {
        add: addHook,
        remove(target) { if (target && typeof target.detach === 'function') target.detach(); else removeHooksAt(target); },
    };

    function hit(id, pid, tid) {
        const l = listeners.get(id);
        if (!l || !l.onEnter || pid !== F.pid) return;
        F.tid = tid;
        const invocation = { context, threadId: tid, depth: 0, get returnAddress() { return ptr(u32(F.api.reg(4))); } };
        try {
            l.onEnter.call(invocation, args);
        } catch (e) {
            console.error('[frida] hook ' + ptr(l.address) + ': ' + (e && e.stack || e));
        }
    }

    // ---- trans (dc00 libLoader.js, MIT: joins a hook's fragments into lines) ----------------------
    function emit(text) {
        if (text === null || text === undefined || text === '') return;
        F.lines.push(text);
        console.log('[frida] ' + JSON.stringify(text));
        if (typeof globalThis.boxedwineText === 'function') globalThis.boxedwineText(text);
    }
    const trans = {
        _copy(s) { emit(s); },
        _send(s) { if (s !== null) this._copy(s); },
        replace(fn, override) {
            if (override === true) {
                this._copy = (s) => emit(fn(s));
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

    // Scripts get these as parameters: a global Module would clash with Emscripten's.
    const fridaGlobals = { NativePointer, ptr, NULL: ptr(0), Memory, Process, Module, Interceptor, Breakpoint, trans, hexdump };
    Object.assign(F, fridaGlobals);
    F.test = { parsePattern, readTerminated, toU32, scriptAllowed }; // for frida-compat.check.mjs

    // ---- loading the script ---------------------------------------------------------------------
    // require() shim: the script's relative requires are fetched up front, then evaluated
    // synchronously as CommonJS modules when the script runs.
    const sources = new Map(), cache = new Map();
    // Scripts run with the page's origin, so only same-origin blob: URLs (made by the parent page)
    // and files under the folder above this page (the embedding site's /vn/) are allowed.
    function scriptAllowed(url, page) {
        let u;
        try { u = new URL(url); } catch (e) { return false; }
        const pageUrl = new URL(page);
        if (u.protocol === 'blob:') return u.origin === pageUrl.origin;
        // servers decode %2F/%5C and then resolve .., so an encoded separator could leave the folder
        if (/%2f|%5c/i.test(u.pathname)) return false;
        return (u.protocol === 'https:' || u.protocol === 'http:') && u.origin === pageUrl.origin &&
            u.href.startsWith(new URL('../', pageUrl).href);
    }
    async function fetchTree(url) {
        if (sources.has(url)) return;
        if (!scriptAllowed(url, location.href)) throw new Error('frida: script URL refused (not same-origin blob: or under ../): ' + url);
        const res = await fetch(url);
        if (!res.ok) throw new Error('frida: ' + url + ': HTTP ' + res.status);
        if (res.redirected && !scriptAllowed(res.url, location.href)) throw new Error('frida: script redirected off the allowed paths: ' + res.url);
        const text = await res.text();
        sources.set(url, text);
        const deps = [...text.matchAll(/require\(\s*['"](\.{1,2}\/[^'"]+)['"]\s*\)/g)].map((m) => new URL(m[1], url).href);
        await Promise.all(deps.map(fetchTree));
    }
    function evaluate(url, parent = null) {
        if (cache.has(url)) return cache.get(url).exports;
        const module = { exports: {}, parent, id: url }; // libs check module.parent to refuse running alone
        cache.set(url, module);
        const require = (name) => {
            if (!/^\.{1,2}\//.test(name)) throw new Error('frida: require of a non-relative module is not supported: ' + name);
            const dep = new URL(name, url).href;
            if (!sources.has(dep)) throw new Error('frida: module not preloaded: ' + dep);
            return evaluate(dep, module);
        };
        const names = Object.keys(fridaGlobals);
        new Function('require', 'module', 'exports', ...names, sources.get(url) + '\n//# sourceURL=' + url)(require, module, module.exports, ...names.map((n) => fridaGlobals[n]));
        return module.exports;
    }

    const params = typeof location === 'undefined' ? null : new URLSearchParams(location.search);
    const scriptUrl = params && params.get('frida') ? new URL(params.get('frida'), location.href).href : null;
    const exe = lower(params && (params.get('p') || '').split(/[\\/]/).pop());
    let loaded = false, attached = false, phase = 0, seenAt = 0;
    if (scriptUrl) fetchTree(scriptUrl).then(() => { loaded = true; }, (e) => console.error(e));

    // From C every 1024 syscalls, on the guest thread of process pid. 0: nothing, 2: load the
    // ranges asked for with prefetch and poll again, 1: load them and attach.
    function poll(pid, tid, teb) {
        if (!loaded || attached || !teb) return 0;
        F.pid = pid;
        let mods;
        try { mods = modulesOf(teb); } catch (e) { return 0; }
        if (!mods.length || lower(mods[0].name) !== exe) return 0;
        // shortcut: attach 2 s after the exe shows up, like attaching to a running process; scripts
        // that hook a DLL loaded later need a wait or a retry (dc00's do it themselves where needed)
        const now = performance.now();
        if (!seenAt) seenAt = now;
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
        attached = true;
        Object.assign(F, { pid, tid, teb });
        const main = modulesOf(teb)[0];
        // self-check of the C scan and pattern parsing on the exe's DOS header
        const ok = ['4d 5a', '4? 5a', '?? 5a 90', '4d 5a : ff ff'].every((p) => {
            const r = scanSync(main.base, 0x40, p);
            return r.length === 1 && r[0].address.equals(main.base);
        }) && scanSync(main.base, 0x40, '4d 5a 4d').length === 0;
        console.log('[frida] attach pid ' + pid + ' ' + main.name + ' @ ' + main.base + ' size 0x' + main.size.toString(16) + ', scan self-check ' + (ok ? 'ok' : 'FAILED'));
        try {
            evaluate(scriptUrl);
        } catch (e) {
            console.error('[frida] script failed: ' + (e && e.stack || e));
        }
    }
})();
