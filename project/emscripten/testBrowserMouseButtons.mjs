import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(process.argv[2] || new URL('../../platform/sdl/knativeinputSDL.cpp', import.meta.url), 'utf8');
const start = source.indexOf('    var capturingMouse =');
const end = source.indexOf('    })());', start);
assert(start >= 0 && end > start);

function browser(threaded) {
    // Run the production handlers with only DOM geometry and the WASM boundary
    // stubbed. Test both preprocessor branches used by the browser builds.
    const code = source.slice(start, end).replace(
        /#if !defined\(BOXEDWINE_MULTI_THREADED\)([\s\S]*?)#endif/g,
        (_, body) => threaded ? '' : body);
    const windowHandlers = new Map();
    const documentHandlers = new Map();
    const events = [];
    const keys = [];
    const mods = [];
    const canvas = {
        width: 640, height: 480, focus() {}, addEventListener() {},
        getBoundingClientRect: () => ({left: 100, top: 50, right: 740, bottom: 530, width: 640, height: 480})
    };
    const document = {
        pointerLockElement: null, hidden: false,
        getElementById: () => null,
        addEventListener: (type, callback) => documentHandlers.set(type, callback)
    };
    const context = vm.createContext({canvas, document,
        window: {addEventListener(type, callback) {
            windowHandlers.set(type, [...(windowHandlers.get(type) || []), callback]);
        }},
        _boxedwineEmscriptenMouseButton: (down, button, x, y) => events.push({down, button, x, y}),
        _boxedwineEmscriptenMouseMove() {}, _boxedwineEmscriptenPointerLock() {},
        _boxedwineEmscriptenKey: (scanCode, key, down) => keys.push([scanCode, down]),
        _boxedwineEmscriptenModifiers: value => mods.push(value)
    });
    vm.runInContext(code, context);
    // Listeners on window run in the order they were added; only stopImmediatePropagation ends the list.
    const dispatch = (type, event) => {
        let consumed = false, immediate = false;
        for (const callback of windowHandlers.get(type) || []) {
            callback({getModifierState: name => !!event.locks?.includes(name), ...event, preventDefault() {},
                stopPropagation() {consumed = true;},
                stopImmediatePropagation() {consumed = immediate = true;}});
            if (immediate) break;
        }
        return consumed;
    };
    return {events, keys, mods, context,
        send: (type, button = 0, clientX = 300, clientY = 250) => dispatch(type, {button, clientX, clientY}),
        key: (type, code, event = {}) => dispatch(type, {code, key: '', target: {tagName: 'CANVAS'}, ...event}),
        // A listener added after the page's, as the main loop's wake and audio's resume are.
        later(type) {
            const calls = {count: 0};
            windowHandlers.get(type).push(() => calls.count++);
            return calls;
        },
        hide() {
            document.hidden = true;
            documentHandlers.get('visibilitychange')();
        },
        lock(enabled) {
            document.pointerLockElement = enabled ? canvas : null;
            documentHandlers.get('pointerlockchange')();
        }
    };
}

for (const threaded of [false, true]) {
    const mode = threaded ? 'MT' : 'ST';
    test(`${mode}: a held button produces no release during motion`, () => {
        const b = browser(threaded);
        assert.equal(b.send('mousedown'), true);
        for (let i = 0; i < 100; i++) b.send('mousemove', 0, 300 + i, 250);
        assert.deepEqual(b.events, [{down: 1, button: 0, x: 200, y: 200}]);
        b.send('mouseup');
        assert.deepEqual(b.events.at(-1), {down: 0, button: 0, x: 200, y: 200});
    });

    for (const first of [0, 2]) {
        test(`${mode}: both flippers release independently, ${first} first`, () => {
            const b = browser(threaded);
            b.send('mousedown', 0);
            b.send('mousedown', 2);
            b.send('mouseup', first);
            assert.equal(b.send('mousemove', 0, 900, 700), true, 'Remaining button keeps capture outside canvas');
            assert.equal(b.send('mouseup', 2 - first, 900, 700), true);
            assert.deepEqual(b.events.map(e => [e.down, e.button]), [[1, 0], [1, 1], [0, first / 2], [0, (2 - first) / 2]]);
            assert.deepEqual(b.events.at(-1), {down: 0, button: (2 - first) / 2, x: 639, y: 479});
            assert.equal(b.send('mouseup', 2 - first), false, 'Duplicate release is ignored');
        });
    }

    test(`${mode}: middle and side buttons retain their identities`, () => {
        const b = browser(threaded);
        for (const button of [1, 3, 4]) {
            b.send('mousedown', button);
            b.send('mouseup', button);
        }
        assert.deepEqual(b.events.map(e => [e.down, e.button]), [[1, 2], [0, 2], [1, 3], [0, 3], [1, 4], [0, 4]]);
        assert.equal(b.send('mousedown', 5), false);
        assert.equal(b.send('mousedown', 0, 90, 250), false);
        assert.equal(b.events.length, 6);
    });

    test(`${mode}: losing focus releases each captured button once`, () => {
        const b = browser(threaded);
        b.send('mousedown', 0);
        b.send('mousedown', 2);
        b.send('mousemove', 0, 400, 300);
        b.send('blur');
        b.send('blur');
        assert.deepEqual(b.events.slice(2), [
            {down: 0, button: 0, x: 300, y: 250},
            {down: 0, button: 1, x: 300, y: 250}
        ]);
        assert.equal(b.send('mouseup'), false);
    });

    test(`${mode}: pointer lock releases absolute capture and passes input to SDL`, () => {
        const b = browser(threaded);
        b.send('mousedown');
        b.lock(true);
        assert.deepEqual(b.events.map(e => e.down), [1, 0]);
        assert.equal(b.send('mousedown'), false);
        assert.equal(b.send('mousemove'), false);
        assert.equal(b.send('mouseup'), false);
        b.lock(false);
        b.send('mousedown');
        b.send('mouseup');
        assert.deepEqual(b.events.map(e => e.down), [1, 0, 1, 0]);
    });

    // Bug: stopImmediatePropagation here blocked the main loop's wake and audio's resume, so input
    // on an idle screen waited for the next timer. Motion stays blocked, to not run a tick per move.
    test(`${mode}: buttons reach later window listeners, motion does not`, () => {
        const b = browser(threaded);
        const down = b.later('mousedown'), up = b.later('mouseup'), move = b.later('mousemove');
        assert.equal(b.send('mousemove'), true);
        assert.equal(b.send('mousedown'), true);
        assert.equal(b.send('mouseup'), true);
        assert.deepEqual([down.count, up.count, move.count], [1, 1, 0]);
    });
}

// The page's own key listeners exist only in the single-threaded builds.
const [ctrl, a] = [224, 4];

// Bug: Ctrl+Tab's key-up goes to another tab, the guest holds Ctrl and the game skips text alone.
test('losing focus or hiding releases each key held in the guest, once', () => {
    for (const lose of [b => b.send('blur'), b => b.hide()]) {
        const b = browser(false);
        b.key('keydown', 'ControlLeft');
        b.key('keydown', 'KeyA');
        b.key('keydown', 'KeyA');
        b.key('keyup', 'KeyA');
        b.key('keydown', 'ShiftLeft');
        lose(b);
        lose(b);
        assert.deepEqual(b.keys.slice(5), [[ctrl, 0], [225, 0]]);
    }
});

test('a key released before focus is lost is not released again', () => {
    const b = browser(false);
    b.key('keydown', 'KeyA');
    b.key('keyup', 'KeyA');
    b.send('blur');
    assert.deepEqual(b.keys, [[a, 1], [a, 0]]);
});

// Bug: a game acting on key-up would act twice when the player comes back and lets go of the key.
test('the real key-up after a focus-loss release sends nothing, and is still consumed', () => {
    const b = browser(false);
    b.key('keydown', 'KeyA');
    b.send('blur');
    assert.equal(b.key('keyup', 'KeyA'), true);
    assert.deepEqual(b.keys, [[a, 1], [a, 0]]);
});

// Bug: boxedwineKey returns null for these key-ups, so the key stayed down in the guest.
test('a held key is released when its key-up comes from a page input or with Ctrl down', () => {
    for (const event of [{target: {tagName: 'INPUT'}}, {ctrlKey: true}]) {
        const b = browser(false);
        b.key('keydown', 'KeyA');
        assert.equal(b.key('keyup', 'KeyA', event), true);
        assert.deepEqual(b.keys, [[a, 1], [a, 0]]);
    }
});

test('keys typed in a page input and browser shortcuts stay with the browser', () => {
    const b = browser(false);
    for (const event of [{target: {tagName: 'INPUT'}}, {ctrlKey: true}]) {
        assert.equal(b.key('keydown', 'KeyA', event), false);
        assert.equal(b.key('keyup', 'KeyA', event), false);
    }
    assert.deepEqual(b.keys, []);
});

// Bug: same as the buttons above; a keyboard-only player also got no audio.
// Bug: the page takes modifier keys before SDL, so the X state never held Shift, Ctrl, Alt, Caps or Num.
test('each key and mouse event sets the modifiers; losing focus keeps only the locks', () => {
    const SHIFT = 0x0001, CTRL = 0x0040, CAPS = 0x2000, NUM = 0x1000;
    const b = browser(false);
    b.key('keydown', 'KeyA', {shiftKey: true, locks: ['CapsLock', 'NumLock']});
    assert.equal(b.mods.at(-1), SHIFT | CAPS | NUM);
    b.key('mousedown', '', {button: 0, clientX: 300, clientY: 250, ctrlKey: true, locks: ['CapsLock']});
    assert.equal(b.mods.at(-1), CTRL | CAPS);
    const sent = b.mods.length;
    b.key('mouseup', '', {button: 0, clientX: 300, clientY: 250, ctrlKey: true, locks: ['CapsLock']});
    assert.equal(b.mods.length, sent, 'unchanged state is not sent again');
    b.send('blur');
    assert.equal(b.mods.at(-1), CAPS);
});

test('keys reach later window listeners', () => {
    const b = browser(false);
    const down = b.later('keydown'), up = b.later('keyup');
    b.key('keydown', 'KeyA');
    b.key('keyup', 'KeyA');
    assert.deepEqual([down.count, up.count], [1, 1]);
});
