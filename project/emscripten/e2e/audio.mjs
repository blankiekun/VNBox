// A null sink for the browser's sound, and what was captured from it. The sink is unloaded by its module id only:
// unloading by name would take other programs' null sinks with it.
import { execFileSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import { sleep } from './site.mjs';

const RATE = 48000, CHANNELS = 2, FRAME_BYTES = 2 * CHANNELS;
const WINDOW_MS = 10;
// Below this peak a 10 ms window counts as silent: about -70 dBFS, under dither and resampler noise.
const SILENT_PEAK = 10;
// Silence shorter than this can be in the music itself.
export const GAP_MS = 50;

const pactl = (...args) => execFileSync('pactl', args, { encoding: 'utf8' }).trim();

export function loadSink(name) {
  // A run that was killed left its sink: found by its exact name, unloaded by its own id.
  for (const row of pactl('list', 'short', 'modules').split('\n')) {
    const [id, module, args] = row.split('\t');
    if (module === 'module-null-sink' && (args || '').split(/\s+/).includes('sink_name=' + name)) pactl('unload-module', id);
  }
  const id = pactl('load-module', 'module-null-sink', 'sink_name=' + name, 'sink_properties=device.description=' + name);
  return { name, unload: () => pactl('unload-module', id) };
}

// Captures the sink's monitor into file until stop() is called.
export function capture(sink, file) {
  const out = fs.openSync(file, 'w');
  const child = spawn('parec', ['-d', sink + '.monitor', '--format=s16le', '--rate=' + RATE, '--channels=' + CHANNELS, '--raw'], { stdio: ['ignore', out, 'ignore'] });
  return { stop: async () => { child.kill(); await sleep(200); fs.closeSync(out); return fs.readFileSync(file); } };
}

// Raw s16le stereo to numbers. Gaps are silences of GAP_MS or more between the first and the last sound.
export function analyze(buf) {
  const frames = Math.floor(buf.length / FRAME_BYTES), windowFrames = RATE * WINDOW_MS / 1000;
  const samples = new Int16Array(buf.buffer, buf.byteOffset, frames * CHANNELS);
  let sum = 0, peak = 0, loudFrames = 0, first = -1, last = -1;
  const silent = [];
  for (let w = 0; w * windowFrames < frames; w++) {
    let wPeak = 0, wSum = 0;
    const end = Math.min((w + 1) * windowFrames, frames) * CHANNELS;
    for (let i = w * windowFrames * CHANNELS; i < end; i++) { const s = Math.abs(samples[i]); wSum += s * s; if (s > wPeak) wPeak = s; }
    const quiet = wPeak < SILENT_PEAK;
    silent.push(quiet);
    if (!quiet) { sum += wSum; loudFrames += (end / CHANNELS) - w * windowFrames; if (first < 0) first = w; last = w; }
    if (wPeak > peak) peak = wPeak;
  }
  const gaps = [];
  let run = 0;
  for (let w = first; first >= 0 && w <= last; w++) {
    if (silent[w]) run++;
    else { if (run * WINDOW_MS >= GAP_MS) gaps.push(run * WINDOW_MS); run = 0; }
  }
  const db = x => x > 0 ? +(20 * Math.log10(x / 32768)).toFixed(1) : null;
  return {
    seconds: +(frames / RATE).toFixed(1),
    soundSeconds: first < 0 ? 0 : +((last - first + 1) * WINDOW_MS / 1000).toFixed(1),
    rmsDb: loudFrames ? db(Math.sqrt(sum / (loudFrames * CHANNELS))) : null,
    peakDb: db(peak),
    gaps: gaps.length,
    gapMs: gaps.reduce((a, b) => a + b, 0),
    longestGapMs: gaps.length ? Math.max(...gaps) : 0,
  };
}
