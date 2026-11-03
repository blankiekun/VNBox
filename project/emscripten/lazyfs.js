// Lazy game folder: the game's files appear in the Emscripten FS as MEMFS nodes whose
// data is read on demand from a FileSystemDirectoryHandle (an OPFS folder, or a folder the
// user picked with showDirectoryPicker). The tree (names and sizes) is listed once at mount, data
// is read per 64 KiB block through a small LRU cache; only small files, up to a budget, are read
// up front (LAZY_EAGER_MAX, LAZY_EAGER_BUDGET). Two ways to make the emulator's synchronous read wait for the browser's
// asynchronous file APIs:
// - "sab": an IO worker reads with FileReaderSync; the caller (main thread, where the
//   Emscripten FS runs; pthreads proxy their syscalls to it) posts the request into a
//   SharedArrayBuffer and spins until it answers. Needs cross-origin isolation.
// - "jspi" (build with -sJSPI and lazyfs-jspi-lib.js): fd_read/fd_pread suspend the wasm
//   stack on an async File read on the main thread. No worker, no isolation.
// Only the block read knows the source: an HTTP backend (ranged fetch over nginx autoindex
// JSON listings) would replace the listing and the block read and keep the rest.
// Writes: the tree is built inside an IDBFS mount (autoPersist), one IndexedDB database per
// game, next to the Wine prefix's. Every game file is a lazy node. The first write or truncate copies a file into a plain MEMFS file (the
// game's own copy, which IDBFS persists); files restored from IndexedDB win over the folder's.
// Untouched game files are kept out of IndexedDB (lazyPristine, see lazyMountOverlay).
// Errors go to the console and to a same-origin parent page as {type: "vnbox-error", source, message}.
// After source "saves", {type: "vnbox-saves-ok"} says every store that failed works again.
// shortcut: a deleted or renamed game file comes back on reload under its old name; add a deleted list if a game needs it.
var LAZY_BLOCK = 1 << 16;
var LAZY_READAHEAD = 4 * LAZY_BLOCK; // read past the requested range when reads are sequential
var LAZY_RUN_MAX = 1 << 22; // longest single read (and the sab buffer)
var LAZY_EAGER_MAX = 1 << 20; // a file up to this size is read whole at mount while the budget lasts
var LAZY_EAGER_BUDGET = 64 << 20; // all of them together: past it they are normal lazy nodes
var LAZY_TAB_WAIT = 3000; // ms: a reload's old page lets go of its storage a moment after the new page asks
var LAZY_CACHE_BLOCKS = 64; // 4 MiB
// win: the stats=1 window (jspi reads), set and taken by the [stats] line every ~2 s; null when stats are off.
var lazyStats = { reads: 0, blocksFetched: 0, bytesFetched: 0, cacheHits: 0, waitMs: 0, files: 0, mode: "", overlay: 0, copied: 0, win: null };
window.__lazyStats = lazyStats;
var lazyCache = new Map(); // "file:block" → Uint8Array, insertion order = LRU order
var lazyFiles = []; // jspi mode: File objects by index
var lazyEagerLeft = LAZY_EAGER_BUDGET;
var lazyFailingStores = new Set(); // mount points reported as failing, until a persist works again
var lazyBase = typeof document !== "undefined" && document.currentScript ? new URL(".", document.currentScript.src).href : ""; // this file's folder, not the page's

function vnboxReport(source, error) {
    var message = String(error && error.stack || error);
    console.error("[" + source + "] " + message);
    vnboxPost({ type: "vnbox-error", source: source, message: message });
}

function vnboxPost(message) {
    try {
        if (window.parent !== window) window.parent.postMessage(message, location.origin);
    } catch (e) { }
}

// A URL param of the player page, decoded; null when absent. Config.urlParams (boxedwine-shell.js) replaces the
// page's query, as in getParameter. The embedding page's URL is not read: pages forward what the player needs.
function vnboxParam(name) {
    var override = typeof Config !== "undefined" && Config.urlParams;
    return new URLSearchParams(override ? "?" + override : location.search).get(name);
}

function lazyOpfsDir(path) { // "vn/albatross" → handle inside the origin private file system
    return path.split("/").filter(Boolean).reduce(
        (dir, name) => dir.then(d => d.getDirectoryHandle(name)), navigator.storage.getDirectory());
}

function lazyCached(key) {
    var hit = lazyCache.get(key);
    if (hit) {
        lazyCache.delete(key);
        lazyCache.set(key, hit);
        lazyStats.cacheHits++;
    }
    return hit;
}

// Splits a run read at file offset start into cached blocks; returns it for the reader.
function lazyKeepRun(file, start, bytes) {
    for (var i = 0; i < bytes.length; i += LAZY_BLOCK) {
        lazyCache.delete(file + ":" + (start + i) / LAZY_BLOCK);
        lazyCache.set(file + ":" + (start + i) / LAZY_BLOCK, bytes.slice(i, i + LAZY_BLOCK));
        lazyStats.blocksFetched++;
    }
    lazyStats.bytesFetched += bytes.length;
    if (lazyStats.win) { lazyStats.win.fetches++; lazyStats.win.fetchBytes += bytes.length; }
    while (lazyCache.size > LAZY_CACHE_BLOCKS) lazyCache.delete(lazyCache.keys().next().value);
    return { start: start, bytes: bytes };
}

// Copies node bytes [position, position + length) into buffer[offset…]. On a cache miss
// readRun(file, start, length) reads from the missing block to the end of the range plus
// LAZY_READAHEAD (when the miss starts where the file's last read ended) in one go, and returns
// the bytes or (jspi) a promise of them.
function lazyCopy(node, buffer, offset, length, position, readRun) {
    lazyStats.reads++;
    var end = Math.min(node.lazySize, position + length), done = 0, run = null, short = false;
    var inRun = pos => run && pos >= run.start && pos < run.start + run.bytes.length;
    var step = () => {
        while (position + done < end) {
            var pos = position + done, bytes, from;
            if (node.lazyBytes) {
                bytes = node.lazyBytes; from = pos;
            } else if (inRun(pos)) {
                bytes = run.bytes; from = pos - run.start;
            } else {
                var block = Math.floor(pos / LAZY_BLOCK), hit = lazyCached(node.lazyFile + ":" + block);
                if (hit) {
                    bytes = hit; from = pos - block * LAZY_BLOCK;
                } else {
                    if (short) break; // the last run came back short: the file shrank
                    var start = block * LAZY_BLOCK;
                    var ahead = node.lazyNext === start ? LAZY_READAHEAD : 0;
                    var stop = Math.min(node.lazySize, start + LAZY_RUN_MAX, Math.ceil(end / LAZY_BLOCK) * LAZY_BLOCK + ahead);
                    node.lazyNext = stop;
                    var r = readRun(node.lazyFile, start, stop - start);
                    var got = b => { short = b.length < stop - start; run = lazyKeepRun(node.lazyFile, start, b); };
                    if (r instanceof Promise) return r.then(b => (got(b), step()));
                    got(r);
                    continue;
                }
            }
            var n = Math.min(bytes.length - from, end - pos);
            if (n <= 0) break;
            if (!buffer.byteLength) buffer = HEAPU8; // memory grew while a jspi read was suspended
            buffer.set(bytes.subarray(from, from + n), offset + done);
            done += n;
        }
        return done;
    };
    return step();
}

// Async run read for jspi mode.
function lazyReadRunAsync(file, start, length) {
    var t = performance.now();
    return lazyFiles[file].slice(start, start + length).arrayBuffer().then(b => {
        lazyStats.waitMs += performance.now() - t;
        return new Uint8Array(b);
    });
}

// fd_read/fd_pread for lazy nodes in jspi mode (lazyfs-jspi-lib.js): reads every iovec, then
// stores the count. Returns a promise, which suspends the wasm caller.
async function lazyReadvAsync(stream, iov, iovcnt, offset) {
    var total = 0, pos = offset === undefined ? stream.position : offset, t = performance.now();
    for (var i = 0; i < iovcnt; i++) {
        var ptr = HEAPU32[(iov + i * 8) >> 2], len = HEAPU32[(iov + i * 8 + 4) >> 2];
        var n = await lazyCopy(stream.node, HEAPU8, ptr, len, pos + total, lazyReadRunAsync);
        total += n;
        if (n < len) break;
    }
    if (offset === undefined) stream.position += total;
    var w = lazyStats.win;
    if (w) { // the whole suspension, cache hits included: the guest slice waits all of it
        var ms = performance.now() - t, name = stream.node.name;
        w.reads++; w.bytes += total; w.ms += ms; w.files[name] = (w.files[name] || 0) + ms;
    }
    return total;
}

// Turns a lazy node into a plain MEMFS file holding bytes: the game's own copy, which IDBFS persists.
function lazyBecomeMemfs(node, bytes) {
    delete node.usedBytes; // the getter
    node.contents = bytes;
    node.usedBytes = bytes.length;
    node.lazyCopied = true;
    node.lazyAsync = false;
    node.lazyBytes = null;
    delete node.lazyPristine;
    lazyStats.copied++;
}

// Reads a File synchronously on the main thread: a sync XHR of a blob: URL, bytes as a binary string.
// shortcut: holds the file twice in memory for a moment; fine for the rare first write to a big
// game file, a block overlay would be needed for games rewriting huge files in place.
function lazyReadFileSync(file) {
    var url = URL.createObjectURL(file);
    try {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", url, false);
        xhr.overrideMimeType("text/plain; charset=x-user-defined");
        xhr.send();
        var text = xhr.responseText, bytes = new Uint8Array(text.length);
        for (var i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i) & 255;
        return bytes;
    } finally {
        URL.revokeObjectURL(url);
    }
}

// Copy on first write or truncate. empty: the copy starts empty (O_TRUNC), nothing is read.
function lazyCopyUp(node, empty) {
    if (node.lazyCopied) return;
    if (empty) return lazyBecomeMemfs(node, new Uint8Array(0));
    var bytes;
    if (node.lazyAsync) {
        bytes = lazyReadFileSync(lazyFiles[node.lazyFile]);
        if (bytes.length !== node.lazySize) {
            vnboxReport("lazyfs", "copy of " + node.name + " read " + bytes.length + " of " + node.lazySize + " bytes");
            throw new FS.ErrnoError(29); // EIO
        }
    } else {
        bytes = new Uint8Array(node.lazySize);
        if (node.lazyBytes) bytes.set(node.lazyBytes);
        else node.lazyReadSync(node, bytes, 0, node.lazySize, 0);
    }
    lazyBecomeMemfs(node, bytes);
}

// Takes size bytes of the eager budget; false: read this file lazily.
function lazyEagerTake(size) {
    if (size > LAZY_EAGER_MAX || size > lazyEagerLeft) return false;
    lazyEagerLeft -= size;
    return true;
}

// A game file as a lazy MEMFS node. bytes: the whole file, read at mount (small files).
// readSync: sab mode's block reader; null in jspi mode, where fd_read/fd_pread read it.
function lazyAddFile(parent, fileName, index, size, readSync, bytes) {
    var node = FS.createFile(parent, fileName, {}, true, true);
    node.lazyPristine = true;
    node.lazyFile = index;
    node.lazySize = size;
    node.lazyBytes = bytes || null;
    node.lazyAsync = !readSync && !bytes;
    node.lazyReadSync = readSync;
    Object.defineProperty(node, "usedBytes", { configurable: true, get: function() { return this.lazySize; } }); // MEMFS getattr/llseek read it
    var memfs = node.stream_ops, setattr = node.node_ops.setattr;
    var read = (n, buffer, offset, length, position) => {
        if (n.lazyBytes) return lazyCopy(n, buffer, offset, length, position, null);
        if (n.lazyReadSync) return n.lazyReadSync(n, buffer, offset, length, position);
        throw new Error("lazy file read outside fd_read/fd_pread in jspi mode");
    };
    var ops = Object.assign({}, memfs); // open streams keep these ops after a copy-up, so each checks
    ops.read = (stream, buffer, offset, length, position) =>
        stream.node.lazyCopied ? memfs.read(stream, buffer, offset, length, position) : read(stream.node, buffer, offset, length, position);
    ops.write = (stream, ...a) => (lazyCopyUp(stream.node, false), memfs.write(stream, ...a));
    ops.mmap = (stream, length, position, prot, flags) => {
        var n = stream.node;
        if (n.lazyCopied) return memfs.mmap(stream, length, position, prot, flags);
        if (n.lazyAsync) { // a synchronous whole-range read is impossible in jspi mode
            vnboxReport("lazyfs", "mmap of " + n.name + " (" + n.lazySize + " bytes) is not supported for lazy files over " + LAZY_EAGER_MAX + " bytes");
            throw new FS.ErrnoError(43); // ENODEV
        }
        if (typeof growMemViews === "function") growMemViews();
        var ptr = mmapAlloc(length);
        if (!ptr) throw new FS.ErrnoError(48); // ENOMEM
        read(n, HEAP8, ptr, length, position);
        return { ptr: ptr, allocated: true };
    };
    node.stream_ops = ops;
    node.node_ops = Object.assign({}, node.node_ops, {
        setattr: (n, attr) => { if (attr.size !== undefined) lazyCopyUp(n, attr.size === 0); return setattr(n, attr); },
    });
    return node;
}

// jspi mode: fetches url into the origin private file system once (lazyfs-cache/), then adds
// it at dir/name as a lazy node read from there, so a root or overlay zip stays out of the JS heap.
// The copy is keyed by the zip's sha256 from vnbox-zips.sha256 (next to the page, written by the
// release), so a new release's zip replaces the old copy.
// shortcut: a zip missing from vnbox-zips.sha256 is kept by name; a changed one needs a new name.
var lazyZipSums = null;
function lazyZipSum(name) {
    lazyZipSums = lazyZipSums || fetch("vnbox-zips.sha256").then(r => r.ok ? r.text() : "", () => "").then(text => {
        var sums = {};
        text.split("\n").forEach(line => { var m = /^([0-9a-f]{64}) [ *](.+)$/.exec(line.trim()); if (m) sums[m[2]] = m[1]; });
        return sums;
    });
    return lazyZipSums.then(sums => sums[name] || "");
}
async function lazyAddCachedFile(dir, name, url) {
    var cache = await (await navigator.storage.getDirectory()).getDirectoryHandle("lazyfs-cache", { create: true });
    var encoded = encodeURIComponent(name), sha = await lazyZipSum(name);
    var cacheName = sha ? sha + "~" + encoded : encoded, file = null;
    try { file = await (await cache.getFileHandle(cacheName)).getFile(); } catch (e) { }
    if (!file || !file.size) { // an interrupted download leaves the file empty
        var response = await fetch(url);
        if (!response.ok) throw new Error("HTTP " + response.status);
        var handle = await cache.getFileHandle(cacheName, { create: true });
        await response.body.pipeTo(await handle.createWritable());
        file = await handle.getFile();
        for await (var key of cache.keys()) { // older copies of this zip
            if (key !== cacheName && (key === encoded || (/^[0-9a-f]{64}~/.test(key) && key.substring(65) === encoded))) {
                await cache.removeEntry(key).catch(() => { });
            }
        }
    }
    lazyAddFile(dir, name, lazyFiles.length, file.size, null, null);
    lazyFiles.push(file);
}

// One path part as Boxedwine names it when the game creates it (Fs::localNameToRemote): game files
// from the folder keep their raw names, so one local name can have two native spellings.
function lazyEscapeName(name) {
    var bytes = new TextEncoder().encode(name);
    return Array.from(bytes, (c, i) => {
        var end = i === bytes.length - 1;
        if (c === 0x3a) return "(_colon_)";
        if (c === 0x3f) return "(_question_)";
        if (c === 0x40) return "(_at_)";
        if (c === 0x2e && end) return "(_dot_)";
        if (c === 0x20 && end) return "(_space_)";
        if (c >= 0x80) return "(_x" + c.toString(16).toUpperCase() + "_)";
        return String.fromCharCode(c);
    }).join("");
}

// Where a game file goes in the tree: under the spelling the overlay already uses for each part. Both
// spellings side by side would decode to one local name and the raw one, added last, would hide the save.
function lazyOverlayPath(root, relative) {
    return relative.split("/").reduce((dir, part) => {
        var raw = dir + "/" + part, escaped = dir + "/" + lazyEscapeName(part);
        return escaped !== raw && !FS.analyzePath(raw).exists && FS.analyzePath(escaped).exists ? escaped : raw;
    }, root);
}

function lazyBuildTree(root, entries, readSync) {
    FS.mkdirTree(root);
    entries.forEach(entry => {
        var path = lazyOverlayPath(root, entry.path);
        if (entry.dir) { FS.mkdirTree(path); return; }
        var slash = path.lastIndexOf("/"), parent = path.substring(0, slash), fileName = path.substring(slash + 1);
        lazyStats.files++;
        if (FS.analyzePath(path).exists) { lazyStats.overlay++; return; } // the game's own copy from IndexedDB
        var bytes = entry.bytes || null;
        if (!bytes && readSync && lazyEagerTake(entry.size)) { // sab mode
            bytes = new Uint8Array(entry.size);
            readSync({ lazyFile: entry.file, lazySize: entry.size }, bytes, 0, entry.size, 0);
        }
        lazyAddFile(parent, fileName, entry.file, entry.size, readSync, bytes).lazyBlob = entry.blob || null;
    });
    console.log("Lazy game folder " + root + " (" + lazyStats.mode + "): " + lazyStats.files + " files listed, " + lazyStats.overlay + " from the saved overlay");
}

// A renamed file is stored under its new name only if it is no longer pristine.
function lazyCopyUpTree(node) {
    if (node.lazyPristine) lazyCopyUp(node, false);
    else if (FS.isDir(node.mode)) Object.values(node.contents).forEach(lazyCopyUpTree);
}

// A failed persist: the next try opens a new connection (a dead one stays cached otherwise), and it is
// retried for as long as it fails, since the data is only in memory. A file removed between the listing
// and the copy (the game saving by temp file and rename) is normal, the rest is reported once per run of
// failures, from the second on: a closed connection recovers by itself and needs no warning.
function lazyPersistFailed(mount, err) {
    var db = IDBFS.dbs[mount.mountpoint];
    if (db) {
        delete IDBFS.dbs[mount.mountpoint];
        try { db.close(); } catch (e) { }
    }
    var vanished = err.name === "ErrnoError";
    console.warn("Persist of " + mount.mountpoint + " failed: " + (vanished ? "errno " + err.errno : err));
    if (!vanished && ++mount.lazyPersistFails === 2) {
        lazyFailingStores.add(mount.mountpoint);
        vnboxReport("saves", "Saves could not be stored: " + err);
    }
    if (!mount.lazyRetry) {
        mount.lazyRetry = setTimeout(() => {
            mount.lazyRetry = 0;
            IDBFS.queuePersist(mount);
        }, mount.lazyPersistFails > 1 ? 30000 : 5000);
    }
}

// What saves need from IDBFS, which calls all of these by name.
function lazyPatchIdbfs() {
    if (IDBFS.lazyPatched) return;
    IDBFS.lazyPatched = true;
    var queuePersist = IDBFS.queuePersist, syncfs = IDBFS.syncfs, reconcile = IDBFS.reconcile, mountIdbfs = IDBFS.mount;
    // A restore goes through the mknod and close hooks of a game's write. A persist started by them sees
    // only the files restored so far and deletes the others from the database. A failed restore stays
    // off: what is in memory then is not what is stored.
    IDBFS.queuePersist = mount => { if (!mount.lazyRestoring) queuePersist(mount); };
    IDBFS.syncfs = (mount, populate, done) => {
        if (populate) mount.lazyRestoring = true;
        syncfs(mount, populate, err => {
            if (populate) mount.lazyRestoring = !!err;
            else if (err) lazyPersistFailed(mount, err);
            else {
                mount.lazyPersistFails = 0;
                if (lazyFailingStores.delete(mount.mountpoint) && !lazyFailingStores.size) vnboxPost({ type: "vnbox-saves-ok" });
            }
            done(err);
        });
    };
    // Runs in an IndexedDB event: a throw from a just closed connection would leave the persist waiting forever.
    IDBFS.reconcile = (src, dst, callback) => {
        try { reconcile(src, dst, callback); } catch (e) { callback(e); }
    };
    IDBFS.mount = mount => {
        var root = mountIdbfs(mount);
        if (!mount.opts?.autoPersist) return root;
        mount.lazyPersistFails = 0;
        var ops = root.node_ops, setattr = ops.setattr, rename = ops.rename; // ops is what every node of the mount shares
        // MEMFS leaves the time on a truncate, and a persist compares only times.
        ops.setattr = (node, attr) => {
            if (attr.size !== undefined) {
                node.mtime = node.ctime = Math.max(Date.now(), node.mtime + 1);
                IDBFS.queuePersist(mount);
            }
            return setattr(node, attr);
        };
        ops.rename = (node, newDir, newName) => {
            try {
                lazyCopyUpTree(node);
            } catch (e) {
                if (e.name === "ErrnoError") throw e;
                vnboxReport("lazyfs", "copy of " + node.name + " for a rename failed: " + e); // too big for memory: the rename fails, nothing is lost
                throw new FS.ErrnoError(29); // EIO
            }
            return rename(node, newDir, newName);
        };
        return root;
    };
}

// One tab per game: each tab holds the saves in memory and a persist makes the database match its copy, so
// a second tab would delete what the first saved. names: the databases this page will use; the locks last
// as long as the page. Without Web Locks (not a secure context) nothing can tell, so it goes ahead.
function lazyHoldStores(names, onHeld) {
    if (typeof navigator === "undefined" || !navigator.locks) {
        if (names.length) console.warn("No Web Locks here: a second tab on this game could overwrite its saves");
        onHeld();
        return;
    }
    var next = i => {
        if (i === names.length) { onHeld(); return; }
        navigator.locks.request("vnbox:" + names[i], { signal: AbortSignal.timeout(LAZY_TAB_WAIT) }, () => {
            setTimeout(next, 0, i + 1); // out of the callback: a throw there would read as a lock not granted
            return new Promise(() => { });
        }).catch(e => {
            if (e.name === "TimeoutError" || e.name === "AbortError") vnboxReport("lazyfs", "This game is already open in another tab");
            else { console.warn("Web Locks failed: " + e); next(i + 1); }
        });
    };
    next(0);
}

// Mounts IDBFS (database named after mountPoint) and loads the game's saved files into it.
function lazyMountOverlay(mountPoint) {
    if (!IDBFS.lazyFiltered) { // persist only files the game wrote, not the folder's own
        var getLocalSet = IDBFS.getLocalSet;
        IDBFS.getLocalSet = (mount, callback) => getLocalSet(mount, (err, set) => {
            if (set && mount.mountpoint.startsWith(LAZY_APP_MOUNT_ROOT)) {
                for (var path in set.entries) if ((FS.analyzePath(path).object || {}).lazyPristine) delete set.entries[path];
            }
            callback(err, set);
        });
        IDBFS.lazyFiltered = true;
    }
    FS.mkdirTree(mountPoint);
    var mount = FS.mount(IDBFS, { autoPersist: true }, mountPoint).mount;
    return new Promise((resolve, reject) => IDBFS.syncfs(mount, true, err => err ? reject(err) : resolve()));
}

// A disc image's folder ("vnbox/discs/<id>": the image and its CUE/BINs or MDS/MDF) as lazy nodes
// under LAZY_DISC_MOUNT, read-only and kept out of IndexedDB. Each folder is mounted once.
var LAZY_DISC_MOUNT = "/lazydisc";
var lazyDiscs = {};
function mountLazyDisc(folder) {
    var parts = folder.split("/").filter(Boolean), key = parts.join("/"), name = parts.pop();
    if (!lazyDiscs[key]) {
        lazyStats.mode = typeof boxedwineJspiReads !== "undefined" ? "jspi" : "sab";
        var mountPoint = [LAZY_DISC_MOUNT].concat(parts).join("/");
        FS.mkdirTree(mountPoint);
        lazyDiscs[key] = (lazyStats.mode === "jspi" ? mountLazyAppJspi : mountLazyAppSab)(mountPoint, name, lazyOpfsDir(key));
    }
    return lazyDiscs[key];
}

var LAZY_APP_MOUNT_ROOT = "/lazyapp/";
function mountLazyApp(mountPoint, name, dirHandle) {
    lazyStats.mode = typeof boxedwineJspiReads !== "undefined" ? "jspi" : "sab";
    return lazyMountOverlay(mountPoint).then(() =>
        lazyStats.mode === "jspi" ? mountLazyAppJspi(mountPoint, name, dirHandle) : mountLazyAppSab(mountPoint, name, dirHandle));
}

async function mountLazyAppJspi(mountPoint, name, dirHandle) {
    var entries = [];
    var walk = async (dir, prefix) => {
        for await (var [entryName, h] of dir.entries()) {
            var path = prefix + entryName;
            if (h.kind === "directory") {
                entries.push({ path: path, dir: true });
                await walk(h, path + "/");
            } else {
                var f = await h.getFile();
                entries.push({ path: path, file: lazyFiles.length, size: f.size, bytes: lazyEagerTake(f.size) ? new Uint8Array(await f.arrayBuffer()) : null });
                lazyFiles.push(f);
            }
        }
    };
    await walk(await dirHandle, "");
    lazyBuildTree(mountPoint + "/" + name, entries, null);
}

function mountLazyAppSab(mountPoint, name, dirHandle) {
    var ctl = new Int32Array(new SharedArrayBuffer(32)); // [state 0 idle/1 asked/2 answered, file, offLo, offHi, len, result]
    var data = new Uint8Array(new SharedArrayBuffer(LAZY_RUN_MAX));
    var worker = new Worker(lazyBase + "lazyfs-worker.js");

    function readRun(file, start, length) {
        var t = performance.now();
        ctl[1] = file; ctl[2] = start % 4294967296; ctl[3] = Math.floor(start / 4294967296); ctl[4] = length;
        Atomics.store(ctl, 0, 1);
        Atomics.notify(ctl, 0);
        while (Atomics.load(ctl, 0) !== 2) { } // main thread may not Atomics.wait
        var n = ctl[5];
        Atomics.store(ctl, 0, 0);
        lazyStats.waitMs += performance.now() - t;
        if (n < 0) {
            vnboxReport("lazyfs", "read of file " + file + " at " + start + " failed");
            throw new FS.ErrnoError(29); // EIO
        }
        return data.slice(0, n);
    }

    return new Promise((resolve, reject) => {
        worker.onerror = e => reject(e.message);
        worker.onmessage = e => {
            if (e.data.error) { reject(e.data.error); return; }
            lazyBuildTree(mountPoint + "/" + name, e.data.entries, (node, buffer, offset, length, position) =>
                lazyCopy(node, buffer, offset, length, position, readRun));
            resolve();
        };
        Promise.resolve(dirHandle).then(handle => worker.postMessage({ handle: handle, ctl: ctl.buffer, data: data.buffer }), reject);
    });
}
