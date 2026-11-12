// Execute the production EM_JS bodies that read GL state without a WebGL getParameter: the buffer bindings
// Emscripten keeps, the pixel-store values kept per context, and the index-buffer shadow's binding lookup.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const read = (file) => readFileSync(new URL('../../source/opengl/' + file, import.meta.url), 'utf8');
function body(source, ret, name) {
    const match = source.match(new RegExp(`EM_JS\\(${ret},\\s*${name},\\s*\\(([^)]*)\\),\\s*\\{([\\s\\S]*?)\\n\\s*\\}\\);`));
    assert.ok(match, 'production ' + name);
    const params = match[1].split(',').map((p) => p.trim().split(/\s+/).pop()).filter(Boolean);
    return {params, code: match[2]};
}
const marshal = read('glMarshal.cpp');
const bufferBound = body(marshal, 'int', 'boxedwine_buffer_bound_js');
const pixelStore = body(marshal, 'int', 'boxedwine_pixel_store_js');
const recordData = body(read('glcommon.cpp'), 'void', 'boxedwine_record_element_array_buffer_data_js');

const ARRAY = 0x8892, ELEMENT = 0x8893, PACK = 0x88eb, UNPACK = 0x88ec;
const UNPACK_ROW_LENGTH = 0x0cf2, UNPACK_ALIGNMENT = 0x0cf5, PACK_ALIGNMENT = 0x0d05;

function compile(globals, fn) {
    return vm.runInContext(`(function(${fn.params.join(',')}) {${fn.code}\n})`, vm.createContext(globals));
}

// A WebGL context that counts getParameter calls and validates pixelStorei like WebGL does.
function fakeContext(state = {}) {
    const ctx = {
        queries: 0,
        state: {[UNPACK_ALIGNMENT]: 4, [UNPACK_ROW_LENGTH]: 0, [PACK_ALIGNMENT]: 4, ...state},
        getParameter(pname) { ++this.queries; return this.state[pname]; },
        pixelStorei(pname, value) {
            if ((pname === UNPACK_ALIGNMENT || pname === PACK_ALIGNMENT) && ![1, 2, 4, 8].includes(value)) return; // INVALID_VALUE
            this.state[pname] = value;
        },
    };
    return ctx;
}

test('buffer bindings come from Emscripten\'s per-context bindings, not getParameter', () => {
    const GLctx = {
        getParameter() { throw Error('no getParameter'); },
        currentArrayBufferBinding: 3, currentElementArrayBufferBinding: 0,
        currentPixelPackBufferBinding: undefined, currentPixelUnpackBufferBinding: 7,
    };
    const bound = compile({GLctx}, bufferBound);
    assert.equal(bound(ARRAY), 1);
    assert.equal(bound(ELEMENT), 0);
    assert.equal(bound(PACK), 0);
    assert.equal(bound(UNPACK), 1);
    GLctx.currentElementArrayBufferBinding = 5;
    GLctx.currentPixelUnpackBufferBinding = 0;
    assert.equal(bound(ELEMENT), 1);
    assert.equal(bound(UNPACK), 0);
    assert.equal(compile({GLctx: null}, bufferBound)(UNPACK), 0);
});

test('a pixel-store value is asked of WebGL once and kept until it is set again', () => {
    const gl = fakeContext({[UNPACK_ROW_LENGTH]: 16});
    const globals = {GLctx: gl};
    const get = compile(globals, pixelStore);
    assert.equal(get(UNPACK_ROW_LENGTH), 16);
    assert.equal(get(UNPACK_ROW_LENGTH), 16);
    assert.equal(get(UNPACK_ALIGNMENT), 4);
    assert.equal(get(UNPACK_ALIGNMENT), 4);
    assert.equal(gl.queries, 2);

    // Emscripten's glPixelStorei calls GLctx.pixelStorei: the value reaches WebGL and is read again once.
    gl.pixelStorei(UNPACK_ROW_LENGTH, 32);
    assert.equal(gl.state[UNPACK_ROW_LENGTH], 32);
    assert.equal(get(UNPACK_ROW_LENGTH), 32);
    assert.equal(get(UNPACK_ROW_LENGTH), 32);
    assert.equal(gl.queries, 3);

    // A value WebGL rejects leaves the old one in place, and so does the kept value.
    gl.pixelStorei(UNPACK_ALIGNMENT, 3);
    assert.equal(get(UNPACK_ALIGNMENT), 4);
});

test('each WebGL context keeps its own pixel-store values', () => {
    const a = fakeContext({[PACK_ALIGNMENT]: 1}), b = fakeContext({[PACK_ALIGNMENT]: 8});
    const globals = {GLctx: a};
    const get = compile(globals, pixelStore);
    assert.equal(get(PACK_ALIGNMENT), 1);
    globals.GLctx = b; // a context switch
    assert.equal(get(PACK_ALIGNMENT), 8);
    b.pixelStorei(PACK_ALIGNMENT, 2);
    assert.equal(get(PACK_ALIGNMENT), 2);
    globals.GLctx = a;
    assert.equal(get(PACK_ALIGNMENT), 1);
    assert.equal(a.queries, 1);
    assert.equal(compile({GLctx: undefined}, pixelStore)(PACK_ALIGNMENT), -1); // no value: the destination is left alone
});

test('the index-buffer shadow finds the bound buffer without getParameter and reuses its bytes', () => {
    const buffer = {name: 5};
    const heap = new Uint8Array(64);
    const module = {};
    const GLctx = {currentElementArrayBufferBinding: 5, getParameter() { throw Error('no getParameter'); }};
    const record = compile({GLctx, GL: {buffers: [null, null, null, null, null, buffer]}, HEAPU8: heap, Module: module}, recordData);
    heap.set([1, 2, 3, 4], 8);
    record(8, 4);
    const bytes = module.boxedwineElementArrayBufferBytes.get(buffer);
    assert.deepEqual(Array.from(bytes), [1, 2, 3, 4]);
    heap.set([9, 8, 7, 6], 8);
    record(8, 4);
    assert.equal(module.boxedwineElementArrayBufferBytes.get(buffer), bytes);
    assert.deepEqual(Array.from(bytes), [9, 8, 7, 6]);
    record(0, 4); // glBufferData without data: zeroed, as a new store would be
    assert.deepEqual(Array.from(module.boxedwineElementArrayBufferBytes.get(buffer)), [0, 0, 0, 0]);
    record(8, 2);
    assert.deepEqual(Array.from(module.boxedwineElementArrayBufferBytes.get(buffer)), [9, 8]);
    GLctx.currentElementArrayBufferBinding = 0;
    heap.set([5, 5], 8);
    record(8, 2);
    assert.deepEqual(Array.from(module.boxedwineElementArrayBufferBytes.get(buffer)), [9, 8]);
});
