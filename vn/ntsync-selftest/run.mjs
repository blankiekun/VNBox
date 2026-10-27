// node run.mjs <chromium|firefox> <url> <logfile> [timeoutSec]: logs the player console until the TAP totals line.
// Playwright from PW_FROM (a package.json whose node_modules has playwright).
import { createRequire } from 'node:module';
import fs from 'node:fs';
const require = createRequire(process.env.PW_FROM);
const pw = require('playwright');
const [bname, url, logf, tsec] = process.argv.slice(2);
const log = fs.createWriteStream(logf);
const t0 = Date.now(); const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
const browser = await pw[bname].launch({ headless: true });
const page = await browser.newPage();
let done;
const finished = new Promise(r => done = r);
page.on('console', m => { const t = m.text(); log.write(`[${stamp()}] ${t}\n`); if (/# Totals:|# FAILED|^Bail out/.test(t) || /Totals: pass/.test(t)) setTimeout(done, 500); });
page.on('pageerror', e => log.write(`[${stamp()}] PAGEERROR ${e}\n`));
await page.goto(url);
await Promise.race([finished, new Promise(r => setTimeout(r, (+tsec || 300) * 1000))]);
log.write(`[${stamp()}] end\n`); log.end();
await browser.close();
