// node suite.mjs <firefox|chromium> <serve dir> <game dir> <work dir> <log dir> <only> <break>: one browser's checks. Started by run.sh.
// Each check starts the browser anew on the kept profile, so one check's crash or stuck game can't fail the next.
import fs from 'node:fs';
import path from 'node:path';
import { analyze, capture, GAP_MS, loadSink } from './audio.mjs';
import { launch, serve, sleep, Tab } from './site.mjs';

const [browserName, serveDir, gameDir, workDir, logDir, only, breakName] = process.argv.slice(2);
if (process.env.E2E_OFFSCREEN !== '1') { console.error('Start this through run.sh: it runs the browser off-screen.'); process.exit(2); }

// The game these checks know, in its own 800x600 pixels.
const GAME = { title: '信天翁航海録', exe: '信天翁航海録.exe', hook: 'PC_Albatross_Koukairoku.js' };
const NOWHERE = [700, 30];        // clicks here skip the movie and do nothing on the title
const NEW_GAME = [190, 424], CONTINUE = [400, 424], TEXT = [400, 300];
const MENU_SAVE = [315, 521], SLOT_1 = [215, 126], SAVE_FILE = 'save/rssave01.dat';
const FIRST_LINE = '『巻頭歌』';
const AUDIO_SECONDS = 30;
// The title screen as Tab.grid() sees it (mean colours of an 8x6 grid, no art), and how far off a screen can be per channel on average (Firefox and Chromium differ by under 1, the screens below from each other by over 24).
const TITLE_GRID = [154,222,237,160,221,226,160,219,218,168,219,215,164,218,215,163,220,216,168,218,213,155,222,235,155,220,233,248,231,182,243,220,169,255,228,167,239,209,138,242,217,159,238,213,158,159,217,217,142,202,213,240,230,206,219,206,177,208,197,171,237,224,194,235,222,192,235,221,190,146,201,207,149,213,227,122,181,194,127,179,190,128,180,191,135,190,202,128,181,193,127,182,194,147,212,226,154,222,237,114,157,167,118,154,161,117,151,159,108,145,153,119,158,167,121,161,170,155,222,237,148,222,238,152,222,237,117,170,181,116,169,181,120,172,183,122,176,188,152,222,237,150,222,238];
const SCREEN_TOLERANCE = 15;
const MENU_GRID = [27,29,34,32,33,38,2,5,10,2,5,10,7,9,14,7,9,14,3,5,10,2,4,9,46,48,52,63,65,68,24,25,29,14,15,19,19,21,24,46,47,50,20,22,25,8,10,13,31,33,37,62,63,66,29,31,34,36,38,41,34,36,39,40,42,45,49,50,53,33,34,38,10,12,16,37,39,42,13,15,18,9,11,15,27,29,32,11,13,16,24,25,29,7,9,12,6,8,10,54,55,58,13,14,17,9,11,14,26,28,30,5,7,10,24,26,28,6,8,11,3,4,7,6,8,10,18,19,21,16,18,19,19,20,22,16,17,19,4,6,9,1,3,6];
const SAVE_GRID = [229,184,191,231,181,189,244,190,198,243,191,199,244,190,198,242,186,195,244,190,198,243,190,199,213,138,149,162,100,109,170,108,117,194,128,138,201,124,135,161,101,110,173,109,119,205,133,144,213,137,148,163,106,114,167,107,116,195,132,141,202,130,141,158,99,108,173,115,124,207,134,145,207,134,145,156,97,106,156,103,111,194,129,139,195,124,135,148,95,103,168,108,117,203,132,143,204,133,144,137,85,93,142,94,102,174,114,123,187,117,127,135,87,94,147,94,102,188,125,135,236,181,190,199,161,167,168,134,139,175,141,146,180,143,149,170,135,140,185,150,156,182,145,151];
const LOAD_GRID = [184,189,230,180,185,229,189,194,243,191,196,243,190,195,244,186,192,242,190,196,244,190,196,243,137,144,204,97,101,135,112,117,167,134,141,199,124,131,201,101,107,161,109,116,173,133,141,205,137,144,213,106,111,163,107,113,167,132,138,195,130,138,202,99,104,158,115,121,173,134,141,207,134,141,207,97,103,156,103,108,156,129,135,194,124,131,195,95,100,148,108,114,168,132,139,203,133,140,204,85,91,137,94,99,142,114,120,174,117,123,187,87,92,135,94,99,147,125,131,188,181,187,236,161,165,199,134,138,168,141,144,175,143,147,180,135,138,170,150,154,185,145,149,182];

// Checks here are known to give different answers on the same build: they still print, but don't count.
const FLAKY = new Set([]);
// One browser per origin and profile for good: a new port would be a new origin and a new multi-GB copy of the game (and a QuotaExceededError).
const PORT = { firefox: 8460, chromium: 8461 }[browserName];

const CHECK_MS = 900000;
class Failure extends Error {}
const fail = msg => { throw new Failure(msg); };

const near = (a, b, tolerance) => a && a.length === b.length && a.reduce((sum, v, i) => sum + Math.abs(v - b[i]), 0) / a.length <= tolerance;

// A browser call has no time limit of its own: a stuck one would otherwise print nothing until run.sh's timeout.
const within = (what, ms, promise) => {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Failure(what + ' got stuck')), ms); })]).finally(() => clearTimeout(timer));
};

async function waitFor(what, timeoutMs, test, every = 500) {
  const end = Date.now() + timeoutMs;
  for (;;) {
    const got = await test();
    if (got) return got;
    if (Date.now() > end) fail(what);
    await sleep(every);
  }
}

const onScreen = async (tab, grid) => near(await tab.grid(), grid, SCREEN_TOLERANCE);
const atTitle = tab => onScreen(tab, TITLE_GRID);
const waitScreen = (tab, grid, what) => waitFor(what + ' never showed', 30000, () => onScreen(tab, grid));

// Clicks through whatever plays until the title shows.
const toTitle = tab => waitFor('the title never showed', 180000, async () => {
  if (await atTitle(tab)) return true;
  await tab.click(...NOWHERE);
  return false;
}, 1500);

// Clicks until the text log has more lines than `count`: a click can land while the game is still busy.
const nextLine = (tab, count, what = 'no new line reached the text log') => waitFor(what, 60000, async () => {
  await tab.click(...TEXT);
  await sleep(2500);
  const l = await tab.lines();
  return l.length > count ? l : null;
}, 500);

async function startText(tab) {
  await toTitle(tab);
  const l = await waitFor('no line reached the text log after New Game', 90000, async () => {
    await tab.click(...NEW_GAME);
    await sleep(2500);
    const l = await tab.lines();
    return l.length ? l : null;
  }, 500);
  return l;
}

const CHECKS = {
  // A plain start: the game gets to its title with nothing wrong on the way.
  // Catches: an engine or zip that stops a game starting, a page error, a host hook off for a root fs of another build, the JIT cache asked for by a plain start.
  'normal-start': async ({ tab, server }) => {
    await tab.play();
    await toTitle(tab);
    const problems = [
      ...tab.pageErrors.map(e => 'page error: ' + e),
      ...tab.console.filter(t => t.startsWith('[hooks]')),
      ...tab.console.filter(t => t.startsWith('[e2e] idb open boxedwine-wasm-jit')),
      ...server.notFound.filter(u => u.endsWith('.zip')).map(u => '404 ' + u),
    ];
    if (problems.length) fail(problems.slice(0, 3).join(' | ').slice(0, 300));
    return `title reached, ${tab.console.length} console lines clean`;
  },

  // The opening movie plays, by the page's own movie object, a click stops it, and the title follows. A screenshot can't tell here: headless shows a playing movie black.
  // Catches: a movie that never opens or never decodes, one a click can't skip, a game that hangs after it.
  movie: async ({ tab }) => {
    await tab.play();
    const playing = await waitFor('no movie started', 120000, async () => (await tab.movies()).find(m => m.running && m.shown === 'block'));
    const first = playing.frames;
    await waitFor(`the movie stuck at frame ${first}`, 30000, async () => (await tab.movies()).some(m => m.frames > first + 10));
    await tab.click(...NOWHERE);
    await waitFor('a click did not stop the movie', 30000, async () => {
      if (!(await tab.movies()).some(m => m.running)) return true;
      await tab.click(...NOWHERE);
      return false;
    }, 1500);
    await toTitle(tab);
    return 'played, skipped, title reached';
  },

  // Lines the game shows reach the site's text log, once each.
  // Catches: a hook that doesn't load or attach, a hook firing twice per line (seen before on this game).
  'text-hook': async ({ tab }) => {
    await tab.play();
    let l = await startText(tab);
    for (let i = 0; i < 5; i++) l = await nextLine(tab, l.length);
    await sleep(3000);
    l = await tab.lines();
    if (l[0] !== FIRST_LINE) fail(`the first line is ${JSON.stringify(l[0])}`);
    const twice = l.findIndex((t, i) => i && t === l[i - 1]);
    if (twice > 0) fail(`logged twice in a row: ${JSON.stringify(l[twice])}`);
    return `${l.length} lines, no repeat`;
  },

  // A save made in the game survives the site's Close Game right after it, and a restart: the slot holds it and loading it comes back to the same line.
  // Catches: saves still only in memory when the frame goes (the store written too late), a store that isn't read back at the next start.
  saves: async ({ tab, breakName }) => {
    await tab.play();
    let l = await startText(tab);
    l = await nextLine(tab, l.length);
    const place = l.at(-1);
    // Right after a line the game may still be drawing it, and the first right click only finishes that.
    await waitFor('the game\'s menu never opened', 60000, async () => {
      if (await onScreen(tab, MENU_GRID)) return true;
      await tab.click(...TEXT, 'right');
      await sleep(3000);
      return false;
    });
    await tab.click(...MENU_SAVE);
    await waitScreen(tab, SAVE_GRID, 'the save screen');
    await tab.hover(...SLOT_1);
    await sleep(1000);
    const empty = await tab.grid(16, 12);
    await tab.click(...SLOT_1);
    // The moment the game shows the slot filled, a player thinks it's saved. The slot is a tenth of the screen, so by its cells, not the mean.
    await waitFor('the save slot never filled', 30000, async () => (await tab.grid(16, 12)).some((v, i) => Math.abs(v - empty[i]) > 20), 50);
    if (breakName === 'close-at-once') await tab.removeFrame(); else await tab.closeGame();
    const stored = await tab.storedFiles();
    const file = Object.keys(stored).find(k => k.endsWith(SAVE_FILE));
    if (!file) fail(`${SAVE_FILE} is not in the store after closing: ${JSON.stringify(Object.keys(stored))}`);
    if (breakName === 'wipe-save') await tab.deleteStored(SAVE_FILE);
    if (breakName === 'close-at-once') await tab.openSite(); // the site wasn't told, so it has no card to play from
    await tab.clickPlay();
    await toTitle(tab);
    // A click while the title is still coming up does nothing, as with New Game.
    await waitFor('the load screen never showed', 60000, async () => {
      if (await onScreen(tab, LOAD_GRID)) return true;
      await tab.click(...CONTINUE);
      await sleep(2500);
      return false;
    });
    await tab.click(...SLOT_1);
    const back = await waitFor(`loading slot 1 brought no line back (saved at ${JSON.stringify(place)})`, 60000, async () => {
      const l = await tab.lines();
      return l.length ? l : null;
    });
    if (back[0] !== place) fail(`saved at ${JSON.stringify(place)}, loaded at ${JSON.stringify(back[0])}`);
    return `${stored[file]} byte save kept, load came back to the same line`;
  },

  // The game's music reaches the sound output. Numbers only beyond that: how a run's gaps vary is too wide for a limit (see the PR).
  // Catches: no sound at all. Gaps and level print for a person to compare.
  audio: async ({ tab, sink, dir }) => {
    await tab.play();
    await toTitle(tab);
    const rec = capture(sink.name, path.join(dir, 'capture.raw'));
    await sleep(AUDIO_SECONDS * 1000);
    const a = analyze(await rec.stop());
    fs.rmSync(path.join(dir, 'capture.raw'));
    const numbers = `${a.soundSeconds} of ${a.seconds} s with sound, rms ${a.rmsDb} dB, peak ${a.peakDb} dB, ${a.gaps} gaps of ${GAP_MS}+ ms, ${a.gapMs} ms in all, longest ${a.longestGapMs} ms`;
    if (a.soundSeconds < AUDIO_SECONDS / 2) fail('hardly any sound: ' + numbers);
    return { status: 'INFO', detail: numbers };
  },

  // A second tab on the same game is refused and says why, and the first plays on.
  // Catches: two tabs writing one game's saves over each other, a refusal that also stops the first tab.
  'two-tabs': async ({ tab, ctx }) => {
    await tab.play();
    const l = await startText(tab);
    const second = new Tab(await ctx.newPage(), tab.origin, GAME);
    await second.play();
    await second.page.getByText('already open in another tab').waitFor({ timeout: 90000 }).catch(() => fail('the second tab never said the game is already open'));
    await second.page.close();
    await tab.page.bringToFront();
    if (!tab.frame()) fail('the first tab lost its game');
    await nextLine(tab, l.length, 'the first tab stopped playing: no new line after the second tab was refused');
    return 'second tab refused, first plays on';
  },
};

// Bad inputs, to see a check fail (--break NAME). Each changes the serve dir's copy; wipe-save is read by the saves check.
const engine = (dir, name) => path.join(dir, 'vnbox/engine', name);
// Both safe to run again: the browsers of one run share the serve dir.
const remove = file => fs.rmSync(file, { force: true });
const edit = (file, from, to) => {
  const s = fs.readFileSync(file, 'utf8');
  if (s.includes(to)) return;
  if (!s.includes(from)) throw new Error(`${from} is not in ${file}`);
  fs.writeFileSync(file, s.replace(from, to));
};
const BREAKS = {
  // normal-start: a zip with its directory damaged. Damage in the middle isn't enough: a start reads few of its files, and 64 KB there passed.
  'corrupt-jp-zip': dir => { const z = engine(dir, 'jp.zip'), b = fs.readFileSync(z); b.fill(0x55, b.length - 65536); fs.writeFileSync(z, b); },
  // movie: the page's movie player gone.
  'no-movie-player': dir => remove(engine(dir, 'vnbox-movie.js')),
  // movie: a movie no click can stop, on the page's side: its stop and close do nothing.
  'unstoppable-movie': dir => edit(engine(dir, 'vnbox-movie.js'), 'var movie = vnboxMovies.get(id);', 'var movie = vnboxMovies.get(id); if (cmd === "stop" || cmd === "close" || cmd === "pause") return 0;'),
  // text-hook: the hook script is gone.
  'no-hook': dir => remove(path.join(dir, 'vnbox/hooks', GAME.hook)),
  // text-hook: a hook that sends every line twice.
  'double-hook': dir => edit(engine(dir, 'frida-compat.js'), "if (typeof globalThis.boxedwineText === 'function') globalThis.boxedwineText(text, t);", "if (typeof globalThis.boxedwineText === 'function') { globalThis.boxedwineText(text, t); globalThis.boxedwineText(text, t); }"),
  // saves: the save deleted from the store between close and restart.
  'wipe-save': () => {},
  // saves: the frame removed the moment the slot shows filled, with none of the site's close path before it.
  'close-at-once': () => {},
  // audio: the site starts the game with no sound.
  'no-sound': dir => edit(path.join(dir, 'js/apps/vnbox.js'), "'audioFreq=44100'", "'audioFreq=44100', 'sound=false'"),
  // two-tabs: no lock between tabs.
  'no-tab-lock': dir => edit(engine(dir, 'lazyfs.js'), 'if (typeof navigator === "undefined" || !navigator.locks) {', 'if (true) {'),
};

const results = [], started = Date.now();
function report(status, name, detail) {
  const counted = !FLAKY.has(name) || status === 'INFO';
  results.push({ status, counted });
  console.log(`${status.padEnd(4)} ${browserName} ${name}${counted ? '' : ' (flaky, not counted)'}: ${detail}`);
}

fs.mkdirSync(logDir, { recursive: true });
const sink = loadSink('vnbox-e2e-' + browserName);
const server = await serve(path.resolve(serveDir), path.resolve(gameDir), PORT);
const profile = path.join(workDir, 'profile-' + browserName);
try {
  if (BREAKS[breakName]) await BREAKS[breakName](serveDir);
  else if (breakName) { console.log('unknown --break ' + breakName + ': ' + Object.keys(BREAKS).join(', ')); process.exit(2); }

  // The game's copy, and a store with no saves left from an earlier run.
  try {
    const ctx = await within('starting the browser', 300000, launch(browserName, profile, sink.name, process.env.E2E_PLAYWRIGHT));
    const tab = new Tab(ctx.pages()[0] || await ctx.newPage(), server.origin, GAME);
    const copied = await within('copying the game', 1800000, tab.prime());
    await within('clearing the save store', 120000, tab.wipeStore());
    if (copied) console.log(`     ${browserName} copied the game into OPFS, ${(copied / 2 ** 30).toFixed(1)} GB`);
    await within('closing the browser', 120000, ctx.close());
  } catch (e) {
    console.log(`FAIL ${browserName} overall: the run couldn't start: ${String(e).split('\n')[0].slice(0, 200)}`);
    server.close(); sink.unload();
    process.exit(2);
  }

  for (const [name, check] of Object.entries(CHECKS)) {
    if (only && !only.split(',').includes(name)) continue;
    server.notFound.length = 0;
    const dir = path.join(logDir, name);
    fs.mkdirSync(dir, { recursive: true });
    const log = fs.createWriteStream(path.join(dir, 'console.log'));
    const ctx = await launch(browserName, profile, sink.name, process.env.E2E_PLAYWRIGHT);
    const tab = new Tab(ctx.pages()[0] || await ctx.newPage(), server.origin, GAME, t => log.write(t + '\n'));
    try {
      const out = await within('the check', CHECK_MS, check({ tab, ctx, server, sink, dir, breakName }));
      if (out && out.status) report(out.status, name, out.detail); else report('PASS', name, out);
    } catch (e) {
      report('FAIL', name, e instanceof Failure ? `${e.message} (${path.relative(process.cwd(), dir)})` : `the check itself stopped: ${String(e).split('\n')[0].slice(0, 200)} (${path.relative(process.cwd(), dir)})`);
      await tab.page.screenshot({ path: path.join(dir, 'failed.png') }).catch(() => {});
    }
    await within('closing the browser', 120000, ctx.close()).catch(() => {});
    log.end();
  }
} finally {
  server.close();
  sink.unload();
}
const counted = results.filter(r => r.counted);
const failed = counted.filter(r => r.status === 'FAIL').length;
console.log(`${failed ? 'FAIL' : 'PASS'} ${browserName} overall: ${counted.filter(r => r.status === 'PASS').length} passed, ${failed} failed, ${counted.filter(r => r.status === 'INFO').length} info, ${results.length - counted.length} flaky not counted, ${Math.round((Date.now() - started) / 1000)} s`);
process.exit(failed ? 1 : 0);
