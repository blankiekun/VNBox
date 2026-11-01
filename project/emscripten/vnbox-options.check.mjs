// Self-check of vnbox-options.json, the list of VNBox URL options wrappers build tuning menus from.
// Catches: an option read in code that the list leaves out (a wrapper can't offer it), a listed option nothing reads
// any more (a menu entry that does nothing), an option read around the one reading path (vnboxUrlInt/vnboxUrlIs in
// C++, vnboxParam in JS), and entries a wrapper can't build UI from (bad type, default out of range, missing field).
// Run: node vnbox-options.check.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const here = path.dirname(new URL(import.meta.url).pathname);
const root = path.resolve(here, '../..');
const registry = JSON.parse(fs.readFileSync(path.join(here, 'vnbox-options.json'), 'utf8'));

// URL params read through vnboxParam that are inputs from the embedding page, not tuning options.
const NOT_OPTIONS = new Set(['lazy']); // the game folder (boxedwine-shell.js)

const errors = [];
const fail = (msg) => errors.push(msg);

// Registry shape.
assert.equal(registry.schema, 1, 'schema version');
const groups = new Set(registry.groups);
const listed = new Map();
for (const o of registry.options) {
    const where = 'vnbox-options.json ' + (o.name || JSON.stringify(o));
    for (const key of ['name', 'label', 'description', 'type', 'group', 'restart', 'experimental']) {
        if (o[key] === undefined || o[key] === '') fail(where + ': no ' + key);
    }
    if (!('default' in o)) fail(where + ': no default');
    if (listed.has(o.name)) fail(where + ': listed twice');
    listed.set(o.name, o);
    if (!groups.has(o.group)) fail(where + ': unknown group ' + o.group);
    if (/\n/.test(o.description || '')) fail(where + ': description is more than one line');
    if (o.type === 'bool') {
        if (typeof o.default !== 'boolean') fail(where + ': bool default is not true/false');
        const u = o.urlValues || {};
        if (typeof u.true !== 'string' || typeof u.false !== 'string' || u.true === u.false) fail(where + ': urlValues needs distinct true and false strings');
    } else if (o.type === 'int' || o.type === 'float') {
        const isNum = o.type === 'int' ? Number.isInteger : Number.isFinite;
        if (![o.default, o.min, o.max].every(isNum)) fail(where + ': default/min/max not ' + o.type);
        else if (!(o.min <= o.default && o.default <= o.max)) fail(where + ': default outside min..max');
    } else if (o.type === 'choice') {
        const values = (o.choices || []).map((c) => c.value);
        if (values.length < 2 || !o.choices.every((c) => c.label)) fail(where + ': choices need a value and a label each');
        if (!values.includes(o.default)) fail(where + ': default is not one of the choices');
    } else {
        fail(where + ': unknown type ' + o.type);
    }
}

// Reads in code.
function walk(dir, exts, out = []) {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) walk(p, exts, out);
        else if (exts.some((x) => e.name.endsWith(x))) out.push(p);
    }
    return out;
}
const cppFiles = ['source', 'include', 'platform'].flatMap((d) => walk(path.join(root, d), ['.cpp', '.h', '.c']));
const jsFiles = fs.readdirSync(here)
    .filter((f) => f.endsWith('.js') && f !== 'binaryen_js.js')
    .map((f) => path.join(here, f));

const read = new Map(); // option -> first place read
const note = (name, file, src, index) => {
    if (!read.has(name)) read.set(name, path.relative(root, file) + ':' + (src.slice(0, index).split('\n').length));
};
const lit = /^\s*(["'])([^"']+)\1/;
for (const file of cppFiles) {
    const src = fs.readFileSync(file, 'utf8');
    for (const m of src.matchAll(/\bvnboxUrl(?:Int|Is)\(([^)]*)/g)) {
        const name = lit.exec(m[1]);
        if (name) note(name[2], file, src, m.index);
        else if (!/^\s*const char\* name/.test(m[1])) fail(path.relative(root, file) + ': vnboxUrlInt/vnboxUrlIs without a literal name');
    }
    for (const m of src.matchAll(/\bvnboxParam\(\s*["']/g)) {
        fail(path.relative(root, file) + ':' + src.slice(0, m.index).split('\n').length + ': reads vnboxParam directly; use vnboxUrlInt/vnboxUrlIs (vnboxparam.h)');
    }
}
for (const file of jsFiles) {
    const src = fs.readFileSync(file, 'utf8');
    for (const m of src.matchAll(/\bvnboxParam\(([^)]*)/g)) {
        const name = lit.exec(m[1]);
        if (name) note(name[2], file, src, m.index);
        else if (!/^\s*name\s*$/.test(m[1])) fail(path.relative(root, file) + ': vnboxParam without a literal name');
    }
    for (const m of src.matchAll(/\bgetParameter\(\s*["']([^"']+)/g)) {
        if (listed.has(m[1])) fail(path.relative(root, file) + ': reads ' + m[1] + ' with getParameter; use vnboxParam');
    }
}

for (const [name, at] of read) {
    if (!listed.has(name) && !NOT_OPTIONS.has(name)) fail(at + ': reads ' + name + ', which vnbox-options.json does not list');
}
for (const name of listed.keys()) {
    if (!read.has(name)) fail('vnbox-options.json lists ' + name + ', which nothing reads');
    if (NOT_OPTIONS.has(name)) fail('vnbox-options.json lists ' + name + ', which is not an option (NOT_OPTIONS)');
}

if (errors.length) {
    console.error(errors.join('\n'));
    process.exit(1);
}
console.log('vnbox-options self-check ok: ' + listed.size + ' options');
