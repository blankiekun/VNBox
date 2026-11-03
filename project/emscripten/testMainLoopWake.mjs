// The browser main loop (source/sdl/emscripten/mainloop.cpp), run as shipped with the browser's clocks and the
// wasm tick stubbed. That input gets past the page's own handlers to this loop is testBrowserMouseButtons.mjs.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(process.argv[2] || new URL('../../source/sdl/emscripten/mainloop.cpp', import.meta.url), 'utf8');
const start = source.indexOf('var tick = WebAssembly.promising');
const end = source.indexOf('    });', source.indexOf('requestAnimationFrame(frame);\n        next();', start));
assert(start >= 0 && end > start);
const code = source.slice(start, end).replace(/#ifdef[\s\S]*?#endif/g, '');

function browser({hidden = false} = {}) {
    let now = 0, ids = 0;
    const timers = new Map(), frames = new Map(), messages = [], listeners = new Map(), ticks = [];
    const failed = [], reported = [], hiddenCalls = [];
    const flush = () => new Promise(resolve => setImmediate(resolve));
    const channel = {port1: {onmessage: null}, port2: {postMessage: () => messages.push(1)}};
    const document = {hidden, addEventListener: (type, f) => listeners.set(type, f)};
    vm.runInContext(code, vm.createContext({
        document, performance: {now: () => now},
        window: {addEventListener: (type, f) => listeners.set(type, f)},
        MessageChannel: function() { return channel; },
        WebAssembly: {promising: () => () => new Promise((resolve, reject) => ticks.push({resolve, reject}))},
        wasmExports: {boxedwine_tick() {}, vnbox_set_hidden: h => hiddenCalls.push(h)},
        setTimeout: (f, ms) => (timers.set(++ids, {f, at: now + ms}), ids),
        clearTimeout: id => timers.delete(id),
        requestAnimationFrame: f => (frames.set(++ids, f), ids),
        cancelAnimationFrame: id => frames.delete(id),
        gameFailed: text => failed.push(text), vnboxReport: (source, text) => reported.push(source),
    }));
    return {document, ticks, timers, frames, messages, failed, reported, hiddenCalls, listeners,
        // Ends the running tick, with the delay it asked for or an error, then lets the page react.
        async finish(delay) { ticks.at(-1).resolve(delay); await flush(); },
        async fail(error) { ticks.at(-1).reject(error); await flush(); },
        // Runs what is due up to ms from now: timers, then posted messages.
        async advance(ms) {
            now += ms;
            for (const [id, timer] of [...timers]) if (timer.at <= now) { timers.delete(id); timer.f(); }
            while (messages.length) { messages.pop(); channel.port1.onmessage(); }
            await flush();
        },
        async paint() { for (const [id, f] of [...frames]) { frames.delete(id); f(); } await this.advance(0); },
        input: type => listeners.get(type)(),
        hide(h) { document.hidden = h; listeners.get('visibilitychange')(); },
    };
}

for (const type of ['keydown', 'keyup', 'mousedown', 'mouseup', 'wheel', 'touchstart', 'touchend', 'touchmove']) {
    // The bug: input during an idle wait sat until the timer, a click answered up to a whole wait late.
    test(`${type} during an idle wait runs the next tick at once`, async () => {
        const b = browser();
        await b.finish(1000);
        assert.equal(b.ticks.length, 1);
        b.input(type);
        await b.advance(0);
        assert.equal(b.ticks.length, 2);
        assert.equal(b.timers.size, 0); // the long wait is gone, or it would run one more tick later
    });
}

// The bug it would catch: a tick per event, which pegs the CPU for moving the mouse over a waiting screen.
test('mouse motion does not wake an idle wait', async () => {
    const b = browser();
    await b.finish(1000);
    assert.equal(b.listeners.has('mousemove'), false);
});

// The bug: a second tick entering wasm while the first is suspended corrupts the guest (see the comment in mainloop.cpp).
test('input while a tick is running does not start another', async () => {
    const b = browser();
    b.input('mousedown');
    await b.advance(1000);
    assert.equal(b.ticks.length, 1);
    await b.finish(1000);
    await b.advance(1000);
    assert.equal(b.ticks.length, 2);
    b.input('keydown'); // the timer has fired: this tick is the running one
    await b.advance(1000);
    assert.equal(b.ticks.length, 2);
});

// The bug: a guest with work queued behind a frame that never comes, stopped for as long as the page is hidden.
test('hiding the page ends a wait for a frame, and hidden ticks wait for none', async () => {
    const b = browser();
    await b.advance(20);
    await b.finish(-1);
    assert.equal(b.frames.size, 2); // the frame loop's and the wait's
    await b.advance(0);
    assert.equal(b.ticks.length, 1);
    b.hide(true);
    assert.deepEqual(b.hiddenCalls.at(-1), 1);
    await b.advance(0);
    assert.equal(b.ticks.length, 2);
    await b.advance(20);
    await b.finish(-1);
    await b.advance(0);
    assert.equal(b.ticks.length, 3);
});

// The bug: a page hidden while it sleeps, then shown: its next tick waits out the whole idle timer with a stale screen.
test('showing the page wakes an idle wait', async () => {
    const b = browser({hidden: true});
    assert.deepEqual(b.hiddenCalls, [1]); // a player opened in a background tab
    await b.finish(1000);
    b.hide(false);
    assert.deepEqual(b.hiddenCalls, [1, 0]);
    await b.advance(0);
    assert.equal(b.ticks.length, 2);
});

// The bug: Firefox gives a display:none iframe no frames with document.hidden false: without the limit the guest stops for good.
test('a frame that never comes ends the wait at 100 ms, and no tick waits again until a frame comes', async () => {
    const b = browser();
    await b.advance(20);
    await b.finish(-1);
    await b.advance(99);
    assert.equal(b.ticks.length, 1);
    await b.advance(1);
    assert.equal(b.ticks.length, 2);
    await b.advance(20);
    await b.finish(-1); // no frames: straight on
    await b.advance(0);
    assert.equal(b.ticks.length, 3);
    await b.paint();    // frames are back: the wait is back
    await b.advance(20);
    await b.finish(-1);
    await b.advance(0);
    assert.equal(b.ticks.length, 3);
});

// The bug: a loop going on after the wasm trapped, or a dead page with no message.
test('a failed tick says so and is the last', async () => {
    const b = browser();
    await b.fail(new Error('trap'));
    assert.match(b.failed[0], /trap/);
    assert.deepEqual(b.reported, ['tick']);
    await b.advance(1000);
    await b.paint();
    assert.equal(b.ticks.length, 1);
    assert.equal(b.frames.size, 0);
});
