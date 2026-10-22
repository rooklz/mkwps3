/* ax_spu_core.h -- the AX voice mix in plain C: ProcessPBList and ProcessVoice from
 * ax_mix.cpp with everything they call (the accelerator, the resamplers, the volume
 * ramps and filters from ax_internal.h and ax_mix_kernels.h). The SPU mixer
 * (runtime/spu/ax_spu.c) runs it, and tests/audio/ax_spu_test.cpp holds it to the
 * C++ mixer bit for bit, so the arithmetic follows the C++ step for step, in the
 * same order and the same integer widths. Change one, change the other.
 *
 * The includer provides guest memory access before including this file:
 *
 *   static void axs_guest_get(void *dst, uint32_t addr, uint32_t bytes);
 *       PB halfwords into host order; zeros where the guest has no memory
 *   static void axs_guest_put(uint32_t addr, const void *src, uint32_t bytes);
 *   static const uint8_t *axs_aram_range(uint32_t addr, uint32_t bytes);
 *       `bytes` sample bytes in one piece, or NULL (then they are read one by one)
 *   static uint8_t axs_aram_byte(uint32_t addr);
 *       0 where nothing backs the address
 */
#ifndef AX_SPU_CORE_H
#define AX_SPU_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AXS_SAMPLES_PER_FRAME 96u
#define AXS_WM_SAMPLES_PER_FRAME 18u
#define AXS_REGULAR_BUSES 12u
#define AXS_WM_BUSES 8u
#define AXS_COEF_COUNT 0x800u

/* With axs_v_check set, each vector kernel call is also run through the scalar loop
 * and any difference counted in axs_v_mismatch. */
static int axs_v_check;
static uint32_t axs_v_mismatch;


/* AXPBWii, field for field. */
typedef struct { uint16_t volume, volume_delta; } AxsVolume;
typedef struct {
    AxsVolume main_left, main_right, auxA_left, auxA_right, auxB_left, auxB_right,
              auxC_left, auxC_right, main_surround, auxA_surround, auxB_surround, auxC_surround;
} AxsMixer;
typedef struct { uint16_t on, addrMemHigh, addrMemLow, offsetLeft, offsetRight, targetLeft, targetRight; } AxsItd;
typedef struct {
    int16_t main_left, auxA_left, auxB_left, auxC_left, main_right, auxA_right, auxB_right, auxC_right,
            main_surround, auxA_surround, auxB_surround, auxC_surround;
} AxsDpop;
typedef struct { uint16_t num_updates[3], data_hi, data_lo; } AxsUpdates;
typedef struct { int16_t cur_volume, cur_volume_delta; } AxsVolEnv;
typedef struct {
    uint16_t looping, sample_format, loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo,
             cur_addr_hi, cur_addr_lo;
} AxsAudioAddr;
typedef struct { int16_t coefs[16]; uint16_t gain, pred_scale; int16_t yn1, yn2; } AxsAdpcm;
typedef struct { uint16_t ratio_hi, ratio_lo, cur_addr_frac; int16_t last_samples[4]; } AxsSrc;
typedef struct { uint16_t cur_addr_frac; int16_t last_samples[4]; } AxsSrcWM;
typedef struct { uint16_t pred_scale, yn1, yn2; } AxsAdpcmLoop;
typedef struct { uint16_t on; int16_t yn1; uint16_t a0; int16_t b0; } AxsLpf;
typedef struct { uint16_t on, unk[3]; } AxsHpf;
typedef struct { uint16_t on; int16_t xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2; } AxsBiquad;
typedef struct { AxsVolume main0, aux0, main1, aux1, main2, aux2, main3, aux3; } AxsMixerWM;
typedef struct { int16_t main0, main1, main2, main3, aux0, aux1, aux2, aux3; } AxsDpopWM;
typedef union { uint16_t on; AxsLpf lpf; AxsBiquad biquad; } AxsIirWM;

typedef struct {
    uint16_t next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo, src_type, coef_select,
             mixer_control_hi, mixer_control_lo, running, is_stream;
    AxsMixer mixer;
    AxsItd initial_time_delay;
    AxsDpop dpop;
    AxsUpdates updates;
    AxsVolEnv vol_env;
    AxsAudioAddr audio_addr;
    AxsAdpcm adpcm;
    AxsSrc src;
    AxsAdpcmLoop adpcm_loop_info;
    AxsLpf lpf;
    union { AxsHpf hpf; AxsBiquad biquad; } filt;
    uint16_t remote, remote_mixer_control;
    AxsMixerWM remote_mixer;
    AxsDpopWM remote_dpop;
    AxsSrcWM remote_src;
    AxsIirWM remote_iir;
    uint16_t pad[2];
} AxsPB;

/* The guest PB as up to three runs of halfwords (ax_mix.cpp PBSegmentsForLayout). */
typedef struct { uint16_t guest, host, bytes; } AxsSegment;

/* AXMixControl */
enum {
    AXS_MAIN_L = 0x000001, AXS_MAIN_L_RAMP = 0x000002, AXS_MAIN_R = 0x000004, AXS_MAIN_R_RAMP = 0x000008,
    AXS_MAIN_S = 0x000010, AXS_MAIN_S_RAMP = 0x000020, AXS_AUXA_L = 0x000040, AXS_AUXA_L_RAMP = 0x000080,
    AXS_AUXA_R = 0x000100, AXS_AUXA_R_RAMP = 0x000200, AXS_AUXA_S = 0x000400, AXS_AUXA_S_RAMP = 0x000800,
    AXS_AUXB_L = 0x001000, AXS_AUXB_L_RAMP = 0x002000, AXS_AUXB_R = 0x004000, AXS_AUXB_R_RAMP = 0x008000,
    AXS_AUXB_S = 0x010000, AXS_AUXB_S_RAMP = 0x020000, AXS_AUXC_L = 0x040000, AXS_AUXC_L_RAMP = 0x080000,
    AXS_AUXC_R = 0x100000, AXS_AUXC_R_RAMP = 0x200000, AXS_AUXC_S = 0x400000, AXS_AUXC_S_RAMP = 0x800000
};

/* Bus order (AxBus): main L R S, aux A L R S, B, C, then the wiimote main/aux pairs. */
enum { AXS_BUS_COUNT = 20 };

static inline uint32_t axs_hilo(uint16_t hi, uint16_t lo) { return ((uint32_t)hi << 16) | lo; }

static inline int16_t axs_clamp_s16(int64_t v)
{
    return (int16_t)(v < -0x8000 ? -0x8000 : (v > 0x7fff ? 0x7fff : v));
}

static inline int16_t axs_clamp_adpcm(int64_t v)
{
    return (int16_t)(v < -0x7fff ? -0x7fff : (v > 0x7fff ? 0x7fff : v));
}

static inline int16_t axs_clamp32_s16(int32_t v)
{
    return (int16_t)(v < -0x8000 ? -0x8000 : (v > 0x7fff ? 0x7fff : v));
}

/* ---- the accelerator (Accelerator in ax_internal.h) ---------------------------- */

typedef struct {
    AxsPB *pb;
    uint32_t start, end, current;
    uint16_t format;
    int16_t gain, yn1, yn2;
    uint16_t pred_scale;
    int32_t coef1, coef2;
    int stopped;
} AxsAccel;

static inline void axs_accel_coefficients(AxsAccel *a)
{
    const int idx = (a->pred_scale >> 4) & 0x7;
    a->coef1 = a->pb->adpcm.coefs[idx * 2 + 0];
    a->coef2 = a->pb->adpcm.coefs[idx * 2 + 1];
}

static inline void axs_accel_setup(AxsAccel *a, AxsPB *pb)
{
    a->pb = pb;
    a->start = axs_hilo(pb->audio_addr.loop_addr_hi, pb->audio_addr.loop_addr_lo) & 0x3fffffffu;
    a->end = axs_hilo(pb->audio_addr.end_addr_hi, pb->audio_addr.end_addr_lo) & 0x3fffffffu;
    a->current = axs_hilo(pb->audio_addr.cur_addr_hi, pb->audio_addr.cur_addr_lo) & 0xbfffffffu;
    a->format = pb->audio_addr.sample_format;
    a->gain = (int16_t)pb->adpcm.gain;
    a->pred_scale = pb->adpcm.pred_scale & 0x7f;
    a->yn1 = pb->adpcm.yn1;
    a->yn2 = pb->adpcm.yn2;
    a->stopped = 0;
    axs_accel_coefficients(a);
}

static inline void axs_accel_commit(AxsAccel *a)
{
    a->pb->audio_addr.cur_addr_hi = (uint16_t)(a->current >> 16);
    a->pb->audio_addr.cur_addr_lo = (uint16_t)a->current;
    a->pb->adpcm.yn1 = a->yn1;
    a->pb->adpcm.yn2 = a->yn2;
    a->pb->adpcm.pred_scale = a->pred_scale;
}

/* Accelerator::ReadSamples */
static void axs_read_samples(AxsAccel *a, int16_t *out, uint32_t n)
{
    AxsPB *const pb = a->pb;
    uint32_t current = a->current;
    const uint32_t start = a->start;
    const uint32_t end = a->end;
    const uint16_t size = a->format & 3u;
    const uint16_t decode = (a->format >> 2) & 3u;
    const int32_t gain = a->gain;
    const uint16_t gain_scale = (a->format >> 4) & 3u;
    const uint8_t gain_shift = gain_scale == 1 ? 0 : (gain_scale == 2 ? 16 : 11);
    int16_t yn1 = a->yn1;
    int16_t yn2 = a->yn2;
    uint16_t pred_scale = a->pred_scale;
    int32_t coef1 = a->coef1;
    int32_t coef2 = a->coef2;
    int stopped = a->stopped;
    const int adpcm4 = size == 0 && decode == 0;
    uint32_t i;

    for (i = 0; i < n; ++i) {
        uint16_t raw = 0;
        int16_t value;
        uint8_t step = 2;
        if (stopped) {
            for (; i < n; ++i) out[i] = 0;
            break;
        }
        if (adpcm4) {
            uint32_t k = 15 - (current & 15);
            const uint32_t room = end >= current + 2 ? end - 2 - current : 0;
            const uint8_t *src;
            if (room < k) k = room;
            if (n - i < k) k = n - i;
            src = k ? axs_aram_range(current >> 1, ((current + k - 1) >> 1) - (current >> 1) + 1) : 0;
            if (src) {
                const uint32_t shift = pred_scale & 0xf;
                const int16_t c1 = (int16_t)coef1, c2 = (int16_t)coef2;
                int32_t y1 = yn1, y2 = yn2;
                int16_t *dst = out + i;
                uint32_t left = k;
                /* Every factor is a 16-bit value, so 16-bit multiplies give the same sums. */
#define AXS_PUT(nib) do { int32_t v_ = (int32_t)((uint32_t)(nib) << shift) + \
                          ((0x400 + c1 * (int16_t)y1 + c2 * (int16_t)y2) >> 11); \
                          v_ = v_ < -0x7fff ? -0x7fff : (v_ > 0x7fff ? 0x7fff : v_); \
                          y2 = y1; y1 = v_; *dst++ = (int16_t)v_; } while (0)
                if (current & 1) {
                    AXS_PUT((int32_t)((*src++ & 0xfu) ^ 8u) - 8);
                    --left;
                }
                for (; left >= 2; left -= 2) {
                    const uint32_t b = *src++;
                    AXS_PUT((int32_t)((b >> 4) ^ 8u) - 8);
                    AXS_PUT((int32_t)((b & 0xfu) ^ 8u) - 8);
                }
                if (left)
                    AXS_PUT((int32_t)((*src >> 4) ^ 8u) - 8);
#undef AXS_PUT
                yn1 = (int16_t)y1;
                yn2 = (int16_t)y2;
                current += k;
                i += k;
                if (i == n)
                    break;
            }
        }
        if (size == 0) {
            const uint8_t byte = axs_aram_byte(current >> 1);
            raw = (current & 1) ? (byte & 0xf) : (byte >> 4);
        } else if (size == 1) {
            raw = axs_aram_byte(current);
        } else if (size == 2) {
            raw = (uint16_t)((axs_aram_byte(current * 2) << 8) | axs_aram_byte(current * 2 + 1));
        }

        if (decode == 0) {
            int32_t nibble, scale;
            raw &= 0xf;
            nibble = raw >= 8 ? (int32_t)raw - 16 : (int32_t)raw;
            scale = 1 << (pred_scale & 0xf);
            value = axs_clamp_adpcm(scale * nibble + ((0x400 + coef1 * yn1 + coef2 * yn2) >> 11));
            yn2 = yn1;
            yn1 = value;
            ++current;
            if ((end & 0xf) == 0 && current == end) {
                current = start + 1;
            } else if ((end & 0xf) == 1 && current == end - 1) {
                current = start;
            } else if ((current & 15) == 0) {
                int idx;
                pred_scale = axs_aram_byte((current & ~15u) >> 1) & 0x7f;
                idx = (pred_scale >> 4) & 0x7;
                coef1 = pb->adpcm.coefs[idx * 2 + 0];
                coef2 = pb->adpcm.coefs[idx * 2 + 1];
                current += 2;
                step += 2;
            }
        } else {
            const int16_t pcm = (int16_t)raw;
            value = (int16_t)(((gain * pcm) >> gain_shift) + ((coef1 * yn1) >> gain_shift) +
                              ((coef2 * yn2) >> gain_shift));
            yn2 = yn1;
            yn1 = value;
            if (decode != 1)
                ++current;
        }

        if (current == end + step - 1) {
            current = start;
            stopped = 1;
            if (pb->audio_addr.looping) {
                int idx;
                pred_scale = pb->adpcm_loop_info.pred_scale & 0x7f;
                idx = (pred_scale >> 4) & 0x7;
                coef1 = pb->adpcm.coefs[idx * 2 + 0];
                coef2 = pb->adpcm.coefs[idx * 2 + 1];
                if (pb->is_stream != 1) {
                    yn1 = (int16_t)pb->adpcm_loop_info.yn1;
                    yn2 = (int16_t)pb->adpcm_loop_info.yn2;
                }
                stopped = 0;
            } else {
                pb->running = 0;
            }
        }
        current &= 0xbfffffffu;
        out[i] = value;
    }

    a->current = current;
    a->yn1 = yn1;
    a->yn2 = yn2;
    a->pred_scale = pred_scale;
    a->coef1 = coef1;
    a->coef2 = coef2;
    a->stopped = stopped;
}

/* ---- resampling (ResampleLine, ResampleAudio) ----------------------------------- */

#if defined(__SPU__) || defined(AXS_VECTOR_STUB)
/* The 32-bit polyphase loop four taps per multiply pair: the taps and the phase's
 * coefficients come in as halfword vectors, mule/mulo give t0c0, t2c2 and t1c1, t3c3,
 * and the int32 sum is the scalar loop's (with fit32 it never leaves 32 bits). Eight
 * outputs are stored at a time, so `out` must be 16-aligned and count a multiple of 8. */
#include <spu_intrinsics.h>

static inline vec_short8 axs_v_load8(const int16_t *p)
{
    const uintptr_t a = (uintptr_t)p;
    const vec_short8 *q = (const vec_short8 *)(a & ~(uintptr_t)15);
    const unsigned off = (unsigned)(a & 15);
    return spu_or(spu_slqwbyte(q[0], off), spu_rlmaskqwbyte(q[1], (int)off - 16));
}

static uint32_t axs_resample_poly32_v(const int16_t *line, int16_t *out, uint32_t count,
                                      uint32_t pos, uint32_t ratio, const int16_t *coeffs, uint32_t *read_out)
{
    /* {p0[0], p1[0], p0[1], p1[1]}, then the lane-0 and lane-1 columns of four products. */
    static const vec_uchar16 interleave = {0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23};
    static const vec_uchar16 column0 = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
    static const vec_uchar16 column1 = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31};
    static const vec_uchar16 pack = {2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31};
    uint32_t read = 0, i;
    for (i = 0; i < count; i += 8) {
        vec_int4 sums[2];
        int g;
        for (g = 0; g < 2; ++g) {
            vec_int4 p[4];
            int k;
            for (k = 0; k < 4; ++k) {
                uintptr_t ca;
                vec_short8 t, c;
                pos += ratio;
                read += pos >> 16;
                pos &= 0xffffu;
                t = axs_v_load8(line + read);
                ca = (uintptr_t)(coeffs + ((pos >> 9) << 2));
                c = spu_slqwbyte(*(const vec_short8 *)(ca & ~(uintptr_t)15), (unsigned)(ca & 8));
                p[k] = spu_add(spu_mule(t, c), spu_mulo(t, c));   /* t0c0+t1c1, t2c2+t3c3 */
            }
            {
                const vec_int4 a = spu_shuffle(p[0], p[1], interleave);
                const vec_int4 b = spu_shuffle(p[2], p[3], interleave);
                vec_int4 v = spu_add(spu_shuffle(a, b, column0), spu_shuffle(a, b, column1));
                v = spu_rlmaska(v, -15);
                v = spu_sel(v, spu_splats(0x7fff), spu_cmpgt(v, spu_splats(0x7fff)));
                v = spu_sel(v, spu_splats(-0x8000), spu_cmpgt(spu_splats(-0x8000), v));
                sums[g] = v;
            }
        }
        *(vec_short8 *)(out + i) = (vec_short8)spu_shuffle(sums[0], sums[1], pack);
    }
    *read_out = read;
    return pos;
}
#endif

static uint32_t axs_resample_line(const int16_t *line, int16_t *out, uint32_t count,
                                  int16_t *last, uint32_t pos, uint32_t ratio, uint16_t src_type,
                                  const int16_t *coeffs, int fit32)
{
    uint32_t read = 0, i;
#if defined(__SPU__) || defined(AXS_VECTOR_STUB)
    if (coeffs && src_type == 0 && fit32 && (count & 7) == 0 && ((uintptr_t)out & 15) == 0 &&
        ((uintptr_t)coeffs & 15) == 0) {
        const uint32_t pos0 = pos;
        pos = axs_resample_poly32_v(line, out, count, pos, ratio, coeffs, &read);
        if (axs_v_check) {
            uint32_t p = pos0, r = 0;
            for (i = 0; i < count; ++i) {
                const int16_t *t, *c;
                p += ratio;
                r += p >> 16;
                p &= 0xffffu;
                t = line + r;
                c = coeffs + ((p >> 9) << 2);
                if (out[i] != axs_clamp_s16((t[0] * c[0] + t[1] * c[1] + t[2] * c[2] + t[3] * c[3]) >> 15)) {
                    ++axs_v_mismatch;
                    break;
                }
            }
            if (p != pos || r != read)
                ++axs_v_mismatch;
        }
        memcpy(last, line + read, sizeof(int16_t) * 4);
        return pos;
    }
#endif
    if (coeffs && src_type == 0) {
        for (i = 0; i < count; ++i) {
            const int16_t *t, *c;
            pos += ratio;
            read += pos >> 16;
            pos &= 0xffffu;
            t = line + read;
            c = coeffs + ((pos >> 9) << 2);
            if (fit32)
                out[i] = axs_clamp_s16((t[0] * c[0] + t[1] * c[1] + t[2] * c[2] + t[3] * c[3]) >> 15);
            else
                out[i] = axs_clamp_s16(((int64_t)t[0] * c[0] + (int64_t)t[1] * c[1] +
                                        (int64_t)t[2] * c[2] + (int64_t)t[3] * c[3]) >> 15);
        }
    } else if (src_type != 0 && src_type != 1) {
        for (i = 0; i < count; ++i)
            out[i] = line[4 + i];
        if (count >= 4)
            memcpy(last, out + count - 4, sizeof(int16_t) * 4);
        return pos;
    } else {
        for (i = 0; i < count; ++i) {
            const int16_t *t;
            pos += ratio;
            read += pos >> 16;
            pos &= 0xffffu;
            t = line + read;
            if (pos == 0) {
                out[i] = t[0];
            } else {
                const int32_t s0 = t[0], s1 = t[1];
                out[i] = (int16_t)((s0 * (uint16_t)(-pos) + s1 * (int32_t)pos) >> 16);
            }
        }
    }
    memcpy(last, line + read, sizeof(int16_t) * 4);
    return pos;
}

/* Where ResampleAudio's reader gets sample `index` from: the voice's accelerator, one
 * sample per call, or (the wiimote downsample) a block clamped at its last sample. */
typedef struct {
    AxsAccel *accel;
    const int16_t *block;
    uint32_t block_count;
} AxsReader;

static inline int16_t axs_reader_get(const AxsReader *r, uint32_t index)
{
    if (r->accel) {
        int16_t v;
        axs_read_samples(r->accel, &v, 1);
        return v;
    }
    return r->block[index < r->block_count - 1 ? index : r->block_count - 1];
}

static uint32_t axs_resample_audio(const AxsReader *rd, int16_t *out, uint32_t count, int16_t *last,
                                   uint32_t pos, uint32_t ratio, uint16_t src_type, const int16_t *coeffs)
{
    int16_t temp[4];
    uint32_t idx = 4, reads = 0, i;
    if (coeffs && src_type == 0) {
        memcpy(temp, last, sizeof temp);
        for (i = 0; i < count; ++i) {
            uint16_t frac_index;
            const int16_t *c;
            int64_t t0, t1, t2, t3;
            pos += ratio;
            while (pos >= 0x10000u) {
                temp[idx++ & 3] = axs_reader_get(rd, reads++);
                pos -= 0x10000u;
            }
            frac_index = (uint16_t)(((pos & 0xffffu) >> 9) << 2);
            c = coeffs + frac_index;
            t0 = temp[idx++ & 3];
            t1 = temp[idx++ & 3];
            t2 = temp[idx++ & 3];
            t3 = temp[idx++ & 3];
            out[i] = axs_clamp_s16((t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15);
        }
        last[3] = temp[--idx & 3];
        last[2] = temp[--idx & 3];
        last[1] = temp[--idx & 3];
        last[0] = temp[--idx & 3];
        return pos & 0xffffu;
    }
    if (src_type != 0 && src_type != 1) {
        for (i = 0; i < count; ++i)
            out[i] = axs_reader_get(rd, i);
        if (count >= 4)
            memcpy(last, out + count - 4, sizeof(int16_t) * 4);
        return pos;
    }
    memcpy(temp, last, sizeof temp);
    for (i = 0; i < count; ++i) {
        uint16_t frac;
        pos += ratio;
        while (pos >= 0x10000u) {
            temp[idx++ & 3] = axs_reader_get(rd, reads++);
            pos -= 0x10000u;
        }
        frac = pos & 0xffffu;
        if (frac == 0) {
            out[i] = temp[idx++ & 3];
            idx += 3;
        } else {
            const int32_t s0 = temp[idx++ & 3];
            const int32_t s1 = temp[idx++ & 3];
            idx += 2;
            out[i] = (int16_t)((s0 * (uint16_t)(-frac) + s1 * frac) >> 16);
        }
    }
    last[3] = temp[--idx & 3];
    last[2] = temp[--idx & 3];
    last[1] = temp[--idx & 3];
    last[0] = temp[--idx & 3];
    return pos & 0xffffu;
}

/* VoiceCoefficients: the voice's polyphase bank, or none (linear) for a bad selector. */
static inline const int16_t *axs_voice_coeffs(const int16_t *base, uint16_t select)
{
    uint32_t offset;
    if (!base)
        return 0;
    offset = (uint32_t)select * 0x200u;
    if (offset + 0x200u > AXS_COEF_COUNT)
        return 0;
    return base + offset;
}

/* AXWii::Resample */
static void axs_resample(AxsAccel *accel, AxsPB *pb, int16_t *out, uint32_t count,
                         const int16_t *coeffs, int fit32)
{
    const uint32_t ratio = axs_hilo(pb->src.ratio_hi, pb->src.ratio_lo);
    const uint64_t needed = (pb->src_type == 0 || pb->src_type == 1)
        ? ((uint64_t)pb->src.cur_addr_frac + (uint64_t)count * ratio) >> 16
        : count;
    if (needed <= 1024) {
        int16_t line[1024 + 4];
        memcpy(line, pb->src.last_samples, sizeof(int16_t) * 4);
        axs_read_samples(accel, line + 4, (uint32_t)needed);
        pb->src.cur_addr_frac = (uint16_t)axs_resample_line(
            line, out, count, pb->src.last_samples, pb->src.cur_addr_frac, ratio, pb->src_type,
            axs_voice_coeffs(coeffs, pb->coef_select), fit32);
        return;
    }
    {
        AxsReader rd;
        rd.accel = accel;
        rd.block = 0;
        rd.block_count = 0;
        pb->src.cur_addr_frac = (uint16_t)axs_resample_audio(
            &rd, out, count, pb->src.last_samples, pb->src.cur_addr_frac, ratio, pb->src_type,
            axs_voice_coeffs(coeffs, pb->coef_select));
    }
}

/* ---- volume, filters, mixing (ax_mix_kernels.h scalar forms, AXWii statics) -------- */

static inline uint16_t axs_scale_ramp(int16_t *s, uint32_t count, uint16_t volume, uint16_t delta)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        s[i] = axs_clamp32_s16(((int32_t)s[i] * (int32_t)volume) >> 15);
        volume = (uint16_t)(volume + delta);
    }
    return volume;
}

static inline void axs_mix_add(int32_t *out, const int16_t *in, uint32_t count, AxsVolume *vd,
                               int16_t *dpop, int ramp)
{
    const uint16_t delta = ramp ? vd->volume_delta : 0;
    uint16_t volume = vd->volume;
    int16_t last = *dpop;
    uint32_t i;
    if (volume == 0 && delta == 0) {
        if (count != 0)
            *dpop = 0;
        return;
    }
    for (i = 0; i < count; ++i) {
        const int16_t sample = axs_clamp32_s16(((int32_t)in[i] * (int32_t)volume) >> 15);
        out[i] += sample;
        volume = (uint16_t)(volume + delta);
        last = sample;
    }
    *dpop = last;
    vd->volume = volume;
}

static inline void axs_lpf(int16_t *s, uint32_t count, AxsLpf *f)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
        f->yn1 = s[i] = axs_clamp_s16((f->a0 * (int32_t)s[i] + f->b0 * (int32_t)f->yn1) >> 15);
}

static inline void axs_biquad(int16_t *s, uint32_t count, AxsBiquad *f)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        const int16_t xn0 = s[i];
        int64_t value = 0;
        int16_t yn0;
        value += f->b0 * (int32_t)xn0;
        value += f->b1 * (int32_t)f->xn1;
        value += f->b2 * (int32_t)f->xn2;
        value += f->a1 * (int32_t)f->yn1;
        value += f->a2 * (int32_t)f->yn2;
        value <<= 2;
        value += (value & 0x10000) ? 0x8000 : 0x7fff;
        yn0 = axs_clamp_s16(value >> 16);
        f->xn2 = f->xn1;
        f->yn2 = f->yn1;
        f->xn1 = xn0;
        f->yn1 = yn0;
        s[i] = yn0;
    }
}

#if defined(__SPU__) || defined(AXS_VECTOR_STUB)
/* The same two kernels four samples at a time, for a full frame of aligned samples.
 *
 * Each lane computes clamp((in * volume) >> 15) with in an int16 and volume the uint16
 * ramp value, exactly as the scalar loop: mpy multiplies the halfwords as signed, so a
 * volume of 0x8000 or more is short by in << 16, which is added back. The 32-bit
 * product is exact either way, so the shift and clamp see the scalar loop's value. */
#include <spu_intrinsics.h>

static const vec_uchar16 axs_v_lo = {0x80, 0x80, 0, 1, 0x80, 0x80, 2, 3, 0x80, 0x80, 4, 5, 0x80, 0x80, 6, 7};
static const vec_uchar16 axs_v_hi = {0x80, 0x80, 8, 9, 0x80, 0x80, 10, 11, 0x80, 0x80, 12, 13, 0x80, 0x80, 14, 15};
static const vec_uchar16 axs_v_pack = {2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31};

static inline vec_int4 axs_v_scale(vec_int4 in32, vec_uint4 volume)
{
    /* in32 holds the sample in its low halfword (high halfword zero). */
    const vec_int4 product = spu_add(spu_mulo((vec_short8)in32, (vec_short8)volume),
                                     spu_and(spu_sl(in32, 16),
                                             (vec_int4)spu_cmpgt(volume, spu_splats(0x7fffu))));
    vec_int4 v = spu_rlmaska(product, -15);
    v = spu_sel(v, spu_splats(0x7fff), spu_cmpgt(v, spu_splats(0x7fff)));
    v = spu_sel(v, spu_splats(-0x8000), spu_cmpgt(spu_splats(-0x8000), v));
    return v;
}

static inline vec_uint4 axs_v_ramp_start(uint16_t volume, uint16_t delta)
{
    const vec_uint4 steps = {0, 1, 2, 3};
    return spu_and(spu_add(spu_splats((uint32_t)volume),
                           spu_mulo((vec_ushort8)steps, (vec_ushort8)spu_splats((uint32_t)delta))),
                   spu_splats(0xffffu));
}

/* axs_scale_ramp for count == 96, s 16-aligned. */
static inline uint16_t axs_scale_ramp_v(int16_t *s, uint16_t volume, uint16_t delta)
{
    vec_short8 *p = (vec_short8 *)s;
    vec_uint4 v = axs_v_ramp_start(volume, delta);
    const vec_uint4 step = spu_splats((uint32_t)(uint16_t)(delta * 4u));
    const vec_uint4 mask = spu_splats(0xffffu);
    uint32_t i;
    for (i = 0; i < AXS_SAMPLES_PER_FRAME / 8; ++i) {
        const vec_short8 x = p[i];
        const vec_int4 a = axs_v_scale((vec_int4)spu_shuffle(x, x, axs_v_lo), v);
        v = spu_and(spu_add(v, step), mask);
        const vec_int4 b = axs_v_scale((vec_int4)spu_shuffle(x, x, axs_v_hi), v);
        v = spu_and(spu_add(v, step), mask);
        p[i] = (vec_short8)spu_shuffle(a, b, axs_v_pack);
    }
    return (uint16_t)(volume + delta * AXS_SAMPLES_PER_FRAME);
}

/* axs_mix_add's loop for count == 96, out and in 16-aligned. Returns the last sample. */
static inline int16_t axs_mix_add_v(int32_t *out, const int16_t *in, uint16_t volume, uint16_t delta)
{
    const vec_short8 *p = (const vec_short8 *)in;
    vec_int4 *o = (vec_int4 *)out;
    vec_uint4 v = axs_v_ramp_start(volume, delta);
    const vec_uint4 step = spu_splats((uint32_t)(uint16_t)(delta * 4u));
    const vec_uint4 mask = spu_splats(0xffffu);
    vec_int4 b = spu_splats(0);
    uint32_t i;
    for (i = 0; i < AXS_SAMPLES_PER_FRAME / 8; ++i) {
        const vec_short8 x = p[i];
        const vec_int4 a = axs_v_scale((vec_int4)spu_shuffle(x, x, axs_v_lo), v);
        v = spu_and(spu_add(v, step), mask);
        b = axs_v_scale((vec_int4)spu_shuffle(x, x, axs_v_hi), v);
        v = spu_and(spu_add(v, step), mask);
        o[2 * i] = spu_add(o[2 * i], a);
        o[2 * i + 1] = spu_add(o[2 * i + 1], b);
    }
    return (int16_t)spu_extract(b, 3);
}
#endif

/* The vector kernels where they apply, the scalar ones everywhere else. */

static inline uint16_t axs_scale_ramp_any(int16_t *s, uint32_t count, uint16_t volume, uint16_t delta)
{
#if defined(__SPU__) || defined(AXS_VECTOR_STUB)
    if (count == AXS_SAMPLES_PER_FRAME && ((uintptr_t)s & 15) == 0) {
        if (axs_v_check) {
            int16_t ref[AXS_SAMPLES_PER_FRAME];
            uint16_t ref_volume, result;
            memcpy(ref, s, sizeof ref);
            ref_volume = axs_scale_ramp(ref, count, volume, delta);
            result = axs_scale_ramp_v(s, volume, delta);
            if (result != ref_volume || memcmp(ref, s, sizeof ref) != 0)
                ++axs_v_mismatch;
            return result;
        }
        return axs_scale_ramp_v(s, volume, delta);
    }
#endif
    return axs_scale_ramp(s, count, volume, delta);
}

static inline void axs_mix_add_any(int32_t *out, const int16_t *in, uint32_t count, AxsVolume *vd,
                                   int16_t *dpop, int ramp)
{
#if defined(__SPU__) || defined(AXS_VECTOR_STUB)
    const uint16_t delta = ramp ? vd->volume_delta : 0;
    if (count == AXS_SAMPLES_PER_FRAME && ((uintptr_t)out & 15) == 0 && ((uintptr_t)in & 15) == 0) {
        int32_t ref[AXS_SAMPLES_PER_FRAME];
        AxsVolume ref_vd = *vd;
        int16_t ref_dpop = *dpop;
        if (axs_v_check) {
            memcpy(ref, out, sizeof ref);
            axs_mix_add(ref, in, count, &ref_vd, &ref_dpop, ramp);
        }
        if (vd->volume == 0 && delta == 0) {
            *dpop = 0;
        } else {
            *dpop = axs_mix_add_v(out, in, vd->volume, delta);
            vd->volume = (uint16_t)(vd->volume + delta * AXS_SAMPLES_PER_FRAME);
        }
        if (axs_v_check && (memcmp(ref, out, sizeof ref) != 0 || ref_vd.volume != vd->volume ||
                            ref_dpop != *dpop))
            ++axs_v_mismatch;
        return;
    }
#endif
    axs_mix_add(out, in, count, vd, dpop, ramp);
}

static uint32_t axs_mixer_control(uint32_t m)
{
    uint32_t r = 0;
    if (m & 0x00000001) r |= AXS_MAIN_L;
    if (m & 0x00000002) r |= AXS_MAIN_R;
    if (m & 0x00000004) r |= AXS_MAIN_L | AXS_MAIN_R | AXS_MAIN_L_RAMP | AXS_MAIN_R_RAMP;
    if (m & 0x00000008) r |= AXS_MAIN_S;
    if (m & 0x00000010) r |= AXS_MAIN_S | AXS_MAIN_S_RAMP;
    if (m & 0x00010000) r |= AXS_AUXA_L;
    if (m & 0x00020000) r |= AXS_AUXA_R;
    if (m & 0x00040000) r |= AXS_AUXA_L | AXS_AUXA_R | AXS_AUXA_L_RAMP | AXS_AUXA_R_RAMP;
    if (m & 0x00080000) r |= AXS_AUXA_S;
    if (m & 0x00100000) r |= AXS_AUXA_S | AXS_AUXA_S_RAMP;
    if (m & 0x00200000) r |= AXS_AUXB_L;
    if (m & 0x00400000) r |= AXS_AUXB_R;
    if (m & 0x00800000) r |= AXS_AUXB_L | AXS_AUXB_R | AXS_AUXB_L_RAMP | AXS_AUXB_R_RAMP;
    if (m & 0x01000000) r |= AXS_AUXB_S;
    if (m & 0x02000000) r |= AXS_AUXB_S | AXS_AUXB_S_RAMP;
    if (m & 0x04000000) r |= AXS_AUXC_L;
    if (m & 0x08000000) r |= AXS_AUXC_R;
    if (m & 0x10000000) r |= AXS_AUXC_L | AXS_AUXC_R | AXS_AUXC_L_RAMP | AXS_AUXC_R_RAMP;
    if (m & 0x20000000) r |= AXS_AUXC_S;
    if (m & 0x40000000) r |= AXS_AUXC_S | AXS_AUXC_S_RAMP;
    return r;
}

/* ---- one voice, one PB list (AXWii::ProcessVoice, AXWii::ProcessPBList) ----------- */

/* Returns 1 when the voice was running and has been mixed. */
static int axs_process_voice(AxsPB *pb, int32_t *const *buses, uint32_t count, uint32_t control,
                             const int16_t *coeffs, int fit32, int new_filter)
{
    AxsAccel accel;
    int16_t samples[AXS_SAMPLES_PER_FRAME] __attribute__((aligned(16)));
    if (pb->running != 1)
        return 0;
    memset(samples, 0, sizeof samples);
    axs_accel_setup(&accel, pb);
    axs_resample(&accel, pb, samples, count, coeffs, fit32);
    axs_accel_commit(&accel);
    pb->vol_env.cur_volume = (int16_t)axs_scale_ramp_any(samples, count, (uint16_t)pb->vol_env.cur_volume,
                                                     (uint16_t)pb->vol_env.cur_volume_delta);
    if (pb->lpf.on)
        axs_lpf(samples, count, &pb->lpf);
    if (new_filter && pb->filt.biquad.on)
        axs_biquad(samples, count, &pb->filt.biquad);

#define AXS_MIX(bit, rbit, bus, vd, dp) \
    if (control & (bit)) axs_mix_add_any(buses[bus], samples, count, &pb->mixer.vd, &pb->dpop.dp, (control & (rbit)) != 0)
    AXS_MIX(AXS_MAIN_L, AXS_MAIN_L_RAMP, 0, main_left, main_left);
    AXS_MIX(AXS_MAIN_R, AXS_MAIN_R_RAMP, 1, main_right, main_right);
    AXS_MIX(AXS_MAIN_S, AXS_MAIN_S_RAMP, 2, main_surround, main_surround);
    AXS_MIX(AXS_AUXA_L, AXS_AUXA_L_RAMP, 3, auxA_left, auxA_left);
    AXS_MIX(AXS_AUXA_R, AXS_AUXA_R_RAMP, 4, auxA_right, auxA_right);
    AXS_MIX(AXS_AUXA_S, AXS_AUXA_S_RAMP, 5, auxA_surround, auxA_surround);
    AXS_MIX(AXS_AUXB_L, AXS_AUXB_L_RAMP, 6, auxB_left, auxB_left);
    AXS_MIX(AXS_AUXB_R, AXS_AUXB_R_RAMP, 7, auxB_right, auxB_right);
    AXS_MIX(AXS_AUXB_S, AXS_AUXB_S_RAMP, 8, auxB_surround, auxB_surround);
    AXS_MIX(AXS_AUXC_L, AXS_AUXC_L_RAMP, 9, auxC_left, auxC_left);
    AXS_MIX(AXS_AUXC_R, AXS_AUXC_R_RAMP, 10, auxC_right, auxC_right);
    AXS_MIX(AXS_AUXC_S, AXS_AUXC_S_RAMP, 11, auxC_surround, auxC_surround);
#undef AXS_MIX

    if (pb->remote) {
        const uint32_t wm_count = count == AXS_SAMPLES_PER_FRAME ? 18u : 6u;
        int16_t wm[AXS_WM_SAMPLES_PER_FRAME];
        AxsReader rd;
        if (new_filter && pb->remote_iir.on) {
            if (pb->remote_iir.on == 2)
                axs_biquad(samples, count, &pb->remote_iir.biquad);
            else
                axs_lpf(samples, count, &pb->remote_iir.lpf);
        }
        memset(wm, 0, sizeof wm);
        rd.accel = 0;
        rd.block = samples;
        rd.block_count = count;
        pb->remote_src.cur_addr_frac = (uint16_t)axs_resample_audio(
            &rd, wm, wm_count, pb->remote_src.last_samples, pb->remote_src.cur_addr_frac,
            0x00055555u, 0, coeffs);
#define AXS_MIXWM(ch, bus, vd) do { \
        const uint16_t mode_ = (uint16_t)((pb->remote_mixer_control >> ((ch) * 2)) & 3u); \
        if (mode_) axs_mix_add(buses[bus], wm, wm_count, &pb->remote_mixer.vd, &pb->remote_dpop.vd, (mode_ & 2u) != 0); \
    } while (0)
        AXS_MIXWM(0, 12, main0);
        AXS_MIXWM(1, 13, aux0);
        AXS_MIXWM(2, 14, main1);
        AXS_MIXWM(3, 15, aux1);
        AXS_MIXWM(4, 16, main2);
        AXS_MIXWM(5, 17, aux2);
        AXS_MIXWM(6, 18, main3);
        AXS_MIXWM(7, 19, aux3);
#undef AXS_MIXWM
    }
    return 1;
}

static void axs_read_pb(uint32_t addr, AxsPB *pb, const AxsSegment *seg, unsigned nseg)
{
    unsigned i;
    memset(pb, 0, sizeof *pb);
    for (i = 0; i < nseg; ++i)
        axs_guest_get((uint8_t *)pb + seg[i].host, addr + seg[i].guest, seg[i].bytes);
}

static void axs_write_pb(uint32_t addr, const AxsPB *pb, const AxsSegment *seg, unsigned nseg)
{
    unsigned i;
    for (i = 0; i < nseg; ++i)
        axs_guest_put(addr + seg[i].guest, (const uint8_t *)pb + seg[i].host, seg[i].bytes);
}

/* ProcessPBList for the new command layouts (no per-millisecond updates), with a known
 * PB layout. Returns the number of voices mixed. */
static uint32_t axs_process_pb_list(uint32_t pb_addr, int32_t *const *buses, const AxsSegment *seg,
                                    unsigned nseg, const int16_t *coeffs, int fit32, int new_filter,
                                    uint32_t *pbs_seen)
{
    uint32_t guard = 0, voices = 0;
    while (pb_addr && guard++ < 256) {
        uint16_t running, w[2];
        AxsPB pb;
        axs_guest_get(&running, pb_addr + (uint32_t)offsetof(AxsPB, running), 2);
        if (running != 1) {
            axs_guest_get(&w[0], pb_addr, 2);
            axs_guest_get(&w[1], pb_addr + 2, 2);
            pb_addr = axs_hilo(w[0], w[1]);
            continue;
        }
        axs_read_pb(pb_addr, &pb, seg, nseg);
        voices += (uint32_t)axs_process_voice(
            &pb, buses, AXS_SAMPLES_PER_FRAME,
            axs_mixer_control(axs_hilo(pb.mixer_control_hi, pb.mixer_control_lo)),
            coeffs, fit32, new_filter);
        axs_write_pb(pb_addr, &pb, seg, nseg);
        pb_addr = axs_hilo(pb.next_pb_hi, pb.next_pb_lo);
    }
    if (pbs_seen)
        *pbs_seen = guard;
    return voices;
}

#endif
