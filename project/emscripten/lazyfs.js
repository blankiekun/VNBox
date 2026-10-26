// Lazy game folder: the game's files appear in the Emscripten FS as MEMFS nodes whose
// data is read on demand from a FileSystemDirectoryHandle (an OPFS folder, or a folder the
// user picked with showDirectoryPicker). Nothing is copied into memory up front: the tree
// (names and sizes) is listed once at mount, data is read per 64 KiB block through a small
// LRU cache. Two ways to make the emulator's synchronous read wait for the browser's
// asynchronous file APIs:
// - "sab": an IO worker reads with FileReaderSync; the caller (main thread, where the
//   Emscripten FS runs; pthreads proxy their syscalls to it) posts the request into a
//   SharedArrayBuffer and spins until it answers. Needs cross-origin isolation.
// - "jspi" (build with -sJSPI and lazyfs-jspi-lib.js): fd_read/fd_pread suspend the wasm
//   stack on an async File read on the main thread. No worker, no isolation.
// Only the block read knows the source: an HTTP backend (ranged fetch over nginx autoindex
// JSON listings) would replace the listing and the block read and keep the rest.
// shortcut: writes to existing game files fail (EACCES), new files stay in memory; route
// writes to IDBFS/OPFS before saves matter.
var LAZY_BLOCK = 1 << 16;
var LAZY_CACHE_BLOCKS = 64;
var lazyStats = { reads: 0, blocksFetched: 0, bytesFetched: 0, cacheHits: 0, waitMs: 0, files: 0, mode: "" };
window.__lazyStats = lazyStats;
var lazyCache = new Map(); // "file:block" → Uint8Array, insertion order = LRU order
var lazyFiles = []; // jspi mode: File objects by index

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

function lazyKeep(key, bytes) {
    lazyStats.blocksFetched++;
    lazyStats.bytesFetched += bytes.length;
    lazyCache.set(key, bytes);
    if (lazyCache.size > LAZY_CACHE_BLOCKS) lazyCache.delete(lazyCache.keys().next().value);
    return bytes;
}

// Copies node bytes [position, position + length) into buffer[offset…] block by block;
// getBlock(file, block) returns the block's bytes, or a promise of them (jspi).
function lazyCopy(node, buffer, offset, length, position, getBlock) {
    lazyStats.reads++;
    var end = Math.min(node.lazySize, position + length), done = 0;
    var step = () => {
        while (position + done < end) {
            var pos = position + done, block = Math.floor(pos / LAZY_BLOCK);
            var bytes = getBlock(node.lazyFile, block);
            if (bytes instanceof Promise) return bytes.then(step);
            var from = pos - block * LAZY_BLOCK, n = Math.min(bytes.length - from, end - pos);
            if (n <= 0) break;
            if (!buffer.byteLength) buffer = HEAPU8; // memory grew while a jspi read was suspended
            buffer.set(bytes.subarray(from, from + n), offset + done);
            done += n;
        }
        return done;
    };
    return step();
}

// Async block read for jspi mode; the promise settles to the bytes, and step() reads the cache again.
function lazyBlockAsync(file, block) {
    var key = file + ":" + block;
    var hit = lazyCached(key);
    if (hit) return hit;
    var t = performance.now(), off = block * LAZY_BLOCK;
    return lazyFiles[file].slice(off, off + LAZY_BLOCK).arrayBuffer().then(b => {
        lazyStats.waitMs += performance.now() - t;
        lazyKeep(key, new Uint8Array(b));
        lazyStats.cacheHits--; // step() will count the re-read as a hit
    });
}

// fd_read/fd_pread for lazy nodes in jspi mode (lazyfs-jspi-lib.js): reads every iovec, then
// stores the count. Returns a promise, which suspends the wasm caller.
async function lazyReadvAsync(stream, iov, iovcnt, offset) {
    var total = 0, pos = offset === undefined ? stream.position : offset;
    for (var i = 0; i < iovcnt; i++) {
        var ptr = HEAPU32[(iov + i * 8) >> 2], len = HEAPU32[(iov + i * 8 + 4) >> 2];
        var n = await lazyCopy(stream.node, HEAPU8, ptr, len, pos + total, lazyBlockAsync);
        total += n;
        if (n < len) break;
    }
    if (offset === undefined) stream.position += total;
    return total;
}

function lazyAddFile(parent, fileName, index, size, readSync) {
    var node = FS.createFile(parent, fileName, {}, true, false);
    node.lazyFile = index;
    node.lazySize = size;
    node.lazyAsync = !readSync;
    Object.defineProperty(node, "usedBytes", { get: function() { return this.lazySize; } }); // MEMFS getattr/llseek read it
    var read = readSync || (() => { throw new Error("lazy file read outside fd_read/fd_pread in jspi mode"); });
    var ops = Object.assign({}, node.stream_ops);
    ops.read = (stream, buffer, offset, length, position) => read(stream.node, buffer, offset, length, position);
    ops.write = () => { throw new FS.ErrnoError(2); }; // EACCES: read-only game data
    ops.mmap = (stream, length, position) => {
        if (typeof growMemViews === "function") growMemViews();
        var ptr = mmapAlloc(length);
        if (!ptr) throw new FS.ErrnoError(48); // ENOMEM
        read(stream.node, HEAP8, ptr, length, position);
        return { ptr: ptr, allocated: true };
    };
    node.stream_ops = ops;
}

function lazyBuildTree(root, entries, readSync) {
    FS.mkdirTree(root);
    entries.forEach(entry => {
        var path = root + "/" + entry.path;
        if (entry.dir) { FS.mkdirTree(path); return; }
        var slash = path.lastIndexOf("/");
        lazyAddFile(path.substring(0, slash), path.substring(slash + 1), entry.file, entry.size, readSync);
        lazyStats.files++;
    });
    console.log("Lazy game folder " + root + " (" + lazyStats.mode + "): " + lazyStats.files + " files listed");
}

function mountLazyApp(mountPoint, name, dirHandle) {
    lazyStats.mode = typeof boxedwineJspiReads !== "undefined" ? "jspi" : "sab";
    return lazyStats.mode === "jspi" ? mountLazyAppJspi(mountPoint, name, dirHandle) : mountLazyAppSab(mountPoint, name, dirHandle);
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
                entries.push({ path: path, file: lazyFiles.length, size: f.size });
                lazyFiles.push(f);
            }
        }
    };
    await walk(await dirHandle, "");
    lazyBuildTree(mountPoint + "/" + name, entries, null);
}

function mountLazyAppSab(mountPoint, name, dirHandle) {
    var ctl = new Int32Array(new SharedArrayBuffer(32)); // [state 0 idle/1 asked/2 answered, file, offLo, offHi, len, result]
    var data = new Uint8Array(new SharedArrayBuffer(LAZY_BLOCK));
    var worker = new Worker("lazyfs-worker.js");

    function fetchBlock(file, block) {
        var key = file + ":" + block;
        var hit = lazyCached(key);
        if (hit) return hit;
        var off = block * LAZY_BLOCK, t = performance.now();
        ctl[1] = file; ctl[2] = off % 4294967296; ctl[3] = Math.floor(off / 4294967296); ctl[4] = LAZY_BLOCK;
        Atomics.store(ctl, 0, 1);
        Atomics.notify(ctl, 0);
        while (Atomics.load(ctl, 0) !== 2) { } // main thread may not Atomics.wait
        var n = ctl[5];
        Atomics.store(ctl, 0, 0);
        lazyStats.waitMs += performance.now() - t;
        if (n < 0) throw new FS.ErrnoError(29); // EIO
        return lazyKeep(key, data.slice(0, n));
    }

    return new Promise((resolve, reject) => {
        worker.onerror = e => reject(e.message);
        worker.onmessage = e => {
            if (e.data.error) { reject(e.data.error); return; }
            lazyBuildTree(mountPoint + "/" + name, e.data.entries, (node, buffer, offset, length, position) =>
                lazyCopy(node, buffer, offset, length, position, fetchBlock));
            resolve();
        };
        Promise.resolve(dirHandle).then(handle => worker.postMessage({ handle: handle, ctl: ctl.buffer, data: data.buffer }), reject);
    });
}
