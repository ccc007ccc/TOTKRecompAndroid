// Internal CPU DSP for the effects used by TOTK. Included by audio.c after Mix
// and the guest-memory helpers. Protocol and algorithms cross-checked against
// Ryujinx.Audio (MIT), Renderer/Parameter/Effect and Renderer/Dsp/{Command,State}:
// https://github.com/Sunshinin11/Ryujinx_Neo/tree/734fe55f8f7e2d2ae69449374476e360521e4d51/src/Ryujinx.Audio
// Copyright and permission notice: LICENSE-audio-effects.txt.
#pragma once
#include <stddef.h>

typedef struct {
    uint8_t input[24], output[24];
    uint32_t channels, sample_rate, capacity, channels_max;
    uint64_t send_info, send_storage, return_info, return_storage;
    uint32_t sample_size, total_samples, sample_count;
} AuxiliaryParam;

typedef struct {
    uint8_t input[6], output[6];
    uint16_t channels_max, channels;
    int32_t sample_rate, lookahead_max, attack_time, release_time, lookahead;
    float attack, release, threshold, input_gain, output_gain;
    int32_t delay_min, delay_max;
    uint8_t status, statistics, reset_statistics, reserved;
} LimiterParam;

typedef struct {
    LimiterParam p;
    float detector[6], compression[6], input_max[6], gain_min[6];
    float *delay;
    uint32_t position[6], capacity, channels;
} Limiter;

typedef struct {
    uint8_t input[6], output[6];
    uint16_t channels_max, channels;
    uint32_t reserved, sample_rate;
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room_gain, reflections_gain,
        reverb_gain, diffusion, reflection_delay, reverb_delay, density, dry_gain;
    uint8_t status;
} Reverb3dParam;

_Static_assert(offsetof(AuxiliaryParam, send_info) == 0x40, "auxiliary parameter ABI");
_Static_assert(offsetof(LimiterParam, status) == 0x40, "limiter parameter ABI");
_Static_assert(offsetof(Reverb3dParam, status) == 0x48, "3D reverb parameter ABI");

static bool effect_channels(unsigned n) { return n == 1 || n == 2 || n == 4 || n == 6; }
static bool effect_buffers(Mix *m, const uint8_t *in, const uint8_t *out, unsigned channels) {
    if (!m->buf || !channels || channels > MIXBUF_MAX) return false;
    for (unsigned c = 0; c < channels; c++) if (in[c] >= m->bufs || out[c] >= m->bufs) return false;
    return true;
}

static int32_t audio_sample_int(float x) {
    if (!isfinite(x)) return 0;
    if (x >= 0x1p31f) return INT32_MAX;
    if (x <= -0x1p31f) return INT32_MIN;
    return (int32_t)x;
}

// Auxiliary buffers are planar int32 samples, not PCM16 or float bit patterns.
// The CPU owns the first 0x40-byte header; the DSP owns the second. Publish the
// cursor only after all channels, so a guest effect never sees a partial frame.
static void auxiliary_process(const AuxiliaryParam *p, Mix *m, uint32_t n, bool enabled) {
    if (!effect_buffers(m, p->input, p->output, p->channels)) return;
    uint64_t bytes = 0x80ull + (uint64_t)p->capacity * 4;
    bool valid = p->capacity && !(p->send_info & 3) && !(p->return_info & 3) &&
                 in_guest(p->send_info, bytes) && in_guest(p->return_info, bytes);
    if (!enabled) {
        if (valid) {
            uint64_t info[] = {p->send_info + 0x40, p->return_info + 0x40};
            for (unsigned i = 0; i < 2; i++) {
                plat_write_prepare(info[i], 16);
                uint32_t *h = (uint32_t *)(uintptr_t)info[i];
                __atomic_store_n(h, 0, __ATOMIC_RELEASE);
                __atomic_store_n(h + 1, 0, __ATOMIC_RELEASE);
                __atomic_store_n(h + 3, 0, __ATOMIC_RELEASE); // retain lost-sample count
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            float inputs[MIXBUF_MAX];
            for (unsigned c = 0; c < p->channels; c++) inputs[c] = m->buf[p->input[c] * n + i];
            for (unsigned c = 0; c < p->channels; c++) m->buf[p->output[c] * n + i] = inputs[c];
        }
        return;
    }
    if (!valid) FATAL("invalid audio auxiliary ring buffers\n");
    uint32_t *send = (uint32_t *)(uintptr_t)(p->send_info + 0x40);
    uint32_t *receive = (uint32_t *)(uintptr_t)(p->return_info + 0x40);
    uint32_t write = __atomic_load_n(send + 1, __ATOMIC_ACQUIRE) % p->capacity;
    uint32_t read = __atomic_load_n(receive, __ATOMIC_ACQUIRE) % p->capacity;
    int32_t *sent = (int32_t *)(uintptr_t)(p->send_info + 0x80);
    const int32_t *returned = (const int32_t *)(uintptr_t)(p->return_info + 0x80);
    plat_write_prepare(p->send_info + 0x40, bytes - 0x40);
    plat_write_prepare(p->return_info + 0x40, 4);
    // Send all inputs before replacing any output; supports permuted/in-place channels.
    for (unsigned c = 0; c < p->channels; c++) for (uint32_t i = 0; i < n; i++) {
        sent[write] = audio_sample_int(m->buf[p->input[c] * n + i]);
        if (++write == p->capacity) write = 0;
    }
    for (unsigned c = 0; c < p->channels; c++) for (uint32_t i = 0; i < n; i++) {
        m->buf[p->output[c] * n + i] = (float)returned[read];
        if (++read == p->capacity) read = 0;
    }
    __atomic_store_n(send + 1, write, __ATOMIC_RELEASE);
    __atomic_store_n(receive, read, __ATOMIC_RELEASE);
}

static void limiter_update(Limiter *l, const uint8_t *data) {
    LimiterParam p; memcpy(&p, data, sizeof p);
    if (!effect_channels(p.channels) || p.delay_min < 1 || p.delay_max < p.delay_min ||
        p.delay_max > 192000 || !isfinite(p.threshold) || p.threshold <= 0 ||
        !isfinite(p.attack) || !isfinite(p.release) || p.attack < 0 || p.attack > 1 || p.release < 0 || p.release > 1)
        FATAL("invalid audio limiter parameters\n");
    if (!l->delay || l->capacity != (uint32_t)p.delay_max || l->channels != p.channels || p.status == 0) {
        free(l->delay);
        l->delay = calloc((size_t)p.delay_max * p.channels, sizeof(float));
        if (!l->delay) FATAL("audio limiter allocation failed\n");
        memset(l->position, 0, sizeof l->position);
        memset(l->detector, 0, sizeof l->detector);
        for (unsigned c = 0; c < 6; c++) l->compression[c] = 1;
        l->capacity = p.delay_max; l->channels = p.channels;
    }
    if (p.reset_statistics || !l->p.channels) {
        memset(l->input_max, 0, sizeof l->input_max);
        for (unsigned c = 0; c < 6; c++) l->gain_min[c] = 1;
    }
    l->p = p;
}

static void limiter_process(Limiter *l, Mix *m, uint32_t n, bool enabled) {
    LimiterParam *p = &l->p;
    if (!effect_buffers(m, p->input, p->output, p->channels)) return;
    for (uint32_t i = 0; i < n; i++) {
        float inputs[6];
        for (unsigned c = 0; c < p->channels; c++) inputs[c] = m->buf[p->input[c] * n + i];
        for (unsigned c = 0; c < p->channels; c++) {
            float x = inputs[c], y = x;
            if (enabled && l->delay) {
                x = x / 32767.f * p->input_gain;
                float peak = fabsf(x), alpha = peak > l->detector[c] ? p->attack : p->release;
                l->detector[c] += alpha * (peak - l->detector[c]);
                float attenuation = l->detector[c] > p->threshold ? p->threshold / l->detector[c] : 1.f;
                alpha = attenuation < l->compression[c] ? p->attack : p->release;
                l->compression[c] += alpha * (attenuation - l->compression[c]);
                l->position[c] %= (uint32_t)p->delay_min;
                float *delayed = l->delay + c * l->capacity + l->position[c];
                y = *delayed * l->compression[c] * p->output_gain * 32767.f;
                *delayed = x;
                l->position[c] = (l->position[c] + 1) % (uint32_t)p->delay_min;
                l->input_max[c] = fmaxf(l->input_max[c], peak);
                l->gain_min[c] = fminf(l->gain_min[c], l->compression[c]);
            }
            m->buf[p->output[c] * n + i] = y;
        }
    }
}

typedef struct {
    float *data;
    uint32_t capacity, read, write, delay;
} Reverb3dDelay;

typedef struct {
    Reverb3dParam p;
    bool ready;
    Reverb3dDelay fdn[4], allpass[2][4], pre, center;
    uint32_t early_delay[20], reflection_delay;
    float early_gain, late_gain, pre_previous_gain, pre_target_gain, pre_previous, diffusion;
    float direct[4], current[4], output[4], feedback[4];
} Reverb3d;

static void reverb3d_free(Reverb3d *r) {
    for (unsigned i = 0; i < 4; i++) {
        free(r->fdn[i].data);
        for (unsigned j = 0; j < 2; j++) free(r->allpass[j][i].data);
    }
    free(r->pre.data); free(r->center.data);
    memset(r, 0, sizeof *r);
}

static void r3_delay_init(Reverb3dDelay *d, uint32_t count) {
    d->capacity = count + 1;
    d->data = calloc(d->capacity, sizeof(float));
    if (!d->data) FATAL("audio reverb allocation failed\n");
    d->read = 0; d->write = d->delay = count;
}
static void r3_delay_set(Reverb3dDelay *d, uint32_t count) {
    d->delay = count < d->capacity ? count : d->capacity - 1;
    d->write = (d->read + d->delay) % d->capacity;
}
static float r3_delay_push(Reverb3dDelay *d, float x) {
    d->data[d->write] = x;
    float y = d->data[d->read];
    if (++d->write == d->capacity) d->write = 0;
    if (++d->read == d->capacity) d->read = 0;
    return y;
}
static float r3_delay_tap(Reverb3dDelay *d, uint32_t delay) {
    if (delay >= d->capacity) delay = d->capacity - 1;
    return d->data[(d->write + d->capacity - delay - 1) % d->capacity];
}
static float r3_allpass(Reverb3dDelay *d, float x, float rate) {
    float v = x - rate * d->data[d->read];
    return r3_delay_push(d, v) + rate * v;
}
static float r3_pow10(float x) { return x >= 0 ? 1 : x <= -5.3f ? 0 : powf(10, x); }
static float r3_radians(float degrees) { return degrees * (float)(M_PI / 180); }

static void reverb3d_update(Reverb3d *r, const uint8_t *data) {
    static const float minimum[4] = {5, 6, 13, 14}, maximum[4] = {45.704f, 82.782f, 149.94f, 271.58f};
    static const float allpass[2][4] = {{17, 13, 9, 7}, {19, 11, 10, 6}};
    static const float early[20] = {.017136f,.059154f,.16173f,.39019f,.42526f,.45541f,.68974f,.74591f,.83384f,.8595f,
        0,.075024f,.16879f,.2999f,.33744f,.3719f,.59901f,.71674f,.81786f,.85166f};
    Reverb3dParam p; memcpy(&p, data, sizeof p);
    if (!effect_channels(p.channels) || p.sample_rate < 8000 || p.sample_rate > 192000 ||
        !(p.decay_time > 0) || !(p.hf_decay_ratio > 0) || !(p.hf_reference > 0))
        FATAL("invalid audio 3D reverb parameters\n");
    if (r->ready && !memcmp(&r->p, &p, offsetof(Reverb3dParam, status)) && p.status != 0) return;
    if (r->ready && (r->p.sample_rate != p.sample_rate || p.status == 0)) reverb3d_free(r);
    float sr = p.sample_rate / 1000.f;
    if (!r->ready) {
        for (unsigned i = 0; i < 4; i++) {
            r3_delay_init(&r->fdn[i], (uint32_t)lrintf(sr * maximum[i]));
            for (unsigned j = 0; j < 2; j++) r3_delay_init(&r->allpass[j][i], (uint32_t)lrintf(sr * allpass[j][i]));
        }
        r3_delay_init(&r->pre, (uint32_t)lrintf(sr * 400));
        r3_delay_init(&r->center, (uint32_t)lrintf(sr * 5));
    }
    r->p = p; r->ready = true;
    memset(r->feedback, 0, sizeof r->feedback); r->pre_previous = 0;
    r->early_gain = r3_pow10(fminf(p.room_gain + p.reflections_gain, 5000) / 2000);
    r->late_gain = r3_pow10(fminf(p.room_gain + p.reverb_gain, 5000) / 2000);
    float high = r3_pow10(p.room_hf / 2000);
    r->pre_previous_gain = 0;
    if (high < 1) {
        float a = 1 - high, b = 2 - 2 * high * cosf(r3_radians(256 * p.hf_reference / p.sample_rate));
        r->pre_previous_gain = (b - sqrtf(fmaxf(0, b * b - 4 * a * a))) / (2 * a);
    }
    r->pre_target_gain = 1 - r->pre_previous_gain;
    r->reflection_delay = (uint32_t)lrintf(sr * 1000 * clampf(p.reflection_delay + p.reverb_delay, 0, .4f));
    r->diffusion = .006f * clampf(p.diffusion, 0, 100);
    float hf_angle = r3_radians(64 * p.hf_reference / p.sample_rate);
    float cot = cosf(hf_angle) / sinf(hf_angle);
    for (unsigned i = 0; i < 4; i++) {
        r3_delay_set(&r->fdn[i], (uint32_t)lrintf(sr * (minimum[i] + clampf(p.density, 0, 100) * .01f * (maximum[i] - minimum[i]))));
        uint32_t length = r->fdn[i].delay + r->allpass[0][i].delay + r->allpass[1][i].delay;
        float a = -60.f * length / (p.decay_time * p.sample_rate), b = a / p.hf_decay_ratio;
        float d = r3_pow10((b - a) / 40), e = .7071f * r3_pow10((b + a) / 40);
        r->direct[i] = e * (d * cot + 1) / (cot + d);
        r->current[i] = e * (1 - d * cot) / (cot + d);
        r->output[i] = (cot - d) / (cot + d);
    }
    for (unsigned i = 0; i < 20; i++) {
        float seconds = p.reflection_delay + early[i] * (p.reverb_delay * .9998f + .02f);
        r->early_delay[i] = (uint32_t)lrintf(sr * 1000 * clampf(seconds, 0, .4f));
    }
}

static void reverb3d_process(Reverb3d *r, Mix *m, uint32_t n, bool enabled, unsigned revision) {
    static const float gain[20] = {.67096f,.61027f,1,.35680f,.68361f,.65978f,.51939f,.24712f,.45945f,.45021f,
        .64196f,.54879f,.92925f,.38270f,.72867f,.69794f,.5464f,.24563f,.45214f,.44042f};
    static const uint8_t stereo[20] = {0,0,0,1,1,1,1,0,0,0,1,1,1,0,0,0,0,1,1,1};
    static const uint8_t quad[20] = {0,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3};
    static const uint8_t surround[20] = {4,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3};
    uint32_t ch = r->p.channels;
    if (!r->ready || !effect_buffers(m, r->p.input, r->p.output, ch)) return;
    uint8_t in[6], out[6]; memcpy(in, r->p.input, 6); memcpy(out, r->p.output, 6);
    if (ch == 6 && revision >= 11) {
        static const uint8_t legacy[6] = {0,1,4,5,2,3}; // FL FR BL BR FC LFE
        for (unsigned c = 0; c < ch; c++) { in[c] = r->p.input[legacy[c]]; out[c] = r->p.output[legacy[c]]; }
    }
    for (uint32_t i = 0; i < n; i++) {
        float input[6], output[6] = {0}, feedback[4], values[4], sum = 0;
        for (unsigned c = 0; c < ch; c++) { input[c] = m->buf[in[c] * n + i]; sum += input[c]; }
        if (!enabled) {
            for (unsigned c = 0; c < ch; c++) m->buf[out[c] * n + i] = input[c];
            continue;
        }
        float late = r3_delay_tap(&r->pre, r->reflection_delay) * r->late_gain;
        for (unsigned e = 0; e < 20; e++) {
            unsigned channel = ch == 1 ? 0 : ch == 2 ? stereo[e] : ch == 4 ? quad[e] : surround[e];
            float tap = r3_delay_tap(&r->pre, r->early_delay[e]) * gain[e] * r->early_gain;
            output[channel] += tap;
            if (ch == 6) output[5] += tap;
        }
        r->pre_previous = sum * r->pre_target_gain + r->pre_previous * r->pre_previous_gain;
        r3_delay_push(&r->pre, r->pre_previous);
        for (unsigned j = 0; j < 4; j++) {
            float v = r->fdn[j].data[r->fdn[j].read];
            feedback[j] = v * r->direct[j] + r->feedback[j];
            r->feedback[j] = v * r->current[j] + feedback[j] * r->output[j];
        }
        float feed[4] = {feedback[2] + feedback[1], -feedback[0] - feedback[3],
                         feedback[0] - feedback[3], feedback[1] - feedback[2]};
        for (unsigned j = 0; j < 4; j++) {
            float v = r3_allpass(&r->allpass[0][j], late + feed[j], r->diffusion);
            values[j] = r3_allpass(&r->allpass[1][j], v, r->diffusion * -.9f);
            r3_delay_push(&r->fdn[j], values[j]);
        }
        for (unsigned c = 0; c < ch; c++) {
            float v = ch == 6 && c == 4 ? r3_delay_push(&r->center, (values[2] - values[3]) * .5f) : values[c % 4];
            if (ch == 1) v += values[1];
            if (ch == 6 && c == 5) v = values[3];
            m->buf[out[c] * n + i] = output[c] + v + input[c] * r->p.dry_gain;
        }
    }
}
