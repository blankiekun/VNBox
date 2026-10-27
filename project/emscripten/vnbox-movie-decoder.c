// The decoder behind vnbox-movie.js: pl_mpeg (lib/pl_mpeg, MIT) reading an MPEG-1 program stream
// from a ring buffer the page fills, built into vnbox-movie-decoder.js (makefile). After each
// vm_decode the page takes the newest frame as RGBA and the audio frames decoded since the last call.
#include <stdlib.h>
#include <string.h>
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "../../lib/pl_mpeg/pl_mpeg.h"
#include <emscripten.h>

#define MAX_AUDIO 64 // 1.5 s of audio frames per call; more (a long stall) are dropped

typedef struct {
    plm_t *plm;
    plm_buffer_t *buffer;
    plm_frame_t *frame;
    uint8_t *rgba;
    int audio_count;
    double audio_time[MAX_AUDIO];
    float audio[MAX_AUDIO][PLM_AUDIO_SAMPLES_PER_FRAME * 2];
} movie_t;

static void on_video(plm_t *plm, plm_frame_t *frame, void *user) {
    ((movie_t *)user)->frame = frame;
}

static void on_audio(plm_t *plm, plm_samples_t *samples, void *user) {
    movie_t *m = user;
    if (m->audio_count == MAX_AUDIO) return;
    m->audio_time[m->audio_count] = samples->time;
    memcpy(m->audio[m->audio_count++], samples->interleaved, sizeof(m->audio[0]));
}

EMSCRIPTEN_KEEPALIVE movie_t *vm_create(void) {
    movie_t *m = calloc(1, sizeof(movie_t));
    m->buffer = plm_buffer_create_with_capacity(4 << 20);
    m->plm = plm_create_with_buffer(m->buffer, 1);
    plm_set_video_decode_callback(m->plm, on_video, m);
    plm_set_audio_decode_callback(m->plm, on_audio, m);
    plm_set_audio_lead_time(m->plm, 0.25);
    return m;
}

EMSCRIPTEN_KEEPALIVE void vm_write(movie_t *m, uint8_t *bytes, int length) { plm_buffer_write(m->buffer, bytes, length); }
EMSCRIPTEN_KEEPALIVE void vm_end(movie_t *m) { plm_buffer_signal_end(m->buffer); }
EMSCRIPTEN_KEEPALIVE int vm_remaining(movie_t *m) { return (int)plm_buffer_get_remaining(m->buffer); }
EMSCRIPTEN_KEEPALIVE double vm_time(movie_t *m) { return plm_get_time(m->plm); }
EMSCRIPTEN_KEEPALIVE int vm_width(movie_t *m) { return plm_get_width(m->plm); }
EMSCRIPTEN_KEEPALIVE int vm_height(movie_t *m) { return plm_get_height(m->plm); }
EMSCRIPTEN_KEEPALIVE int vm_samplerate(movie_t *m) { return plm_get_samplerate(m->plm); }
EMSCRIPTEN_KEEPALIVE uint8_t *vm_rgba(movie_t *m) { return m->rgba; }
EMSCRIPTEN_KEEPALIVE int vm_audio_count(movie_t *m) { return m->audio_count; }
EMSCRIPTEN_KEEPALIVE double vm_audio_time(movie_t *m, int i) { return m->audio_time[i]; }
EMSCRIPTEN_KEEPALIVE float *vm_audio(movie_t *m, int i) { return m->audio[i]; }

// Decodes up to seconds further; bit 1: a new frame is in vm_rgba, bit 2: the movie has ended.
EMSCRIPTEN_KEEPALIVE int vm_decode(movie_t *m, double seconds) {
    m->frame = NULL;
    m->audio_count = 0;
    plm_decode(m->plm, seconds);
    int flags = plm_has_ended(m->plm) ? 2 : 0;
    if (m->frame) {
        if (!m->rgba) {
            m->rgba = malloc(m->frame->width * m->frame->height * 4);
            memset(m->rgba, 255, m->frame->width * m->frame->height * 4); // alpha, never written
        }
        plm_frame_to_rgba(m->frame, m->rgba, m->frame->width * 4);
        flags |= 1;
    }
    return flags;
}

EMSCRIPTEN_KEEPALIVE void vm_destroy(movie_t *m) {
    plm_destroy(m->plm);
    free(m->rgba);
    free(m);
}
