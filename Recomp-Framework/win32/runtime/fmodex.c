// FMOD Ex 4.01 (fmodex.dll, the stdcall C API) for 32-bit guests, on CoreAudio.
//
// Covers what Call of Cthulhu DCoTE uses: sounds created from memory (FMOD_OPENMEMORY) as samples or streams,
// 2D and 3D channels (listener, min/max distance, inverse rolloff, doppler), priorities and channel stealing, and
// the lowpass DSP the game puts on occluded channels. Samples decode completely at creation; streams keep their
// compressed data and decode on the mixing thread as they play. Formats: Ogg Vorbis (stb_vorbis) and RIFF WAVE PCM.
// Mixing runs on the CoreAudio thread from host data only.
//
// Output: the default output unit pulls an AUSpatialMixer (macOS). Its input 0 is the stereo mix of the channels
// (balance panning for 3D channels), passed through. With Headphone Audio (rt_enh.h hrtf), each 3D channel i plays
// on its own mono input 1 + i instead, as a point source of the head-related transfer function (HRTF) of the mixer
// for headphones (kSpatializationAlgorithm_UseOutputType, kSpatialMixerOutputType_Headphones; a personalized HRTF of
// the user when macOS has one). The direction of the source in the listener frame sets its azimuth and elevation.
// The attenuation of the distance and the doppler stay those of this file, so the mixer gets a fixed distance at its
// reference distance (no attenuation).
//
// Constants were checked against the game's own fmodex.dll (FMOD_System_GetVersion reports 0x40105): every C
// entry point validates its object and returns 0x21 (FMOD_ERR_INVALID_HANDLE) for one it doesn't know.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../mac32/third_party/stb_vorbis.c"
// stb_vorbis leaves its channel-mask macros defined; C would clash with cpu32.h's register-file macro
#undef L
#undef C
#undef R
#include "rt32.h"
#include "rt_enh.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>

#define FMOD_OK 0
#define FMOD_ERR_FORMAT 0x17
#define FMOD_ERR_INVALID_PARAM 0x25
#define FMOD_ERR_INVALID_HANDLE 0x21
#define FMOD_ERR_MEMORY 0x29
#define FMOD_ERR_CHANNEL_ALLOC 0x0A
#define FMOD_ERR_OUTPUT_INIT 0x31

// FMOD_MODE bits
#define M_LOOP_OFF      0x1
#define M_LOOP_NORMAL   0x2
#define M_LOOP_BIDI     0x4
#define M_2D            0x8
#define M_3D            0x10
#define M_CREATESTREAM  0x80
#define M_OPENMEMORY    0x800
#define M_OPENRAW       0x1000
// FMOD_TIMEUNIT
#define TU_MS 1
#define TU_PCM 2
#define TU_PCMBYTES 4
#define DSP_LOWPASS 3

static int trace = -1;
#define T(...) do { if (trace < 0) trace = getenv("RT_TRACE_FMOD") != NULL; if (trace) rt_log("[fmod] " __VA_ARGS__); } while (0)

// ---------------- objects ----------------
// Handles are opaque values the game stores and passes back (it never reads through them): a tag in the top
// bits, an index, and for channels a generation, so a handle to a channel that ended or was reused is detected.
#define H_SYSTEM 0x90000000u
#define H_SOUND  0xA0000000u
#define H_DSP    0xB0000000u
#define H_CHAN   0xC0000000u
#define MAXSND 16384
#define MAXCH 64
#define MAXDSP 256

typedef struct {
    int used;
    uint32_t mode;
    int nch; float rate;
    uint32_t frames;            // length in sample frames
    int16_t *pcm;               // samples: decoded PCM (interleaved)
    uint8_t *data; uint32_t len;   // streams: the compressed file
    int is_ogg;
    int refs;                   // channels playing it (a released stream frees its data when the last one stops)
    int released;
} Snd;

typedef struct {
    int used, playing, paused;
    uint32_t gen;
    int sound;
    int priority;
    uint32_t mode;
    float vol, freq;
    double pos;                 // source frames from the start (samples), or from the decoder's start (streams)
    // 3D
    float p[3], v[3], mind, maxd;
    // stream decoding
    stb_vorbis *vb;
    float sbuf[2048 * 2]; uint32_t sbuf_n; double sbuf_base;
    int stream_done;
    // lowpass DSP
    int dsp;
    float z[2][2];              // biquad state per output side
} Chan;

typedef struct { int used, chan; int type; float cutoff, resonance; } Dsp;

static Snd snds[MAXSND];
static Chan chans[MAXCH];
static Dsp dsps[MAXDSP];
static int nchannels = 32;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static int sys_created, sys_init;
static float out_rate = 48000;
static float l_pos[3], l_vel[3], l_fwd[3] = {0, 0, 1}, l_up[3] = {0, 1, 0};
static float doppler = 1, distfactor = 1, rolloff = 1;
static AudioUnit unit, mixer;
static int bus_on[MAXCH];          // render thread: input 1 + i of the mixer is enabled, with the direction of channel i
static int hrtf_live[MAXCH];       // render thread: channel i plays on input 1 + i in this render cycle
static int hrtf_logged = -1;       // the Headphone Audio state of the last render cycle (written by the render thread)
static _Atomic OSStatus render_err;   // the first error of the mixer's render (reported by System_Update)
// The gain of the HRTF inputs: measured with white noise through the mixer (headphones, a source in front at its
// reference distance), the sum of the energy of the two ears is 1.113 x the energy of the input, and 2.0 x with the
// balance panning of input 0 (the full source on both sides). +2.55 dB = 10 log10(2.0 / 1.113) keeps the loudness of
// a source in front.
#define HRTF_GAIN_DB 2.55f

static uint32_t snd_handle(int i) { return H_SOUND | (uint32_t)i; }
static int snd_index(uint32_t h) { uint32_t i = h & 0x0FFFFFFFu; return (h & 0xF0000000u) == H_SOUND && i < MAXSND && snds[i].used && !snds[i].released ? (int)i : -1; }
static uint32_t ch_handle(int i) { return H_CHAN | (chans[i].gen & 0x3FFFFFu) << 6 | (uint32_t)i; }
static int ch_index(uint32_t h) {
    if ((h & 0xF0000000u) != H_CHAN) return -1;
    uint32_t i = h & 63, g = (h >> 6) & 0x3FFFFFu;
    return i < MAXCH && chans[i].used && (chans[i].gen & 0x3FFFFFu) == g ? (int)i : -1;
}
static uint32_t dsp_handle(int i) { return H_DSP | (uint32_t)i; }
static int dsp_index(uint32_t h) { uint32_t i = h & 0x0FFFFFFFu; return (h & 0xF0000000u) == H_DSP && i < MAXDSP && dsps[i].used ? (int)i : -1; }

// ---------------- decoding ----------------
static int wav_parse(const uint8_t *d, uint32_t len, int *nch, float *rate, uint32_t *bits, uint32_t *off, uint32_t *bytes) {
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    uint32_t p = 12; int fmt = 0;
    while (p + 8 <= len) {
        uint32_t sz = (uint32_t)d[p + 4] | (uint32_t)d[p + 5] << 8 | (uint32_t)d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
        if (!memcmp(d + p, "fmt ", 4) && sz >= 16 && p + 8 + 16 <= len) {
            uint16_t tag = (uint16_t)(d[p + 8] | d[p + 9] << 8);
            if (tag != 1) return 0;   // PCM only
            *nch = d[p + 10] | d[p + 11] << 8;
            *rate = (float)((uint32_t)d[p + 12] | (uint32_t)d[p + 13] << 8 | (uint32_t)d[p + 14] << 16 | (uint32_t)d[p + 15] << 24);
            *bits = (uint32_t)(d[p + 22] | d[p + 23] << 8);
            fmt = 1;
        } else if (!memcmp(d + p, "data", 4) && fmt) {
            *off = p + 8; *bytes = sz <= len - (p + 8) ? sz : len - (p + 8);
            return *nch >= 1 && *nch <= 2 && (*bits == 8 || *bits == 16) && *rate > 0;
        }
        p += 8 + sz + (sz & 1);
    }
    return 0;
}
static inline int16_t wav_sample(const uint8_t *s, uint32_t bits) {
    return bits == 8 ? (int16_t)(((int)s[0] - 128) << 8) : (int16_t)(s[0] | s[1] << 8);
}

// ---------------- mixer (CoreAudio thread) ----------------
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

// 3D: inverse-distance rolloff between min and max distance (att), a left-handed listener frame
// (FMOD_INIT_3D_RIGHTHANDED is not set), doppler from the velocities (fmul). loc: the direction to the source in the
// listener frame (x right, y up, z forward; scaled by the distance); dist: the distance.
static void spatial(Chan *ch, float *att, float *fmul, float loc[3], float *dist_out) {
    float d[3] = {ch->p[0] - l_pos[0], ch->p[1] - l_pos[1], ch->p[2] - l_pos[2]};
    float dist = sqrtf(dot3(d, d));
    float mind = ch->mind > 0 ? ch->mind : 1, maxd = ch->maxd > mind ? ch->maxd : mind;
    float cd = dist < mind ? mind : dist > maxd ? maxd : dist;
    *att = mind / (mind + rolloff * (cd - mind));
    float right[3]; cross3(l_up, l_fwd, right);
    float rl = sqrtf(dot3(right, right)), ul = sqrtf(dot3(l_up, l_up)), fl = sqrtf(dot3(l_fwd, l_fwd));
    loc[0] = rl > 1e-6f ? dot3(d, right) / rl : 0;
    loc[1] = ul > 1e-6f ? dot3(d, l_up) / ul : 0;
    loc[2] = fl > 1e-6f ? dot3(d, l_fwd) / fl : 0;
    *dist_out = dist;
    *fmul = 1;
    if (doppler > 0 && dist > 1e-4f) {
        float c = 340.0f * distfactor;
        float vl = dot3(l_vel, d) / dist, vs = dot3(ch->v, d) / dist;
        float den = c + doppler * vs, num = c + doppler * vl;
        if (den > 1e-3f && num > 0) { *fmul = num / den; if (*fmul > 4) *fmul = 4; if (*fmul < 0.25f) *fmul = 0.25f; }
    }
}
// the gains of the balance panning of input 0: linear between the sides, the full source in the middle
static void balance(Chan *ch, float *gl, float *gr, float *fmul) {
    float att, loc[3], dist;
    spatial(ch, &att, fmul, loc, &dist);
    float pan = dist > 1e-4f ? loc[0] / dist : 0;
    *gl = att * (pan > 0 ? 1 - pan : 1);
    *gr = att * (pan < 0 ? 1 + pan : 1);
}
// the azimuth (degrees, -180 .. 180, positive to the right; measured with the mixer: +90 is the right ear) and the
// elevation (degrees, -90 .. 90, positive up) of the source of a 3D channel for the HRTF input
static void direction(Chan *ch, float *az, float *el, float *att, float *fmul) {
    float loc[3], dist;
    spatial(ch, att, fmul, loc, &dist);
    if (dist <= 1e-4f) { *az = 0; *el = 0; return; }
    *az = atan2f(loc[0], loc[2]) * (180.0f / (float)M_PI);
    *el = atan2f(loc[1], sqrtf(loc[0] * loc[0] + loc[2] * loc[2])) * (180.0f / (float)M_PI);
}
// stream frame access: refill the channel's decode buffer so frames [i, i+1] are present; 0 when the stream ended
static int stream_fill(Chan *ch, Snd *s, double want) {
    while (want + 1 >= ch->sbuf_base + ch->sbuf_n) {
        if (ch->stream_done) return 0;
        // keep the last frame for interpolation across the refill
        float keep[2] = {0, 0}; int had = ch->sbuf_n > 0;
        if (had) { keep[0] = ch->sbuf[(ch->sbuf_n - 1) * 2]; keep[1] = ch->sbuf[(ch->sbuf_n - 1) * 2 + 1]; }
        double base = had ? ch->sbuf_base + ch->sbuf_n - 1 : ch->sbuf_base;
        uint32_t n = had ? 1 : 0;
        if (had) { ch->sbuf[0] = keep[0]; ch->sbuf[1] = keep[1]; }
        uint32_t cap = sizeof ch->sbuf / sizeof ch->sbuf[0] / 2;
        // only Ogg Vorbis sounds stream (WAVE PCM is always held decoded in memory)
        int rewound = 0;
        while (n < cap) {
            float tmp[2 * 512];
            uint32_t want_n = cap - n < 512 ? cap - n : 512;
            // the float reader does not copy a mono stream into the second channel, so mono is duplicated here
            int got = stb_vorbis_get_samples_float_interleaved(ch->vb, 2, tmp, (int)(want_n * 2));
            for (int k = 0; k < got; k++) { ch->sbuf[(n + (uint32_t)k) * 2] = tmp[k * 2]; ch->sbuf[(n + (uint32_t)k) * 2 + 1] = s->nch > 1 ? tmp[k * 2 + 1] : tmp[k * 2]; }
            if (got <= 0) {
                // a looping stream restarts once per refill; a stream that yields nothing even after a rewind is ended
                if ((ch->mode & M_LOOP_NORMAL) && !rewound) { stb_vorbis_seek_start(ch->vb); rewound = 1; continue; }
                ch->stream_done = 1;
                break;
            }
            n += (uint32_t)got; rewound = 0;
        }
        ch->sbuf_base = base; ch->sbuf_n = n;
        if (n < 2 && ch->stream_done) return 0;
    }
    return 1;
}
static inline void frame_at(Chan *ch, Snd *s, double pos, float *l, float *r) {
    if (s->pcm) {
        uint32_t i0 = (uint32_t)pos, i1 = i0 + 1;
        if (i1 >= s->frames) i1 = (ch->mode & M_LOOP_NORMAL) ? 0 : i0;
        float t = (float)(pos - i0);
        const int16_t *a = s->pcm + (size_t)i0 * (size_t)s->nch, *b = s->pcm + (size_t)i1 * (size_t)s->nch;
        float a0 = a[0] / 32768.0f, b0 = b[0] / 32768.0f;
        float a1 = s->nch > 1 ? a[1] / 32768.0f : a0, b1 = s->nch > 1 ? b[1] / 32768.0f : b0;
        *l = a0 + (b0 - a0) * t; *r = a1 + (b1 - a1) * t;
    } else {
        double rel = pos - ch->sbuf_base;
        uint32_t i0 = (uint32_t)rel, i1 = i0 + 1 < ch->sbuf_n ? i0 + 1 : i0;
        float t = (float)(rel - i0);
        *l = ch->sbuf[i0 * 2] + (ch->sbuf[i1 * 2] - ch->sbuf[i0 * 2]) * t;
        *r = ch->sbuf[i0 * 2 + 1] + (ch->sbuf[i1 * 2 + 1] - ch->sbuf[i0 * 2 + 1]) * t;
    }
}
static void chan_stop_locked(Chan *ch) {
    if (!ch->used) return;
    if (ch->vb) { stb_vorbis_close(ch->vb); ch->vb = NULL; }
    if (ch->sound >= 0) {
        Snd *s = &snds[ch->sound];
        if (--s->refs <= 0 && s->released) { free(s->pcm); free(s->data); memset(s, 0, sizeof *s); }
    }
    if (ch->dsp >= 0) dsps[ch->dsp].chan = -1;
    ch->used = 0; ch->playing = 0; ch->sound = -1; ch->dsp = -1;
    ch->gen++;
}
static int chan_live(const Chan *ch) { return ch->used && ch->playing && !ch->paused && ch->sound >= 0; }
static int hrtf_on(void) { return rt_enh && rt_enh->hrtf; }
// mixes n frames of channel ch into outl/outr with the gains gl/gr and the frequency factor fm. mono: the HRTF input
// (outl only, gain gl). Locked.
static void chan_mix(Chan *ch, float *outl, float *outr, UInt32 n, float gl, float gr, float fm, int mono) {
    Snd *s = &snds[ch->sound];
    double step = ch->freq * fm / out_rate;
    // lowpass biquad (RBJ), recomputed per buffer from the DSP's parameters
    int lp = ch->dsp >= 0 && dsps[ch->dsp].type == DSP_LOWPASS && dsps[ch->dsp].cutoff < out_rate * 0.49f;
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    if (lp) {
        float fc = dsps[ch->dsp].cutoff < 10 ? 10 : dsps[ch->dsp].cutoff;
        float q = 0.70710678f * (dsps[ch->dsp].resonance < 1 ? 1 : dsps[ch->dsp].resonance);
        float w = 2 * (float)M_PI * fc / out_rate, cw = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
        b0 = (1 - cw) / 2 / a0; b1 = (1 - cw) / a0; b2 = b0; a1 = -2 * cw / a0; a2 = (1 - al) / a0;
    }
    for (UInt32 k = 0; k < n; k++) {
        if (s->pcm) {
            if (ch->pos >= s->frames) {
                if ((ch->mode & M_LOOP_NORMAL) && s->frames) ch->pos = fmod(ch->pos, (double)s->frames);
                else { ch->playing = 0; break; }
            }
        } else if (!stream_fill(ch, s, ch->pos)) { ch->playing = 0; break; }
        float l, r; frame_at(ch, s, ch->pos, &l, &r);
        if (ch->mode & M_3D) { float m = (l + r) * 0.5f; l = r = m; }   // 3D: the source is a point
        if (lp) {
            float x[2] = {l, r}, y[2];
            for (int sd = 0; sd < 2; sd++) {   // transposed direct form II
                y[sd] = b0 * x[sd] + ch->z[sd][0];
                ch->z[sd][0] = b1 * x[sd] - a1 * y[sd] + ch->z[sd][1];
                ch->z[sd][1] = b2 * x[sd] - a2 * y[sd];
            }
            l = y[0]; r = y[1];
        }
        if (mono) outl[k] += l * gl;
        else {
            outl[k] += l * gl;
            if (outr) outr[k] += r * gr; else outl[k] += r * gr;
        }
        ch->pos += step;
    }
    if (!ch->playing) chan_stop_locked(ch);
}
// the inputs of the mixer: input 0 the stereo mix of the channels that are not on an HRTF input; input 1 + i channel
// i while it is on its HRTF input (hrtf_live, set by out_render before the mixer renders)
static OSStatus bus_render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)ref; (void)ts;
    float *outl = io->mBuffers[0].mData, *outr = io->mNumberBuffers > 1 ? io->mBuffers[1].mData : NULL;
    memset(outl, 0, n * sizeof(float)); if (outr) memset(outr, 0, n * sizeof(float));
    if (rt_paused()) { *fl |= kAudioUnitRenderAction_OutputIsSilence; return noErr; }   // paused in the background
    pthread_mutex_lock(&mx);
    if (bus == 0) {
        for (int i = 0; i < MAXCH; i++) {
            Chan *ch = &chans[i];
            if (!chan_live(ch) || hrtf_live[i]) continue;
            float gl = ch->vol, gr = ch->vol, fm = 1;
            if (ch->mode & M_3D) { float a, b; balance(ch, &a, &b, &fm); gl *= a; gr *= b; }
            chan_mix(ch, outl, outr, n, gl, gr, fm, 0);
        }
    } else if (bus <= MAXCH && hrtf_live[bus - 1]) {
        Chan *ch = &chans[bus - 1];
        if (chan_live(ch) && (ch->mode & M_3D)) {
            float az, el, att, fm;
            direction(ch, &az, &el, &att, &fm);
            chan_mix(ch, outl, NULL, n, ch->vol * att, 0, fm, 1);
        }
    }
    pthread_mutex_unlock(&mx);
    return noErr;
}
// the output unit's input: the direction of each HRTF channel goes to its mixer input, then the mixer renders
static OSStatus out_render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)ref; (void)bus;
    int on = hrtf_on();
    OSStatus e = noErr;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < MAXCH && !e; i++) {
        Chan *ch = &chans[i];
        int live = on && chan_live(ch) && (ch->mode & M_3D);
        if (live) {
            float az, el, att, fm;
            direction(ch, &az, &el, &att, &fm);
            if ((e = AudioUnitSetParameter(mixer, kSpatialMixerParam_Azimuth, kAudioUnitScope_Input, (UInt32)(1 + i), az, 0)) ||
                (e = AudioUnitSetParameter(mixer, kSpatialMixerParam_Elevation, kAudioUnitScope_Input, (UInt32)(1 + i), el, 0))) break;
        }
        if (live != bus_on[i]) {
            if ((e = AudioUnitSetParameter(mixer, kSpatialMixerParam_Enable, kAudioUnitScope_Input, (UInt32)(1 + i), live ? 1.0f : 0.0f, 0))) break;
            bus_on[i] = live;
        }
        hrtf_live[i] = live;
    }
    pthread_mutex_unlock(&mx);
    if (!e) e = AudioUnitRender(mixer, fl, ts, 0, n, io);
    if (e) {
        if (!render_err) render_err = e;
        for (UInt32 b = 0; b < io->mNumberBuffers; b++) memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
        *fl |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }
    __atomic_store_n(&hrtf_logged, on, __ATOMIC_RELAXED);
    float *outl = io->mBuffers[0].mData, *outr = io->mNumberBuffers > 1 ? io->mBuffers[1].mData : NULL;
    for (UInt32 k = 0; k < n; k++) {
        if (outl[k] > 1) outl[k] = 1; else if (outl[k] < -1) outl[k] = -1;
        if (outr) { if (outr[k] > 1) outr[k] = 1; else if (outr[k] < -1) outr[k] = -1; }
    }
    return noErr;
}
static void audio_stop(void) {
    if (unit) { AudioOutputUnitStop(unit); AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); unit = NULL; }
    if (mixer) { AudioUnitUninitialize(mixer); AudioComponentInstanceDispose(mixer); mixer = NULL; }
}
static int audio_start(void) {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit)) { rt_log("[fmod] no audio output device"); unit = NULL; return 0; }
    AudioComponentDescription md = {kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent mcomp = AudioComponentFindNext(NULL, &md);
    if (!mcomp || AudioComponentInstanceNew(mcomp, &mixer)) { rt_log("[fmod] no AUSpatialMixer"); mixer = NULL; audio_stop(); return 0; }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = out_rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mChannelsPerFrame = 2; f.mFramesPerPacket = 1; f.mBitsPerChannel = 32; f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    AudioStreamBasicDescription fm = f; fm.mChannelsPerFrame = 1;
    UInt32 inputs = 1 + MAXCH, maxf = 0, sz = sizeof maxf;
    UInt32 bypass = kSpatialMixerSourceMode_Bypass, point = kSpatialMixerSourceMode_PointSource;
    UInt32 alg = kSpatializationAlgorithm_UseOutputType, otype = kSpatialMixerOutputType_Headphones;
    UInt32 personal = kSpatialMixerPersonalizedHRTFMode_Auto;
    MixerDistanceParams dp = {1.0f, 1000.0f, 0.0f};   // reference distance 1 m: no attenuation at the fixed distance 1 m
    AURenderCallbackStruct in_cb = {bus_render, NULL}, out_cb = {out_render, NULL};
    const char *step = "";
    OSStatus e;
#define AU(what, call) (step = what, (e = (call)) != noErr)
    if (AU("output frames", AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxf, &sz)) ||
        AU("mixer frames", AudioUnitSetProperty(mixer, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxf, sizeof maxf)) ||
        AU("mixer inputs", AudioUnitSetProperty(mixer, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inputs, sizeof inputs)) ||
        AU("mixer output format", AudioUnitSetProperty(mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f)) ||
        AU("mixer output type", AudioUnitSetProperty(mixer, kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global, 0, &otype, sizeof otype)) ||
        AU("mixer personalized HRTF", AudioUnitSetProperty(mixer, kAudioUnitProperty_SpatialMixerPersonalizedHRTFMode, kAudioUnitScope_Global, 0, &personal, sizeof personal)) ||
        AU("mixer input 0 format", AudioUnitSetProperty(mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) ||
        AU("mixer input 0 mode", AudioUnitSetProperty(mixer, kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input, 0, &bypass, sizeof bypass)) ||
        AU("mixer input 0 callback", AudioUnitSetProperty(mixer, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &in_cb, sizeof in_cb)))
        goto fail;
    for (UInt32 b = 1; b < inputs; b++)
        if (AU("HRTF input format", AudioUnitSetProperty(mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, b, &fm, sizeof fm)) ||
            AU("HRTF input mode", AudioUnitSetProperty(mixer, kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input, b, &point, sizeof point)) ||
            AU("HRTF input algorithm", AudioUnitSetProperty(mixer, kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input, b, &alg, sizeof alg)) ||
            AU("HRTF input distance", AudioUnitSetProperty(mixer, kAudioUnitProperty_SpatialMixerDistanceParams, kAudioUnitScope_Input, b, &dp, sizeof dp)) ||
            AU("HRTF input callback", AudioUnitSetProperty(mixer, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, b, &in_cb, sizeof in_cb)))
            goto fail;
    if (AU("mixer initialize", AudioUnitInitialize(mixer))) goto fail;
    for (UInt32 b = 1; b < inputs; b++)
        if (AU("HRTF input distance", AudioUnitSetParameter(mixer, kSpatialMixerParam_Distance, kAudioUnitScope_Input, b, 1.0f, 0)) ||
            AU("HRTF input gain", AudioUnitSetParameter(mixer, kSpatialMixerParam_Gain, kAudioUnitScope_Input, b, HRTF_GAIN_DB, 0)) ||
            AU("HRTF input enable", AudioUnitSetParameter(mixer, kSpatialMixerParam_Enable, kAudioUnitScope_Input, b, 0.0f, 0)))
            goto fail;
    memset(bus_on, 0, sizeof bus_on); memset(hrtf_live, 0, sizeof hrtf_live);
    if (AU("output format", AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f)) ||
        AU("output callback", AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &out_cb, sizeof out_cb)) ||
        AU("output initialize", AudioUnitInitialize(unit)) ||
        (rt_mute && AU("output mute", AudioUnitSetParameter(unit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, 0.0f, 0))) ||
        AU("output start", AudioOutputUnitStart(unit)))
        goto fail;
#undef AU
    rt_log("[fmod] output: AUSpatialMixer, %u inputs (stereo mix and %d HRTF inputs for headphones), %u frames per slice",
           (unsigned)inputs, MAXCH, (unsigned)maxf);
    return 1;
fail:
    rt_log("[fmod] audio output setup failed at \"%s\" (OSStatus %d)", step, (int)e);
    audio_stop();
    return 0;
}
// game thread (System_Update): the Headphone Audio state of the mixer and a render error go to the log
static void audio_report(void) {
    static int logged = -1;
    static OSStatus err_logged;
    int st = __atomic_load_n(&hrtf_logged, __ATOMIC_RELAXED);
    if (st >= 0 && st != logged) { rt_log("[fmod] Headphone Audio (HRTF) %s", st ? "on" : "off"); logged = st; }
    OSStatus e = render_err;
    if (e && e != err_logged) { rt_log("[fmod] the mixer did not render (OSStatus %d); the output is silent", (int)e); err_logged = e; }
}

// ---------------- System ----------------
SHIM(fmodex_FMOD_System_Create, 1) {
    if (!ARG(0)) return FMOD_ERR_INVALID_PARAM;
    sys_created = 1;
    wr32(ARG(0), H_SYSTEM);
    return FMOD_OK;
}
#define SYS_OK() do { if (ARG(0) != H_SYSTEM || !sys_created) return FMOD_ERR_INVALID_HANDLE; } while (0)
SHIM(fmodex_FMOD_System_Init, 4) {
    SYS_OK();
    int32_t maxch = (int32_t)ARG(1);
    if (maxch <= 0 || maxch > 4096) return FMOD_ERR_INVALID_PARAM;
    pthread_mutex_lock(&mx);
    nchannels = maxch < MAXCH ? maxch : MAXCH;
    for (int i = 0; i < MAXCH; i++) { chans[i].sound = -1; chans[i].dsp = -1; }
    pthread_mutex_unlock(&mx);
    if (!sys_init && !audio_start()) return FMOD_ERR_OUTPUT_INIT;
    sys_init = 1;
    T("System_Init(%d channels, flags 0x%x)", maxch, ARG(2));
    return FMOD_OK;
}
SHIM(fmodex_FMOD_System_Release, 1) {
    SYS_OK();
    audio_stop();
    pthread_mutex_lock(&mx);
    for (int i = 0; i < MAXCH; i++) chan_stop_locked(&chans[i]);
    for (int i = 0; i < MAXSND; i++) if (snds[i].used) { free(snds[i].pcm); free(snds[i].data); memset(&snds[i], 0, sizeof snds[i]); }
    memset(dsps, 0, sizeof dsps);
    pthread_mutex_unlock(&mx);
    sys_created = sys_init = 0;
    return FMOD_OK;
}
SHIM(fmodex_FMOD_System_Set3DSettings, 4) {
    SYS_OK();
    pthread_mutex_lock(&mx); doppler = ARGF(1); distfactor = ARGF(2) > 0 ? ARGF(2) : 1; rolloff = ARGF(3) >= 0 ? ARGF(3) : 1; pthread_mutex_unlock(&mx);
    return FMOD_OK;
}
static void rdvec(uint32_t a, float *v) { if (a) { v[0] = rdf(a); v[1] = rdf(a + 4); v[2] = rdf(a + 8); } }
SHIM(fmodex_FMOD_System_Set3DListenerAttributes, 6) {
    SYS_OK();
    if (ARG(1) != 0) return FMOD_ERR_INVALID_PARAM;   // one listener
    pthread_mutex_lock(&mx); rdvec(ARG(2), l_pos); rdvec(ARG(3), l_vel); rdvec(ARG(4), l_fwd); rdvec(ARG(5), l_up); pthread_mutex_unlock(&mx);
    return FMOD_OK;
}
SHIM(fmodex_FMOD_System_Update, 1) { SYS_OK(); audio_report(); return FMOD_OK; }   // 3D state is applied by the mixer as it changes

SHIM(fmodex_FMOD_System_CreateSound, 5) {
    SYS_OK();
    uint32_t src = ARG(1), mode = ARG(2), ex = ARG(3), out = ARG(4);
    if (!out) return FMOD_ERR_INVALID_PARAM;
    wr32(out, 0);
    if (!(mode & M_OPENMEMORY) || !ex || rd32(ex) < 8) {
        rt_log("[fmod] CreateSound: mode 0x%x (only sounds from memory are implemented)", mode);
        return FMOD_ERR_INVALID_PARAM;
    }
    uint32_t len = rd32(ex + 4);
    if (!src || !len) return FMOD_ERR_INVALID_PARAM;
    uint8_t *data = malloc(len);
    if (!data) return FMOD_ERR_MEMORY;
    memcpy(data, G2H(src), len);
    Snd s = {0};
    s.used = 1; s.mode = mode;
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(data, (int)len, &err, NULL);
    uint32_t wbits = 0, woff = 0, wbytes = 0;
    if (v) {
        stb_vorbis_info vi = stb_vorbis_get_info(v);
        s.is_ogg = 1; s.nch = vi.channels; s.rate = (float)vi.sample_rate; s.frames = stb_vorbis_stream_length_in_samples(v);
        if (s.nch < 1 || s.nch > 2) { stb_vorbis_close(v); free(data); rt_log("[fmod] CreateSound: %d-channel Ogg is not supported", s.nch); return FMOD_ERR_FORMAT; }
        if (mode & M_CREATESTREAM) { s.data = data; s.len = len; data = NULL; }
        else {
            s.pcm = malloc((size_t)s.frames * (size_t)s.nch * sizeof(int16_t) + 4);
            if (!s.pcm) { stb_vorbis_close(v); free(data); return FMOD_ERR_MEMORY; }
            uint32_t done = 0;
            while (done < s.frames) {
                int got = stb_vorbis_get_samples_short_interleaved(v, s.nch, s.pcm + (size_t)done * (size_t)s.nch, (int)((s.frames - done) * (uint32_t)s.nch));
                if (got <= 0) break;
                done += (uint32_t)got;
            }
            s.frames = done;
        }
        stb_vorbis_close(v);
    } else if (wav_parse(data, len, &s.nch, &s.rate, &wbits, &woff, &wbytes)) {
        // WAVE PCM: always decoded to 16-bit PCM in memory (it is uncompressed already)
        uint32_t fb = wbits / 8 * (uint32_t)s.nch;
        s.frames = wbytes / fb;
        s.pcm = malloc((size_t)s.frames * (size_t)s.nch * sizeof(int16_t) + 4);
        if (!s.pcm) { free(data); return FMOD_ERR_MEMORY; }
        for (uint32_t f = 0; f < s.frames * (uint32_t)s.nch; f++) s.pcm[f] = wav_sample(data + woff + f * (wbits / 8), wbits);
    } else {
        rt_log("[fmod] CreateSound: unrecognized audio data (%u bytes, starts %02x %02x %02x %02x)", len, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
        free(data);
        return FMOD_ERR_FORMAT;
    }
    free(data);
    pthread_mutex_lock(&mx);
    int i = 0; while (i < MAXSND && snds[i].used) i++;
    if (i == MAXSND) { pthread_mutex_unlock(&mx); free(s.pcm); free(s.data); rt_log("[fmod] more than %d sounds", MAXSND); return FMOD_ERR_MEMORY; }
    snds[i] = s;
    pthread_mutex_unlock(&mx);
    T("CreateSound #%d: %s %s, %u frames, %d ch, %.0f Hz", i, s.is_ogg ? "ogg" : "wav", (mode & M_CREATESTREAM) ? "stream" : "sample", s.frames, s.nch, s.rate);
    wr32(out, snd_handle(i));
    return FMOD_OK;
}

// FMOD_CHANNELINDEX: FMOD_CHANNEL_FREE (-1) picks a channel, FMOD_CHANNEL_REUSE (-2) reuses *channel's
SHIM(fmodex_FMOD_System_PlaySound, 5) {
    SYS_OK();
    int32_t index = (int32_t)ARG(1); uint32_t out = ARG(4);
    pthread_mutex_lock(&mx);
    int s = snd_index(ARG(2));
    if (s < 0) { pthread_mutex_unlock(&mx); return FMOD_ERR_INVALID_HANDLE; }
    int i = -1;
    if (index == -2 && out) { i = ch_index(rd32(out)); if (i >= 0) chan_stop_locked(&chans[i]); }
    else if (index >= 0) { if (index >= nchannels) { pthread_mutex_unlock(&mx); return FMOD_ERR_INVALID_PARAM; } i = index; chan_stop_locked(&chans[i]); }
    if (i < 0) for (int k = 0; k < nchannels; k++) if (!chans[k].used) { i = k; break; }
    if (i < 0) {   // steal the least important channel (FMOD priority 0 is the most important)
        int worst = -1;
        for (int k = 0; k < nchannels; k++) if (worst < 0 || chans[k].priority > chans[worst].priority) worst = k;
        if (worst >= 0 && chans[worst].priority >= 128) { i = worst; chan_stop_locked(&chans[i]); }
    }
    if (i < 0) { pthread_mutex_unlock(&mx); return FMOD_ERR_CHANNEL_ALLOC; }
    Chan *ch = &chans[i];
    uint32_t gen = ch->gen;
    memset(ch, 0, sizeof *ch);
    ch->gen = gen; ch->used = 1; ch->playing = 1; ch->paused = ARG(3) != 0; ch->sound = s; ch->dsp = -1;
    ch->priority = 128; ch->mode = snds[s].mode; ch->vol = 1; ch->freq = snds[s].rate; ch->mind = 1; ch->maxd = 10000;
    memcpy(ch->p, l_pos, sizeof ch->p);
    if (!snds[s].pcm) {
        int err = 0;
        ch->vb = stb_vorbis_open_memory(snds[s].data, (int)snds[s].len, &err, NULL);
        if (!ch->vb) { ch->used = 0; ch->gen++; pthread_mutex_unlock(&mx); rt_log("[fmod] PlaySound: stream #%d can't be decoded (%d)", s, err); return FMOD_ERR_FORMAT; }
    }
    snds[s].refs++;
    uint32_t h = ch_handle(i);
    pthread_mutex_unlock(&mx);
    T("PlaySound #%d on channel %d%s", s, i, ARG(3) ? " (paused)" : "");
    if (out) wr32(out, h);
    return FMOD_OK;
}
SHIM(fmodex_FMOD_System_CreateDSPByType, 3) {
    SYS_OK();
    if (!ARG(2)) return FMOD_ERR_INVALID_PARAM;
    if (ARG(1) != DSP_LOWPASS) { rt_log("[fmod] CreateDSPByType(%u): only the lowpass DSP is implemented", ARG(1)); wr32(ARG(2), 0); return FMOD_ERR_INVALID_PARAM; }
    pthread_mutex_lock(&mx);
    int i = 0; while (i < MAXDSP && dsps[i].used) i++;
    if (i < MAXDSP) dsps[i] = (Dsp){1, -1, DSP_LOWPASS, 5000.0f, 1.0f};   // FMOD's lowpass defaults
    pthread_mutex_unlock(&mx);
    if (i == MAXDSP) return FMOD_ERR_MEMORY;
    wr32(ARG(2), dsp_handle(i));
    return FMOD_OK;
}

// ---------------- Sound ----------------
SHIM(fmodex_FMOD_Sound_Release, 1) {
    pthread_mutex_lock(&mx);
    int s = snd_index(ARG(0));
    if (s < 0) { pthread_mutex_unlock(&mx); return FMOD_ERR_INVALID_HANDLE; }
    // channels playing it stop, as FMOD does
    for (int i = 0; i < MAXCH; i++) if (chans[i].used && chans[i].sound == s) chan_stop_locked(&chans[i]);
    free(snds[s].pcm); free(snds[s].data); memset(&snds[s], 0, sizeof snds[s]);
    pthread_mutex_unlock(&mx);
    return FMOD_OK;
}
SHIM(fmodex_FMOD_Sound_GetLength, 3) {
    pthread_mutex_lock(&mx);
    int s = snd_index(ARG(0));
    if (s < 0 || !ARG(1)) { pthread_mutex_unlock(&mx); return s < 0 ? FMOD_ERR_INVALID_HANDLE : FMOD_ERR_INVALID_PARAM; }
    Snd *x = &snds[s]; uint32_t tu = ARG(2), v;
    if (tu == TU_MS) v = (uint32_t)((double)x->frames * 1000.0 / x->rate);
    else if (tu == TU_PCM) v = x->frames;
    else if (tu == TU_PCMBYTES) v = x->frames * 2u * (uint32_t)x->nch;
    else { pthread_mutex_unlock(&mx); return FMOD_ERR_FORMAT; }
    pthread_mutex_unlock(&mx);
    wr32(ARG(1), v);
    return FMOD_OK;
}

// ---------------- Channel ----------------
#define CHAN_BEGIN(var) pthread_mutex_lock(&mx); int var = ch_index(ARG(0)); \
    if (var < 0) { pthread_mutex_unlock(&mx); return FMOD_ERR_INVALID_HANDLE; }
#define CHAN_END() pthread_mutex_unlock(&mx); return FMOD_OK
SHIM(fmodex_FMOD_Channel_SetPriority, 2) {
    CHAN_BEGIN(i);
    int32_t p = (int32_t)ARG(1); chans[i].priority = p < 0 ? 0 : p > 256 ? 256 : p;
    CHAN_END();
}
SHIM(fmodex_FMOD_Channel_SetPaused, 2) { CHAN_BEGIN(i); chans[i].paused = ARG(1) != 0; CHAN_END(); }
SHIM(fmodex_FMOD_Channel_SetVolume, 2) { CHAN_BEGIN(i); float v = ARGF(1); chans[i].vol = v < 0 ? 0 : v > 1 ? 1 : v; CHAN_END(); }
SHIM(fmodex_FMOD_Channel_SetFrequency, 2) { CHAN_BEGIN(i); float f = ARGF(1); if (f > 0) chans[i].freq = f; CHAN_END(); }
SHIM(fmodex_FMOD_Channel_SetMode, 2) {
    CHAN_BEGIN(i);
    uint32_t m = ARG(1), cur = chans[i].mode;
    if (m & (M_LOOP_OFF | M_LOOP_NORMAL | M_LOOP_BIDI)) cur = (cur & ~(uint32_t)(M_LOOP_OFF | M_LOOP_NORMAL | M_LOOP_BIDI)) | (m & (M_LOOP_OFF | M_LOOP_NORMAL | M_LOOP_BIDI));
    if (m & (M_2D | M_3D)) cur = (cur & ~(uint32_t)(M_2D | M_3D)) | (m & (M_2D | M_3D));
    if (cur & M_LOOP_BIDI) cur = (cur & ~(uint32_t)M_LOOP_BIDI) | M_LOOP_NORMAL;   // ping-pong plays as a forward loop
    chans[i].mode = cur;
    CHAN_END();
}
SHIM(fmodex_FMOD_Channel_Set3DAttributes, 3) { CHAN_BEGIN(i); rdvec(ARG(1), chans[i].p); rdvec(ARG(2), chans[i].v); CHAN_END(); }
SHIM(fmodex_FMOD_Channel_Set3DMinMaxDistance, 3) {
    float mn = ARGF(1), mxd = ARGF(2);
    if (!(mn > 0) || !(mxd >= mn)) return FMOD_ERR_INVALID_PARAM;
    CHAN_BEGIN(i); chans[i].mind = mn; chans[i].maxd = mxd; CHAN_END();
}
SHIM(fmodex_FMOD_Channel_Stop, 1) { CHAN_BEGIN(i); chan_stop_locked(&chans[i]); CHAN_END(); }
SHIM(fmodex_FMOD_Channel_IsPlaying, 2) {
    pthread_mutex_lock(&mx);
    int i = ch_index(ARG(0));
    if (ARG(1)) wr32(ARG(1), i >= 0 && chans[i].playing);
    pthread_mutex_unlock(&mx);
    return i >= 0 ? FMOD_OK : FMOD_ERR_INVALID_HANDLE;
}
SHIM(fmodex_FMOD_Channel_AddDSP, 2) {
    CHAN_BEGIN(i);
    int d = dsp_index(ARG(1));
    if (d < 0) { pthread_mutex_unlock(&mx); return FMOD_ERR_INVALID_HANDLE; }
    if (dsps[d].chan >= 0 && dsps[d].chan != i) chans[dsps[d].chan].dsp = -1;   // a DSP sits in one place
    if (chans[i].dsp >= 0 && chans[i].dsp != d) dsps[chans[i].dsp].chan = -1;
    dsps[d].chan = i; chans[i].dsp = d;
    memset(chans[i].z, 0, sizeof chans[i].z);
    CHAN_END();
}

// ---------------- DSP ----------------
SHIM(fmodex_FMOD_DSP_GetNumParameters, 2) {
    pthread_mutex_lock(&mx); int d = dsp_index(ARG(0)); pthread_mutex_unlock(&mx);
    if (d < 0) return FMOD_ERR_INVALID_HANDLE;
    if (ARG(1)) wr32(ARG(1), 2);   // lowpass: cutoff, resonance
    return FMOD_OK;
}
SHIM(fmodex_FMOD_DSP_SetParameter, 3) {
    pthread_mutex_lock(&mx);
    int d = dsp_index(ARG(0));
    uint32_t r = FMOD_OK;
    float v = ARGF(2);
    if (d < 0) r = FMOD_ERR_INVALID_HANDLE;
    else if (ARG(1) == 0) dsps[d].cutoff = v < 10 ? 10 : v > 22000 ? 22000 : v;          // FMOD_DSP_LOWPASS_CUTOFF
    else if (ARG(1) == 1) dsps[d].resonance = v < 1 ? 1 : v > 10 ? 10 : v;              // FMOD_DSP_LOWPASS_RESONANCE
    else r = FMOD_ERR_INVALID_PARAM;
    pthread_mutex_unlock(&mx);
    return r;
}
SHIM(fmodex_FMOD_DSP_Remove, 1) {
    pthread_mutex_lock(&mx);
    int d = dsp_index(ARG(0));
    if (d >= 0 && dsps[d].chan >= 0) { chans[dsps[d].chan].dsp = -1; dsps[d].chan = -1; }
    pthread_mutex_unlock(&mx);
    return d >= 0 ? FMOD_OK : FMOD_ERR_INVALID_HANDLE;
}
SHIM(fmodex_FMOD_DSP_Release, 1) {
    pthread_mutex_lock(&mx);
    int d = dsp_index(ARG(0));
    if (d >= 0) { if (dsps[d].chan >= 0) chans[dsps[d].chan].dsp = -1; memset(&dsps[d], 0, sizeof dsps[d]); }
    pthread_mutex_unlock(&mx);
    return d >= 0 ? FMOD_OK : FMOD_ERR_INVALID_HANDLE;
}
