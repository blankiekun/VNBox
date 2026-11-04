// The site under test and a browser driving it through its UI: a static server for a site dir (the
// game's files beside it, for the copy into OPFS), a persistent browser profile, the VNBox window.
import { createRequire } from 'node:module';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.wasm': 'application/wasm', '.zip': 'application/zip', '.svg': 'image/svg+xml', '.png': 'image/png', '.woff2': 'font/woff2' };
const GAME_PREFIX = '/__e2e/game/', LISTING = '/__e2e/listing.json';
export const sleep = ms => new Promise(r => setTimeout(r, ms));

function listFiles(dir, base = '') {
  return fs.readdirSync(path.join(dir, base), { withFileTypes: true }).flatMap(e => {
    const rel = base ? base + '/' + e.name : e.name;
    return e.isDirectory() ? listFiles(dir, rel) : [{ path: rel, size: fs.statSync(path.join(dir, rel)).size }];
  });
}

// no-store: a profile is kept between builds on one origin, and a cached engine would test the last build.
// notFound collects every 404, so a check sees requests no page event shows (a worker's, say).
export function serve(siteDir, gameDir, port) {
  const notFound = [];
  const server = http.createServer((req, res) => {
    const url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (url === LISTING) { res.writeHead(200, { 'Content-Type': MIME['.json'], 'Cache-Control': 'no-store' }); return res.end(JSON.stringify(listFiles(gameDir))); }
    const [root, rel] = url.startsWith(GAME_PREFIX) ? [gameDir, url.slice(GAME_PREFIX.length)] : [siteDir, url === '/' ? 'index.html' : url.slice(1)];
    const file = path.resolve(root, rel);
    if (!file.startsWith(path.resolve(root) + path.sep) || !fs.statSync(file, { throwIfNoEntry: false })?.isFile()) { notFound.push(url); res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Content-Length': fs.statSync(file).size, 'Cache-Control': 'no-store' });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve, reject) => {
    server.on('error', reject);
    server.listen(port, '127.0.0.1', () => resolve({ origin: 'http://127.0.0.1:' + port, notFound, close: () => { server.closeAllConnections(); server.close(); } }));
  });
}

// Runs before the page's scripts in every frame: the only way to see an IndexedDB open that a later look at databases() would miss.
const WATCH_IDB = () => {
  const open = IDBFactory.prototype.open;
  IDBFactory.prototype.open = function (name, ...rest) { console.log('[e2e] idb open ' + name); return open.call(this, name, ...rest); };
};

export async function launch(browserName, profile, sink, playwrightDir) {
  const pw = createRequire(path.resolve(playwrightDir, 'package.json'))('playwright');
  const opts = { headless: false, viewport: { width: 1400, height: 1000 }, env: { ...process.env, PULSE_SINK: sink } };
  if (browserName === 'chromium') opts.args = ['--ozone-platform=x11', '--autoplay-policy=no-user-gesture-required', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'];
  else opts.firefoxUserPrefs = { 'media.autoplay.default': 0, 'media.autoplay.block-webaudio': false, 'javascript.options.wasm_js_promise_integration': true,
    'webgl.force-enabled': true, 'webgl.disable-fail-if-major-performance-caveat': true };
  const ctx = await pw[browserName].launchPersistentContext(profile, opts);
  await ctx.addInitScript(WATCH_IDB);
  return ctx;
}

// One browser tab with the site open: what it logged, and the steps a player takes.
export class Tab {
  constructor(page, origin, game, onLog) {
    Object.assign(this, { page, origin, game, onLog });
    this.console = [];
    this.pageErrors = [];
    this.crashed = false;
    page.on('console', m => { const t = m.text(); this.console.push(t); if (onLog) onLog(t); });
    page.on('pageerror', e => { this.pageErrors.push(String(e)); if (onLog) onLog('PAGEERROR ' + e); });
    page.on('crash', () => { this.crashed = true; if (onLog) onLog('CRASH'); });
    // Answers a dialog the site didn't get as a msgbox (it shouldn't ask, but a stuck run helps nobody).
    page.on('dialog', d => d.dismiss().catch(() => {}));
  }

  // Copies the game into OPFS (files already there at their size stay) and lists it as a recent game, as
  // opening its folder would: the folder picker is the one step of the site's that can't run unattended.
  async prime() {
    await this.page.goto(this.origin + '/privacy.html');
    return this.page.evaluate(async ({ game, listing, prefix }) => {
      const vn = await (await navigator.storage.getDirectory()).getDirectoryHandle('vn', { create: true });
      const dirHandle = await vn.getDirectoryHandle(game.title, { create: true });
      let copied = 0, size = 0;
      for (const f of await (await fetch(listing)).json()) {
        const parts = f.path.split('/');
        let dir = dirHandle;
        for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p, { create: true });
        const fh = await dir.getFileHandle(parts.at(-1), { create: true });
        size += f.size;
        if ((await fh.getFile()).size === f.size) continue;
        const res = await fetch(prefix + f.path.split('/').map(encodeURIComponent).join('/'));
        await res.body.pipeTo(await fh.createWritable());
        copied += f.size;
      }
      localStorage.setItem('w95.vnbox.recent', JSON.stringify([{ title: game.title, at: Date.now(), size, exe: game.exe, hook: game.hook, hooked: true }]));
      localStorage.setItem('w95.vnbox.log', 'true');
      return copied;
    }, { game: this.game, listing: LISTING, prefix: GAME_PREFIX });
  }

  // The site's two save databases for the game (js/apps/vnsaves.js saveAreas). In the store a file is { contents, mode, timestamp }.
  saveDbs() {
    const lazy = encodeURIComponent('opfs:vn/' + this.game.title);
    return ['/lazyapp/' + lazy, '/root/lazy/' + lazy];
  }

  async wipeStore() {
    await this.page.goto(this.origin + '/privacy.html');
    await this.page.evaluate(names => Promise.all(names.map(n => new Promise((resolve, reject) => {
      const r = indexedDB.deleteDatabase(n);
      r.onsuccess = resolve; r.onerror = () => reject(r.error);
    }))), this.saveDbs());
  }

  // Paths and sizes of the files the game wrote into its own folder, read from the store. Only with the game stopped.
  storedFiles() {
    return this.page.evaluate(name => new Promise((resolve, reject) => {
      const r = indexedDB.open(name);
      r.onerror = () => reject(r.error);
      r.onsuccess = () => {
        const db = r.result, out = {};
        if (!db.objectStoreNames.contains('FILE_DATA')) { db.close(); return resolve(out); }
        const cur = db.transaction('FILE_DATA').objectStore('FILE_DATA').openCursor();
        cur.onerror = () => { db.close(); reject(cur.error); };
        cur.onsuccess = () => {
          const c = cur.result;
          if (!c) { db.close(); return resolve(out); }
          if (c.value.contents) out[c.key] = c.value.contents.length;
          c.continue();
        };
      };
    }), this.saveDbs()[0]);
  }

  // Deletes those files: what a store losing a save looks like to the next start.
  deleteStored(match) {
    return this.page.evaluate(({ name, match }) => new Promise((resolve, reject) => {
      const r = indexedDB.open(name);
      r.onerror = () => reject(r.error);
      r.onsuccess = () => {
        const db = r.result, t = db.transaction('FILE_DATA', 'readwrite'), store = t.objectStore('FILE_DATA');
        let n = 0;
        store.openCursor().onsuccess = e => { const c = e.target.result; if (c) { if (new RegExp(match).test(c.key)) { c.delete(); n++; } c.continue(); } };
        t.oncomplete = () => { db.close(); resolve(n); };
        t.onerror = () => { db.close(); reject(t.error); };
      };
    }), { name: this.saveDbs()[0], match });
  }

  // File menu to Play, the way a visitor starts a game they've opened before.
  async openSite() {
    await this.page.goto(this.origin + '/?app=vnbox');
    await this.page.waitForSelector('.vnb-card', { timeout: 60000 });
  }
  async clickPlay() {
    await this.page.click('.vnb-card');
    await this.page.getByRole('button', { name: /^Play$/ }).first().click();
    await this.page.waitForSelector('.vnb-log', { state: 'attached', timeout: 60000 });
  }
  async play() { await this.openSite(); await this.clickPlay(); }

  // File > Close Game, answering the site's question: the site's own way to stop a game.
  async closeGame() {
    await this.page.getByRole('menubar').getByRole('menuitem', { name: 'File' }).click();
    await this.page.getByRole('menu').getByRole('menuitem', { name: /Close Game/ }).click();
    await this.page.getByRole('button', { name: 'Yes' }).click();
    await this.page.waitForSelector('.vnb-card', { timeout: 30000 });
  }

  frame() { return this.page.frames().find(f => f.url().includes('boxedwine.html')); }

  // From now on the frame removes itself in the same tick a write of the file ends: IDBFS asks for a persist then, and runs it on a later timer.
  removeFrameOnWrite(file) {
    return this.frame().evaluate(file => {
      const written = node => FS.isDir(node.mode) ? Object.values(node.contents).some(written) : FS.getPath(node).endsWith('/' + file) && node.usedBytes > 0;
      const queue = IDBFS.queuePersist;
      IDBFS.queuePersist = mount => {
        queue(mount);
        if (written(mount.root)) window.frameElement.remove();
      };
    }, file);
  }

  // The game's canvas on the page, with its size in game pixels. Null until it has a real size.
  async canvas() {
    const f = this.frame();
    const c = f && await f.$('canvas#canvas').catch(() => null);
    const box = c && await c.boundingBox().catch(() => null);
    if (!box || box.width < 100) return null;
    const [w, h] = await c.evaluate(e => [e.width, e.height]);
    return { box, w, h };
  }

  // A click at game pixels. False if there's no game screen yet.
  async click(x, y, button = 'left') {
    const c = await this.canvas();
    if (!c) return false;
    await this.page.mouse.click(c.box.x + x * c.box.width / c.w, c.box.y + y * c.box.height / c.h, { button, delay: 80 });
    return true;
  }

  async hover(x, y) {
    const c = await this.canvas();
    if (c) await this.page.mouse.move(c.box.x + x * c.box.width / c.w, c.box.y + y * c.box.height / c.h);
  }

  // The canvas's area of the page as a grid of mean colours (cols x rows x [r,g,b]): enough to tell one of the game's screens from
  // another without keeping a copy of its art. Decoded by the page itself, so no image library. The movie's canvas isn't told apart from the game's here.
  async grid(cols = 8, rows = 6) {
    const c = await this.canvas();
    if (!c) return null;
    const png = (await this.page.screenshot({ clip: c.box })).toString('base64');
    return this.page.evaluate(async ({ png, cols, rows }) => {
      const bytes = Uint8Array.from(atob(png), ch => ch.charCodeAt(0));
      const bmp = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
      const g = new OffscreenCanvas(bmp.width, bmp.height).getContext('2d');
      g.drawImage(bmp, 0, 0);
      const { data } = g.getImageData(0, 0, bmp.width, bmp.height);
      // Averaged here: what drawImage does when scaling down differs between browsers.
      const sum = new Array(cols * rows * 3).fill(0), count = new Array(cols * rows).fill(0);
      for (let y = 0; y < bmp.height; y++) for (let x = 0; x < bmp.width; x++) {
        const cell = Math.floor(y * rows / bmp.height) * cols + Math.floor(x * cols / bmp.width), p = (y * bmp.width + x) * 4;
        for (let c = 0; c < 3; c++) sum[cell * 3 + c] += data[p + c];
        count[cell]++;
      }
      return sum.map((v, i) => Math.round(v / count[Math.floor(i / 3)]));
    }, { png, cols, rows });
  }

  lines() { return this.page.evaluate(() => [...document.querySelectorAll('.vnb-log p:not(.vnb-log-empty)')].map(p => p.textContent)); }

  // State of the page's movies (vnbox-movie.js), read straight from its objects: what a screenshot can't be trusted for.
  movies() {
    const f = this.frame();
    return f ? f.evaluate(() => typeof vnboxMovies === 'undefined' ? [] : [...vnboxMovies.values()].map(m => ({ frames: m.D && m.m ? m.D._vm_frames(m.m) : 0, shown: m.canvas.style.display, running: !!m.running, ended: !!m.ended })))
      .catch(() => []) : Promise.resolve([]);
  }
}
