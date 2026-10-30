// IO worker for lazyfs.js: lists a FileSystemDirectoryHandle once, then answers block reads
// posted into a SharedArrayBuffer. File objects come from getFile() (works for OPFS and for
// picked folders alike) and are read with FileReaderSync on a slice.
// shortcut: File snapshots are cached for the session; a game folder changed on disk while
// running reads stale (NotReadableError → EIO).
onmessage = async e => {
    var { handle, ctl: ctlBuffer, data: dataBuffer } = e.data;
    var ctl = new Int32Array(ctlBuffer), data = new Uint8Array(dataBuffer);
    var handles = [], files = [], entries = [];
    try {
        var walk = async (dir, prefix) => {
            for await (var [name, h] of dir.entries()) {
                var path = prefix + name;
                if (h.kind === "directory") {
                    entries.push({ path: path, dir: true });
                    await walk(h, path + "/");
                } else {
                    var f = await h.getFile();
                    entries.push({ path: path, file: handles.length, size: f.size, blob: f }); // blob: the page hands movies to its decoder
                    handles.push(h); files.push(f);
                }
            }
        };
        await walk(handle, "");
    } catch (err) {
        postMessage({ error: String(err) });
        return;
    }
    postMessage({ entries: entries });
    var reader = new FileReaderSync();
    for (;;) {
        Atomics.wait(ctl, 0, 0);
        if (Atomics.load(ctl, 0) !== 1) continue;
        var i = ctl[1], off = ctl[2] + ctl[3] * 4294967296, len = ctl[4], n;
        try {
            var bytes = new Uint8Array(reader.readAsArrayBuffer(files[i].slice(off, off + len)));
            data.set(bytes);
            n = bytes.length;
        } catch (err) {
            try { files[i] = await handles[i].getFile(); } catch (_) { } // retried on the next read
            n = -1;
        }
        ctl[5] = n;
        Atomics.store(ctl, 0, 2);
    }
};
