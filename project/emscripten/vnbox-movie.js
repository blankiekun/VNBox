// Movies a game plays through DirectShow, decoded and drawn by the page. VNBox's quartz
// (vn/wine/quartz-browser-movies.patch) adds no decoder to the graph when it renders an MPEG-1
// program stream: it sends the file, its video window's screen rectangle and the graph's state
// through /dev/vnbox-movie (devmovie.cpp), whose calls reach boxedwineMovie (boxedwine-shell.js)
// and, once this file and vnbox-movie-decoder.js are loaded, vnboxMovieCommand below. The file is
// read straight from its Emscripten FS node (the lazy game folder's File), in 1 MiB chunks into
// pl_mpeg's ring buffer (vnbox-movie-decoder.c), and the frames go to a canvas over the game's at
// the window's place, the sound to Web Audio. The canvas takes no input (pointer-events: none), so
// clicks and keys still reach the game, which can stop the movie. The end (or any failure) is
// reported back, and quartz turns it into EC_COMPLETE.
"use strict";

var vnboxMovieModule = null; // the decoder (one wasm instance for every movie)
var vnboxMovieAudio = null; // one AudioContext for every movie
var vnboxMovies = new Map(); // id → movie

var VNBOX_MOVIE_CHUNK = 1 << 20;
var VNBOX_MOVIE_AHEAD = 2 << 20; // keep this much undecoded data in the ring buffer
var vnboxMovieStats = vnboxParam("stats") === "1";

function VnboxMovie(blob, canvas) {
    this.blob = blob;
    this.canvas = canvas;
    this.context = canvas.getContext("2d");
    this.ended = false;
    this.stopped = true;
    this.running = false;
    this.decoded = false;
    this.rect = null;
    this.volume = 1;
    this.sources = new Set();
    this.D = null;
    this.ready = (vnboxMovieModule = vnboxMovieModule || VnboxMovieDecoder()).then(D => {
        this.D = D;
        this.restart();
    });
    this.ready.catch(error => this.fail(error));
}

VnboxMovie.prototype.fail = function (error) {
    vnboxReport("movie", error);
    this.ended = true;
    this.pause();
};

VnboxMovie.prototype.restart = function () {
    if (this.m) this.D._vm_destroy(this.m);
    this.m = this.D._vm_create();
    this.offset = 0;
    this.reading = null;
    this.ended = false;
    this.feed();
};

VnboxMovie.prototype.feed = function () {
    var D = this.D, m = this.m;
    if (this.reading || this.offset >= this.blob.size || D._vm_remaining(m) >= VNBOX_MOVIE_AHEAD) return;
    var start = this.offset, end = Math.min(this.blob.size, start + VNBOX_MOVIE_CHUNK);
    var reading = this.reading = this.blob.slice(start, end).arrayBuffer().then(buffer => {
        if (this.reading !== reading) return; // restarted or closed meanwhile
        var p = D._malloc(buffer.byteLength);
        D.HEAPU8.set(new Uint8Array(buffer), p);
        D._vm_write(m, p, buffer.byteLength);
        D._free(p);
        this.offset = end;
        if (end >= this.blob.size) D._vm_end(m);
        this.reading = null;
        this.feed();
    }, error => this.fail(error));
};

VnboxMovie.prototype.run = function () {
    if (this.running || this.ended) return;
    this.running = true;
    this.stopped = false;
    vnboxMovieAudio = vnboxMovieAudio || new AudioContext();
    if (vnboxMovieAudio.state === "suspended") vnboxMovieAudio.resume();
    this.gain = this.gain || vnboxMovieAudio.createGain();
    this.gain.gain.value = this.volume;
    this.gain.connect(vnboxMovieAudio.destination);
    this.last = performance.now() / 1000;
    this.clock = null; // set from the first decode, once the decoder is ready
    this.stats = null;
    var step = () => {
        if (!this.running) return;
        this.frame = requestAnimationFrame(step);
        try { this.step(); } catch (e) { this.fail(e); }
    };
    this.frame = requestAnimationFrame(step);
};

VnboxMovie.prototype.step = function () {
    var D = this.D, m = this.m, now = performance.now() / 1000;
    if (!D) return;
    this.feed();
    // Wait for data rather than let pl_mpeg's clock run past it (it skips what it couldn't decode).
    if (this.offset < this.blob.size && D._vm_remaining(m) < VNBOX_MOVIE_CHUNK / 4) { this.clock = null; return; }
    var time = D._vm_time(m);
    if (this.clock === null) { // (re)start: stream time 0 plays at wall now and audio now + 50 ms
        this.clock = now - time;
        this.audioClock = vnboxMovieAudio.currentTime + 0.05 - time;
    }
    var flags = D._vm_decode(m, Math.max(0, now - this.clock - time));
    if (flags & 1) this.draw();
    for (var i = 0, n = D._vm_audio_count(m); i < n; i++) this.play(D._vm_audio_time(m, i), D._vm_audio(m, i));
    if (flags & 2) {
        this.ended = true;
        this.running = false;
    }
};

VnboxMovie.prototype.draw = function () {
    var D = this.D, m = this.m, w = D._vm_width(m), h = D._vm_height(m);
    if (this.canvas.width !== w || this.canvas.height !== h) { this.canvas.width = w; this.canvas.height = h; }
    this.context.putImageData(new ImageData(new Uint8ClampedArray(D.HEAPU8.buffer, D._vm_rgba(m), w * h * 4), w, h), 0, 0);
    if (!this.decoded) {
        this.decoded = true;
        console.log("[movie] playing " + w + "x" + h + " at " + D._vm_framerate(m).toFixed(2) + " fps");
        vnboxMoviePlace(this);
    }
    this.noteShown(D._vm_frames(m));
};

// stats=1: every ~2 s while it plays, the frames shown and decoded per second and the longest gap between two shown.
VnboxMovie.prototype.noteShown = function (decoded) {
    if (!vnboxMovieStats) return;
    var now = performance.now(), s = this.stats;
    if (!s) s = this.stats = { start: now, last: now, shown: 0, decoded: decoded, gap: 0 };
    s.shown++;
    s.gap = Math.max(s.gap, now - s.last);
    s.last = now;
    var sec = (now - s.start) / 1000;
    if (sec < 2) return;
    console.log("[movie] stats shown/s=" + (s.shown / sec).toFixed(1) + " decoded/s=" + ((decoded - s.decoded) / sec).toFixed(1) +
        " maxGap=" + s.gap.toFixed(0) + "ms");
    this.stats = { start: now, last: now, shown: 0, decoded: decoded, gap: 0 };
};

VnboxMovie.prototype.play = function (time, pointer) {
    var when = this.audioClock + time, rate = this.D._vm_samplerate(this.m);
    if (when < vnboxMovieAudio.currentTime || !rate) return; // late (a stall): drop rather than pile up
    var samples = this.D.HEAPF32.subarray(pointer >> 2, (pointer >> 2) + 1152 * 2);
    var buffer = vnboxMovieAudio.createBuffer(2, 1152, rate), left = buffer.getChannelData(0), right = buffer.getChannelData(1);
    for (var i = 0; i < 1152; i++) { left[i] = samples[2 * i]; right[i] = samples[2 * i + 1]; }
    var source = vnboxMovieAudio.createBufferSource();
    source.buffer = buffer;
    source.connect(this.gain);
    source.onended = () => this.sources.delete(source);
    this.sources.add(source);
    source.start(when);
};

// shortcut: the audio decoded ahead (250 ms) is dropped on pause, a short gap on resume.
VnboxMovie.prototype.pause = function () {
    this.running = false;
    cancelAnimationFrame(this.frame);
    this.sources.forEach(source => source.stop());
    this.sources.clear();
};

VnboxMovie.prototype.destroy = function () {
    this.pause();
    this.reading = null;
    if (this.m) this.D._vm_destroy(this.m);
    this.m = null;
    if (this.gain) this.gain.disconnect();
    this.canvas.remove();
};

function vnboxMovieCanvas() { // the game's visible canvas (2D for X drawing, or the WebGL one)
    var best = null;
    for (var c of [document.getElementById("canvas"), document.getElementById("boxedwine-webgl-canvas-0")]) {
        if (c && c.offsetWidth && (!best || c.offsetWidth * c.offsetHeight > best.offsetWidth * best.offsetHeight)) best = c;
    }
    return best;
}

// rect is in guest screen pixels, which are the game canvas's pixels.
// shortcut: assumes the canvas shows the whole guest screen from its top left, as VNBox's do.
function vnboxMoviePlace(movie) {
    var game = vnboxMovieCanvas(), r = movie.rect, style = movie.canvas.style;
    var show = game && r && r[2] > 0 && r[3] > 0 && !movie.stopped && movie.decoded;
    style.display = show ? "block" : "none";
    if (!show) return;
    var box = game.getBoundingClientRect(), sx = box.width / game.width, sy = box.height / game.height;
    style.left = box.left + r[0] * sx + "px";
    style.top = box.top + r[1] * sy + "px";
    style.width = r[2] * sx + "px";
    style.height = r[3] * sy + "px";
}

function vnboxMovieCommand(id, cmd, arg) {
    if (cmd === "open") {
        var blob = vnboxMovieBlob(arg);
        if (!blob) return -1;
        var canvas = document.createElement("canvas");
        canvas.style.cssText = "position: fixed; display: none; pointer-events: none; background: #000;";
        var game = vnboxMovieCanvas() || document.body.lastChild;
        game.parentNode.insertBefore(canvas, game.nextSibling); // under the page's overlays, like the game
        vnboxMovies.set(id, new VnboxMovie(blob, canvas));
        return 1;
    }
    var movie = vnboxMovies.get(id);
    if (!movie) return 1;
    if (cmd === "status") return movie.ended ? 1 : 0;
    if (cmd === "close") {
        vnboxMovies.delete(id);
        movie.destroy();
        return 0;
    }
    if (cmd === "rect") {
        movie.rect = arg.split(" ").map(Number);
    } else if (cmd === "run") {
        movie.ready.then(() => movie.run());
    } else if (cmd === "pause") {
        movie.stopped = false;
        movie.pause();
    } else if (cmd === "stop") {
        movie.stopped = true;
        movie.pause();
    } else if (cmd === "seek") {
        // shortcut: only a seek to the start restarts (the ring buffer can't seek back); others
        // keep the position, which is what quartz's seek to the current position on Stop means.
        if (Number(arg) === 0 && movie.D) { movie.pause(); movie.restart(); }
    } else if (cmd === "volume") {
        movie.volume = Number(arg) / 1000;
        if (movie.gain) movie.gain.gain.value = movie.volume;
    }
    vnboxMoviePlace(movie);
    return 0;
}

window.addEventListener("resize", () => vnboxMovies.forEach(vnboxMoviePlace));
