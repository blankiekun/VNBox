// Command order and decoder lifetime in vnbox-movie.js, with a fake pl_mpeg (vnbox-movie-decoder.c) and a fake page.
// Run: node vnbox-movie.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
const source = fs.readFileSync(new URL('./vnbox-movie.js', import.meta.url), 'utf8');

// One page: its own decoder load (open it with loadDecoder()), rAF queue and reports.
function page() {
    const t = { frames: [], reports: [], created: 0, destroyed: 0, decodedWith: [], flags: 0, played: 0, loadDecoder: null };
    const D = {
        HEAPU8: new Uint8Array(1 << 21),
        _malloc: () => 16, _free() {},
        _vm_create: () => { t.created++; return 1000 + t.created; },
        _vm_destroy: () => { t.destroyed++; },
        _vm_write() {}, _vm_end() {}, _vm_remaining: () => 1 << 21, _vm_time: () => 0,
        _vm_decode: (m) => { t.decodedWith.push(m); return t.flags; },
        _vm_audio_count: () => 0,
    };
    const canvas = { style: {}, getContext: () => ({}), remove() {} };
    const globals = {
        VnboxMovieDecoder: () => new Promise((resolve) => { t.loadDecoder = () => resolve(D); }),
        vnboxParam: () => null,
        vnboxReport: (source, error) => t.reports.push(source + ': ' + error),
        vnboxMovieBlob: () => new Blob([new Uint8Array(16)]),
        window: { addEventListener() {} },
        document: { createElement: () => canvas, getElementById: () => null, body: { lastChild: { parentNode: { insertBefore() {} } } } },
        requestAnimationFrame: (f) => t.frames.push(f),
        cancelAnimationFrame() { t.frames.length = 0; },
        AudioContext: class {
            state = 'running'; currentTime = 0; destination = {};
            createGain() { t.played++; return { gain: {}, connect() {}, disconnect() {} }; }
        },
    };
    const names = Object.keys(globals);
    const made = new Function(...names, source + '\nreturn { vnboxMovieCommand, vnboxMovies };')(...names.map((n) => globals[n]));
    t.cmd = made.vnboxMovieCommand;
    t.movie = () => made.vnboxMovies.get(1);
    return t;
}
const settle = () => new Promise((r) => setTimeout(r, 0));
const tick = (t) => t.frames.splice(0).forEach((f) => f());
async function opened({ ready }) {
    const t = page();
    assert.equal(t.cmd(1, 'open', 'a.mpg'), 1);
    if (ready) { t.loadDecoder(); await settle(); }
    return t;
}

// Nothing wrong with a plain run, or a run after a stop: the checks below would pass on a movie that never starts.
for (const ready of [false, true]) {
    const t = await opened({ ready });
    t.cmd(1, 'run', '');
    t.cmd(1, 'stop', '');
    t.cmd(1, 'run', '');
    if (!ready) t.loadDecoder();
    await settle();
    assert.equal(t.movie().running, true, 'run, stop, run');
    tick(t);
    assert.deepEqual(t.decodedWith, [1001]);
}
// Bug caught: run is deferred, so stop or pause in the same task (or queued during the first load) came before it: sound over a stopped game.
for (const ready of [false, true]) for (const cmd of ['stop', 'pause']) {
    const t = await opened({ ready });
    t.cmd(1, 'run', '');
    t.cmd(1, cmd, '');
    if (!ready) t.loadDecoder();
    await settle();
    assert.equal(t.movie().running, false, `run, ${cmd}, ready=${ready}`);
    assert.equal(t.frames.length, 0);
    assert.equal(t.played, 0);
}
// Bug caught: run then close started a rAF loop decoding a freed movie, writing near address 0 of the decoder's memory.
{
    const t = await opened({ ready: true });
    t.cmd(1, 'run', '');
    t.cmd(1, 'close', '');
    await settle();
    assert.equal(t.frames.length, 0);
    assert.equal(t.played, 0);
    assert.deepEqual([t.created, t.destroyed], [1, 1]);
}
// Bug caught: closed before the decoder loaded, the load still made a decoder (over 4 MB) that nobody frees; with a run, its sound played to the end.
{
    const t = await opened({ ready: false });
    t.cmd(1, 'run', '');
    t.cmd(1, 'close', '');
    t.loadDecoder();
    await settle();
    assert.equal(t.created, 0);
    assert.equal(t.frames.length, 0);
    assert.equal(t.played, 0);
}
// Bug caught: a file pl_mpeg can't start on never reported its end, and the game waited on a black window for good.
{
    const t = await opened({ ready: true });
    t.cmd(1, 'run', '');
    await settle();
    assert.equal(t.cmd(1, 'status', ''), 0);
    t.flags = 4;
    tick(t);
    assert.equal(t.cmd(1, 'status', ''), 1);
    assert.equal(t.frames.length, 0);
    assert.equal(t.reports.length, 1);
}
console.log('vnbox-movie check ok');
