/* SPDX-License-Identifier: MPL-2.0 */
/* Miracle Dual-Core Real-Time Port of fm_piano_v5.py + dsp_v4/v5.py + render_v5.py for M-VAVE FM-1:
 *   1. 16-Partial Stiff-String Coupled-Quadrature (Magic-Circle) Modal Bank per voice:
 *      - Zero LUT reads per partial; exact unit-determinant 2-mul/3-add oscillator in registers
 *      - Exact Yamaha C5 fine_structure + stretched tuning + stiff-string inharmonicity per partial
 *      - Block-rate dual-stage prompt + aftersound decay + 3-string unison beating + slow v5 drift
 *      - Linearly ramped per-sample partial amplitude (zero zipper noise, 6x fewer muls)
 *   2. Longitudinal Sum-Frequency Phantom Partials (Bank & Sujbert, m <= 64, vel^2 scaling)
 *   3. Boot-Synthesized Upper-Partial Dispersion Wavelets (partials 17..48 travelling pulse)
 *   4. Global 16-Mode Per-Key Bridge/Soundboard Structure Bank (structure_fit.json + Ege/Boutillon)
 *      - Every key excites its own spatial bridge signature cos(2*pi*cycles*(m-21)/87 + theta)
 *   5. 8192-Sample 5-Band NoisePool (90..1440 Hz hammer felt thud, diffuse case air, damper whump & pedal sigh)
 *   6. Sympathetic Open-String Combs + Undamped Treble Shimmer + 4-Line Hadamard FDN Concert Hall
 */
#include "plat.h"
#include "fastmath.h"
#include "dsp_piano.h"
#include "piano_data.h"

#define NOISE_LEN 8192u
#define NOISE_MASK (NOISE_LEN - 1u)
#define DISP_BANKS 8u
#define DISP_LEN 2048u

static piano_voice_t voices[PIANO_MAX_VOICES];
static uint32_t voice_seq;
static uint32_t voice_rng = 0x51C51905u;
static uint8_t pedal_state;
static float gain_smooth;

/* Dual-core per-core scratch buffers + SRAM LUTs & Dispersion Wavelets in .pool */
static float buf_c0[PIANO_BLOCK_MAX * 2u] PLAT_POOL;
static float buf_c1[PIANO_BLOCK_MAX * 2u] PLAT_POOL;
static float sin_lut[513] PLAT_POOL;
static float noise_pool[NOISE_LEN] PLAT_POOL;
static float disp_wave[DISP_BANKS][DISP_LEN] PLAT_POOL;
static uint32_t cur_n;

/* Global 16-mode Structure Resonator Bank (stereo L/R Magic-Circle decaying modes) */
static float st_uL[PIANO_NSTRUCT], st_vL[PIANO_NSTRUCT];
static float st_uR[PIANO_NSTRUCT], st_vR[PIANO_NSTRUCT];
static float st_eps[PIANO_NSTRUCT], st_dec[PIANO_NSTRUCT], st_gain[PIANO_NSTRUCT], st_sdrv[PIANO_NSTRUCT];

/* Pedal & Damper Mechanism state (render_v5.py mechanism() + damper lift sigh) */
static uint32_t mech_np_pos;
static float mech_env, mech_mul;
static float string_rms_sq;

/* Sympathetic open strings + early reflections + 4-line Hadamard FDN hall */
#define SYM_LEN0 1319u
#define SYM_LEN1 1747u
#define ER_LEN0 503u
#define ER_LEN1 647u
#define FDN_0 1481u
#define FDN_1 1877u
#define FDN_2 2341u
#define FDN_3 2897u

static float sym_buf0[SYM_LEN0] PLAT_POOL;
static float sym_buf1[SYM_LEN1] PLAT_POOL;
static float er_buf0[ER_LEN0] PLAT_POOL;
static float er_buf1[ER_LEN1] PLAT_POOL;
static float fdn0[FDN_0] PLAT_POOL;
static float fdn1[FDN_1] PLAT_POOL;
static float fdn2[FDN_2] PLAT_POOL;
static float fdn3[FDN_3] PLAT_POOL;
static float fdn_lp[4];
static float tilt_lp[2];
static float spk_hpf_x[2], spk_hpf_y[2];
static uint32_t sym_idx0, sym_idx1;
static uint32_t er_idx0, er_idx1;
static uint32_t fdn_idx[4];

#define SCOPE_N 64u
static int8_t scope_buf[SCOPE_N];
static uint32_t scope_idx;

static inline uint32_t next_rng(void)
{
    uint32_t x = voice_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    voice_rng = x;
    return x;
}

static inline float rng_uni(void)
{
    return (float)(next_rng() >> 8) * (1.0f / 16777216.0f);
}

static inline __attribute__((always_inline)) float sin_turns_lut(float turns)
{
    float p = turns - (float)(int32_t)turns;
    float idx, frac;
    uint32_t i0;
    if (p < 0.0f) p += 1.0f;
    idx = p * 512.0f;
    i0 = (uint32_t)idx;
    if (i0 >= 512u) i0 = 511u;
    frac = idx - (float)i0;
    return sin_lut[i0] + frac * (sin_lut[i0 + 1u] - sin_lut[i0]);
}

/* Per-block decay multiplier for rate in dB/sec over N samples */
static inline float db_per_sec_blk(float r_db, float n_samp)
{
    return fm_expf(-r_db * (0.11512925f / PIANO_FS) * n_samp);
}

static float vel_to_layer(uint8_t vel, const piano_cfg_t *cfg, uint8_t soft)
{
    static const float VP[8] = {1.0f, 20.0f, 35.0f, 48.0f, 68.0f, 93.0f, 117.0f, 127.0f};
    static const float LP[8] = {0.8f, 1.5f, 3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 16.0f};
    float v = (float)vel, layer = LP[7];
    uint32_t i;
    if (v <= VP[0]) {
        layer = LP[0];
    } else {
        for (i = 0; i < 7u; i++) {
            if (v <= VP[i + 1u]) {
                float t = (v - VP[i]) / (VP[i + 1u] - VP[i]);
                layer = LP[i] * (1.0f - t) + LP[i + 1u] * t;
                break;
            }
        }
    }
    static const float LID_OFF[4] = {-1.8f, -0.6f, 0.0f, 1.2f};
    static const float ACT_OFF[4] = {-0.9f, 0.0f, 0.8f, 0.3f};
    layer += LID_OFF[cfg->lid & 3u] + ACT_OFF[cfg->action & 3u];
    if (cfg->str == STR_FELT) layer -= 2.6f;
    if (soft) layer -= 1.0f;
    return fm_clampf(layer, 0.9f, 16.0f);
}

static void interp_layer(uint32_t ki, float layer, piano_layer_t *out)
{
    float pos = fm_clampf((layer - 3.0f) / 3.0f, 0.0f, 4.0f);
    uint32_t i0 = (uint32_t)pos;
    uint32_t i1 = i0 < 4u ? i0 + 1u : 4u;
    float t = pos - (float)i0;
    float u = 1.0f - t;
    const piano_layer_t *a = &PIANO_LAYER[ki][i0];
    const piano_layer_t *b = &PIANO_LAYER[ki][i1];
    uint32_t k;

    for (k = 0; k < PIANO_VPART; k++)
        out->amp[k] = a->amp[k] * u + b->amp[k] * t;
    out->disp_amp = a->disp_amp * u + b->disp_amp * t;
    out->knock_amp = a->knock_amp * u + b->knock_amp * t;
    out->stoch_amp = a->stoch_amp * u + b->stoch_amp * t;
}

/* Build 8192-sample 5-band noise pool (90, 180, 360, 720, 1440 Hz + 1.65 kHz LPF matching dsp_v5.py) */
static void build_noise_pool(void)
{
    static const float FC[5] = {90.0f, 180.0f, 360.0f, 720.0f, 1440.0f};
    static const float BG[5] = {0.355f, 0.562f, 0.794f, 1.000f, 0.447f};
    float s[5] = {0}, d[5] = {0}, c[5];
    float lp1 = 0.0f, lp2 = 0.0f, lp3 = 0.0f, lp4 = 0.0f;
    uint32_t i, b;
    float sum2 = 0.0f, scale;

    for (b = 0; b < 5u; b++)
        c[b] = (2.0f * FM_PI * FC[b]) / PIANO_FS;

    for (i = 0; i < NOISE_LEN + 256u; i++) {
        float w = ((float)(int32_t)(next_rng() >> 8) * (1.0f / 8388608.0f));
        float acc = 0.0f;
        for (b = 0; b < 5u; b++) {
            s[b] += c[b] * (w - s[b] - 0.55f * d[b]);
            d[b] += c[b] * s[b];
            acc += s[b] * BG[b];
        }
        lp1 += (acc - lp1) * 0.22f;
        lp2 += (lp1 - lp2) * 0.22f;
        lp3 += (lp2 - lp3) * 0.22f;
        lp4 += (lp3 - lp4) * 0.22f;
        if (i >= 256u) {
            uint32_t idx = i - 256u;
            noise_pool[idx] = lp4;
            sum2 += lp4 * lp4;
        }
    }
    scale = 1.0f / fm_sqrtf(sum2 / (float)NOISE_LEN + 1e-12f);
    for (i = 0; i < NOISE_LEN; i++)
        noise_pool[i] *= scale;
}

/* Pre-synthesize 8 register-spaced stiff-string upper-partial dispersion wavelets (partials 17..44) */
static void build_dispersion_wavelets(void)
{
    static const uint8_t REF_M[DISP_BANKS] = {24, 33, 42, 51, 60, 69, 78, 87};
    uint32_t b, i, p;
    for (b = 0; b < DISP_BANKS; b++) {
        const piano_key_t *pk = &PIANO_KEY[REF_M[b] - PIANO_M_MIN];
        float f1 = pk->f1, B = pk->B;
        float sum2 = 0.0f;
        for (i = 0; i < DISP_LEN; i++) disp_wave[b][i] = 0.0f;

        for (p = 17u; p <= 44u; p++) {
            float n = (float)p;
            float fn = n * f1 * fm_sqrtf((1.0f + B * n * n) / (1.0f + B));
            if (fn >= 18500.0f) break;
            {
                float wgt = 1.0f / (n - 12.0f);
                float g = 1.0f + (fn / 3500.0f);
                float r_db = pk->pr1[15] * g * 1.15f;
                float dec_mul = fm_expf(-r_db * (0.11512925f / PIANO_FS));
                float env = wgt;
                float ph = rng_uni();
                float inc = fn / PIANO_FS;
                if (fn > 7000.0f)
                    env *= fm_exp2f(-1.8f * fm_log2f(fn / 7000.0f));
                for (i = 0; i < DISP_LEN; i++) {
                    disp_wave[b][i] += sin_turns_lut(ph) * env;
                    ph += inc;
                    env *= dec_mul;
                }
            }
        }
        /* Smooth 128-sample end taper + normalize unit peak RMS */
        for (i = 0; i < DISP_LEN; i++) {
            if (i > DISP_LEN - 128u)
                disp_wave[b][i] *= (float)(DISP_LEN - i) * (1.0f / 128.0f);
            sum2 += disp_wave[b][i] * disp_wave[b][i];
        }
        if (sum2 > 1e-12f) {
            float sc = 1.0f / fm_sqrtf(sum2 / (float)DISP_LEN);
            for (i = 0; i < DISP_LEN; i++) disp_wave[b][i] *= sc * 0.42f;
        }
    }
}

static void init_structure_bank(void)
{
    uint32_t m;
    float norm_sq = 0.0f;
    for (m = 0; m < PIANO_NSTRUCT; m++) {
        float f = STRUCT_MODES[m].freq;
        float t60 = STRUCT_MODES[m].t60;
        st_uL[m] = st_vL[m] = st_uR[m] = st_vR[m] = 0.0f;
        st_eps[m] = 2.0f * fm_sinf(FM_PI * f / PIANO_FS);
        st_dec[m] = fm_expf(-6.907755f / (t60 * PIANO_FS));
        st_gain[m] = STRUCT_MODES[m].weight;
        norm_sq += st_gain[m] * st_gain[m];
    }
    if (norm_sq > 1e-9f) {
        float sc = 1.0f / fm_sqrtf(norm_sq);
        for (m = 0; m < PIANO_NSTRUCT; m++) {
            st_gain[m] *= sc;
            st_sdrv[m] = st_gain[m] * fm_sqrtf(1.0f - st_dec[m]) * 0.018f;
        }
    }
}

void dsp_piano_init(void)
{
    uint32_t i;
    for (i = 0; i <= 512u; i++)
        sin_lut[i] = fm_sin_turns((float)i * (1.0f / 512.0f));
    build_noise_pool();
    build_dispersion_wavelets();
    init_structure_bank();

    for (i = 0; i < PIANO_MAX_VOICES; i++)
        voices[i].active = 0;
    voice_seq = 0;
    pedal_state = 0;
    gain_smooth = 0.0f;
    mech_np_pos = 0;
    mech_env = 0.0f;
    mech_mul = fm_expf(-1.0f / (0.045f * PIANO_FS));
    string_rms_sq = 0.0f;

    for (i = 0; i < 2u; i++) {
        tilt_lp[i] = 0.0f;
        spk_hpf_x[i] = spk_hpf_y[i] = 0.0f;
    }
    for (i = 0; i < SYM_LEN0; i++) sym_buf0[i] = 0.0f;
    for (i = 0; i < SYM_LEN1; i++) sym_buf1[i] = 0.0f;
    for (i = 0; i < ER_LEN0; i++) er_buf0[i] = 0.0f;
    for (i = 0; i < ER_LEN1; i++) er_buf1[i] = 0.0f;
    for (i = 0; i < FDN_0; i++) fdn0[i] = 0.0f;
    for (i = 0; i < FDN_1; i++) fdn1[i] = 0.0f;
    for (i = 0; i < FDN_2; i++) fdn2[i] = 0.0f;
    for (i = 0; i < FDN_3; i++) fdn3[i] = 0.0f;
    for (i = 0; i < 4u; i++) { fdn_lp[i] = 0.0f; fdn_idx[i] = 0; }
    sym_idx0 = sym_idx1 = 0;
    er_idx0 = er_idx1 = 0;
    for (i = 0; i < SCOPE_N; i++) scope_buf[i] = 0;
    scope_idx = 0;
}

/* Inject per-key bridge force pulse into the 16-mode Structure resonator bank (dsp_v4.py Structure.ir(m) * pulse) */
static void excite_key_structure(uint8_t pitch, float knock_amp, float layer)
{
    uint32_t m;
    float pos = (float)(pitch - 21) * (1.0f / 87.0f);
    /* Contact duration T shrinks at high pitch & loud strike -> higher modes excited more at treble/ff */
    float fc_pulse = 320.0f + 18.0f * (float)(pitch - 21) + 25.0f * layer;
    for (m = 0; m < PIANO_NSTRUCT; m++) {
        float f = STRUCT_MODES[m].freq;
        float h_pulse = 1.0f / (1.0f + (f / fc_pulse) * (f / fc_pulse));
        float w_mid = (f >= 120.0f && f <= 280.0f) ? 1.55f : 1.0f;
        float ang = FM_TWO_PI * STRUCT_MODES[m].cycles * pos + STRUCT_MODES[m].theta;
        float spL = 0.55f + 0.45f * fm_cosf(ang);
        float spR = 0.55f + 0.45f * fm_cosf(ang + 0.85f);
        float amp = st_gain[m] * knock_amp * h_pulse * w_mid * 1.60f;
        st_vL[m] += spL * amp;
        st_vR[m] += spR * amp;
    }
}

void dsp_piano_note_on(uint8_t pitch, uint8_t vel, uint8_t role, uint8_t soft, const piano_cfg_t *cfg)
{
    uint32_t i, k, best = 0;
    float lowest_score = 1e30f;
    uint32_t ki;
    const piano_key_t *pk;
    piano_layer_t pl;
    piano_voice_t *v;
    float layer, unison_scale, bright_tilt, knock_scale, decay_scale;
    float pan_a, f1, blk_n = (float)PIANO_BLOCK_MAX;

    if (pitch < PIANO_M_MIN || pitch > PIANO_M_MAX || vel == 0)
        return;

    /* Same-pitch re-strike cuts previous voice cleanly like a real hammer/damper */
    for (i = 0; i < PIANO_MAX_VOICES; i++) {
        if (!voices[i].active) { best = i; break; }
        if (voices[i].pitch == pitch) { best = i; break; }
    }
    if (i == PIANO_MAX_VOICES) {
        for (i = 0; i < PIANO_MAX_VOICES; i++) {
            float sc = voices[i].vis_env * (voices[i].damped ? 0.20f : 1.0f) + (voices[i].role == 0 ? 0.35f : 0.0f);
            if (sc < lowest_score) { lowest_score = sc; best = i; }
        }
    }

    ki = (uint32_t)(pitch - PIANO_M_MIN);
    pk = &PIANO_KEY[ki];
    layer = vel_to_layer(vel, cfg, soft);
    interp_layer(ki, layer, &pl);

    {
        static const float LID_TLT[4] = {-0.14f, -0.05f, 0.0f, 0.08f};
        static const float STR_UN[4]  = {0.35f, 1.00f, 2.35f, 0.65f};
        static const float ACT_KN[4]  = {0.45f, 0.85f, 1.20f, 1.85f};
        static const float BODY_DEC[4]= {1.22f, 1.08f, 1.00f, 0.88f};
        bright_tilt = LID_TLT[cfg->lid & 3u] + (cfg->str == STR_FELT ? -0.18f : 0.0f);
        unison_scale = STR_UN[cfg->str & 3u];
        knock_scale = ACT_KN[cfg->action & 3u] * (cfg->body == BODY_UPRIGHT ? 1.15f : 1.0f);
        decay_scale = BODY_DEC[cfg->body & 3u];
    }

    v = &voices[best];
    v->active = 1;
    v->damped = 0;
    v->pitch = pitch;
    v->role = role;
    v->soft = soft;
    v->vel = vel;
    v->key_down = 1;
    v->age = ++voice_seq;
    v->ns = pitch > 46u ? 3u : 2u;

    f1 = pk->f1;
    v->npart = 0u;
    {
        float base_beat_hz = f1 * (pk->det_ratio - 1.0f) * unison_scale;
        v->beat_a1 = 0.28f * fm_minf(1.5f, unison_scale);
        v->beat_a2 = (v->ns == 3u) ? (0.20f * fm_minf(1.5f, unison_scale)) : 0.0f;

        for (k = 0; k < PIANO_VPART; k++) {
            float fk = f1 * pk->pratio[k];
            float amp = pl.amp[k];
            if (fk >= 18200.0f || amp < 2.0e-6f) {
                v->mc_eps[k] = v->mc_base_eps[k] = 0.0f;
                v->p_prompt[k] = v->p_mid[k] = v->p_after[k] = 0.0f;
                continue;
            }
            v->npart = (uint8_t)(k + 1u);

            if (bright_tilt != 0.0f && k > 0u)
                amp *= fm_exp2f(bright_tilt * (float)k * 0.25f);

            {
                float eps = 2.0f * fm_sinf(FM_PI * fk / PIANO_FS);
                /* v5 line 96: lowest 3 partials in bass (6 in mid/treble) start coherent; above start random phase */
                uint32_t coh_lim = (pitch <= 52u) ? 3u : 6u;
                float phi0 = (k < coh_lim) ? 0.0f : (rng_uni() * FM_TWO_PI);
                float s0 = fm_sinf(phi0);
                float c0 = fm_cosf(phi0);
                float denom = fm_sqrtf(fm_maxf(0.05f, 1.0f - 0.25f * eps * eps));
                v->mc_base_eps[k] = eps;
                v->mc_eps[k] = eps;
                v->mc_u[k] = s0;
                v->mc_v[k] = (c0 - 0.5f * eps * s0) / denom;
            }

            {
                /* Exact fm_piano_v5.py 3-string decay stages: (1-w2-w3, r1), (w3, 1.2*sqrt(r1*r2)), (w2, r2) */
                float w2 = pk->pw2[k];
                float w3 = (v->ns == 3u && k < 10u) ? (fm_sqrtf(w2) * 0.3981f) : 0.0f;
                float j1 = 0.88f + 0.24f * rng_uni();
                float j2 = 0.88f + 0.24f * rng_uni();
                float r1k = pk->pr1[k] * decay_scale * j1;
                float r2k = pk->pr2[k] * decay_scale * j2;
                float rmk = 1.2f * fm_sqrtf(r1k * r2k);
                if (soft || cfg->str == STR_FELT)
                    w2 = fm_minf(0.55f, w2 * 1.25f);
                if (w2 + w3 > 0.85f) w3 = 0.85f - w2;
                v->p_prompt[k] = amp * (1.0f - w2 - w3);
                v->p_mid[k] = amp * w3 * 0.76f;
                v->p_after[k] = amp * w2 * 0.82f;
                v->d_prompt_blk[k] = db_per_sec_blk(r1k, blk_n);
                v->d_mid_blk[k] = db_per_sec_blk(rmk, blk_n);
                v->d_after_blk[k] = db_per_sec_blk(r2k, blk_n);
            }

            /* Each partial beats at its harmonic multiple of the string detuning */
            v->beat_ph1[k] = rng_uni() * 0.20f;
            v->beat_ph2[k] = rng_uni() * 0.25f;
            v->beat_inc1[k] = (base_beat_hz * (float)(k + 1u) * (0.88f + 0.24f * rng_uni())) * (blk_n / PIANO_FS);
            v->beat_inc2[k] = (base_beat_hz * 0.71f * (float)(k + 1u) * (0.88f + 0.24f * rng_uni())) * (blk_n / PIANO_FS);
        }
    }

    /* v5 slow unison wander + hard-strike initial sharpness (0.0028 * (vel/127)^3) */
    v->wander_ph = rng_uni();
    v->wander_inc = (0.28f + 0.30f * rng_uni()) * (blk_n / PIANO_FS);
    v->wander_depth = 0.00026f * unison_scale;
    {
        float vn = (float)vel * (1.0f / 127.0f);
        v->sharp_depth = 0.0028f * vn * vn * vn;
        v->sharp_env = v->sharp_depth > 0.00018f ? 1.0f : 0.0f;
    }

    /* Longitudinal sum-frequency phantom partials near fL = (17 or 15.5) * f1 for m <= 64 (v5 lines 103-114) */
    for (k = 0; k < PIANO_NPHANTOM; k++) {
        v->ph_amp[k] = 0.0f;
        v->ph_eps[k] = 0.0f;
    }
    if (pitch <= 64u && v->npart >= 6u) {
        float fL = (pitch <= 46u ? 17.0f : 15.5f) * f1;
        uint32_t p_pairs[2][2] = {{3u, 11u}, {5u, 10u}};
        for (k = 0; k < PIANO_NPHANTOM; k++) {
            uint32_t ia = p_pairs[k][0], ib = p_pairs[k][1];
            if (ib < v->npart) {
                float fs = f1 * (pk->pratio[ia] + pk->pratio[ib]);
                if (fs < 9800.0f) {
                    float df = (fs - fL) / (0.14f * fL);
                    float res = 1.0f / (1.0f + df * df);
                    float a_ij = 0.15f * pl.amp[ia] * pl.amp[ib] * res;
                    float eps = 2.0f * fm_sinf(FM_PI * fs / PIANO_FS);
                    float phi = rng_uni() * FM_TWO_PI;
                    float s0 = fm_sinf(phi), c0 = fm_cosf(phi);
                    v->ph_eps[k] = eps;
                    v->ph_u[k] = s0;
                    v->ph_v[k] = (c0 - 0.5f * eps * s0) / fm_sqrtf(fm_maxf(0.05f, 1.0f - 0.25f * eps * eps));
                    v->ph_amp[k] = a_ij;
                    v->ph_dec_blk[k] = db_per_sec_blk((pk->pr1[ia] + pk->pr1[ib]) * decay_scale, blk_n);
                }
            }
        }
    }

    /* Upper-partial dispersion wavelet (partials 17..44 travelling pulse) */
    {
        static const uint8_t REF_M[DISP_BANKS] = {24, 33, 42, 51, 60, 69, 78, 87};
        uint32_t bi = 0, d;
        uint32_t best_dist = 999u;
        for (d = 0; d < DISP_BANKS; d++) {
            uint32_t dist = pitch > REF_M[d] ? (uint32_t)(pitch - REF_M[d]) : (uint32_t)(REF_M[d] - pitch);
            if (dist < best_dist) { best_dist = dist; bi = d; }
        }
        v->disp_bank = (uint8_t)bi;
        v->disp_pos_q12 = 0u;
        {
            float ref_f1 = PIANO_KEY[REF_M[bi] - PIANO_M_MIN].f1;
            float ratio = fm_clampf(f1 / ref_f1, 0.5f, 2.0f);
            v->disp_inc_q12 = (uint32_t)(ratio * 4096.0f);
        }
        v->disp_amp = pl.disp_amp * (cfg->str == STR_FELT ? 0.35f : 1.0f);
        v->disp_mul = fm_expf(-1.0f / (0.045f * PIANO_FS));
    }

    /* Attack & 4-class frequency-dependent damper release (fm_piano_v4.py DAMP_CLASS = 1.0, 0.75, 0.55, 0.40) */
    {
        static const float DCLASS[4] = {1.0f, 0.75f, 0.55f, 0.40f};
        float slow = fm_clampf(layer / 9.0f, 0.3f, 1.0f);
        float att_s = pk->tau_a * (1.0f + 0.5f * (1.0f - slow)) * (cfg->str == STR_FELT ? 1.7f : 1.0f);
        float rel_s = fm_maxf(0.14f, pk->tau_r * 1.85f);
        v->att_env = 1.0f;
        v->att_mul = fm_expf(-1.0f / (fm_maxf(0.0010f, att_s) * PIANO_FS));
        for (k = 0; k < 4u; k++) {
            v->damp_c[k] = 1.0f;
            v->damp_blk[k] = fm_expf(-blk_n / ((rel_s * DCLASS[k]) * PIANO_FS));
        }
    }

    /* Excite per-key bridge structure bank + 5-band NoisePool blow & diffuse tail */
    excite_key_structure(pitch, pl.knock_amp * knock_scale, layer);
    {
        float np_lvl = pl.stoch_amp * 2.1f * knock_scale;
        v->np_pos = next_rng() & NOISE_MASK;
        v->np_fast = np_lvl;
        v->np_fast_mul = fm_expf(-1.0f / (0.024f * PIANO_FS));
        v->np_tail = np_lvl * 0.30f;
        v->np_tail_mul = fm_expf(-1.0f / (0.480f * PIANO_FS));
    }

    pan_a = (fm_clampf(pk->pan, -1.0f, 1.0f) + 1.0f) * (0.25f * FM_PI);
    v->gl = 1.4142135f * fm_cosf(pan_a);
    v->gr = 1.4142135f * fm_sinf(pan_a);
    v->vis_env = fm_clampf(0.25f + (float)vel / 140.0f, 0.2f, 1.0f);
}

void dsp_piano_note_off(uint8_t pitch)
{
    uint32_t i;
    for (i = 0; i < PIANO_MAX_VOICES; i++) {
        piano_voice_t *v = &voices[i];
        if (v->active && v->pitch == pitch) {
            v->key_down = 0;
            if (!pedal_state && pitch < 90u)
                v->damped = 1;
        }
    }
}

void dsp_piano_set_pedal(uint8_t down)
{
    uint32_t i;
    uint8_t next_p = down ? 1u : 0u;
    if (next_p == pedal_state)
        return;

    /* Audible damper & pedal mechanism (render_v5.py mechanism() + damper lift sigh) */
    mech_np_pos = next_rng() & NOISE_MASK;
    if (next_p && !pedal_state) {
        /* Dampers lifting: soft open-string sigh */
        mech_env = 0.0022f;
        mech_mul = fm_expf(-1.0f / (0.038f * PIANO_FS));
    } else {
        /* Dampers landing on moving strings: soft low whump proportional to active string RMS */
        float rms = fm_sqrtf(string_rms_sq + 1e-12f);
        mech_env = 0.028f * rms + 0.0012f;
        if (mech_env > 0.018f) mech_env = 0.018f;
        mech_mul = fm_expf(-1.0f / (0.048f * PIANO_FS));
        for (i = 0; i < PIANO_MAX_VOICES; i++) {
            piano_voice_t *v = &voices[i];
            if (v->active && !v->key_down && v->pitch < 90u)
                v->damped = 1;
        }
    }
    pedal_state = next_p;
}

void dsp_piano_all_off(void)
{
    uint32_t i;
    pedal_state = 0;
    for (i = 0; i < PIANO_MAX_VOICES; i++) {
        if (voices[i].active) {
            voices[i].key_down = 0;
            voices[i].damped = 1;
        }
    }
}

void dsp_piano_panic(void)
{
    uint32_t i;
    pedal_state = 0;
    for (i = 0; i < PIANO_MAX_VOICES; i++)
        voices[i].active = 0;
}

void dsp_piano_shed(void)
{
    uint32_t i, best = 0;
    float low = 1e30f;
    for (i = 0; i < PIANO_MAX_VOICES; i++) {
        if (voices[i].active) {
            float s = voices[i].vis_env * (voices[i].damped ? 0.15f : 1.0f);
            if (s < low) { low = s; best = i; }
        }
    }
    if (low < 1e20f) {
        uint32_t k;
        voices[best].damped = 1u;
        for (k = 0; k < 4u; k++) voices[best].damp_blk[k] = 0.12f;
    }
}

PLAT_RAM_CODE static void render_voice_range(uint32_t v0, uint32_t v1, float *out, uint32_t n)
{
    uint32_t i, vi, k;
    float inv_n = 1.0f / (float)n;

    for (i = 0; i < 2u * n; i++)
        out[i] = 0.0f;

    for (vi = v0; vi < v1; vi++) {
        piano_voice_t *v = &voices[vi];
        float blk_ab[2][PIANO_BLOCK_MAX];
        float drift_mul, energy_sum = 0.0f;
        uint32_t npart;

        if (!v->active)
            continue;

        for (i = 0; i < n; i++) {
            blk_ab[0][i] = 0.0f;
            blk_ab[1][i] = 0.0f;
        }
        /* 1. Block-rate v5 wander + hard-strike pitch settle */
        v->wander_ph += v->wander_inc;
        if (v->wander_ph >= 1.0f) v->wander_ph -= 1.0f;
        drift_mul = 1.0f + v->sharp_env * v->sharp_depth + v->wander_depth * sin_turns_lut(v->wander_ph);
        v->sharp_env *= 0.918f;

        if (v->damped) {
            for (k = 0; k < 4u; k++)
                v->damp_c[k] *= v->damp_blk[k];
        }

        /* 2. Render 16 true stiff-string partials via register-resident Magic-Circle loops (2 muls + 3 adds/sample!) */
        npart = v->npart;
        for (k = 0; k < npart; k++) {
            uint32_t cls = (k == 0u) ? 0u : (k < 3u) ? 1u : (k < 8u) ? 2u : 3u;
            float dm0 = v->damp_c[cls];
            float p0 = v->p_prompt[k] + v->p_mid[k] + v->p_after[k];
            float p1, b0, b1, a_cur, a_step, u, vv, eps;

            if (p0 * dm0 < 3.0e-6f) {
                v->p_prompt[k] *= v->d_prompt_blk[k];
                v->p_mid[k] *= v->d_mid_blk[k];
                v->p_after[k] *= v->d_after_blk[k];
                continue;
            }

            /* Advance 3-string unison beating envelope from start to end of block */
            b0 = 1.0f + v->beat_a1 * sin_turns_lut(v->beat_ph1[k]) + v->beat_a2 * sin_turns_lut(v->beat_ph2[k]);
            v->beat_ph1[k] += v->beat_inc1[k];
            if (v->beat_ph1[k] >= 1.0f) v->beat_ph1[k] -= 1.0f;
            v->beat_ph2[k] += v->beat_inc2[k];
            if (v->beat_ph2[k] >= 1.0f) v->beat_ph2[k] -= 1.0f;
            b1 = 1.0f + v->beat_a1 * sin_turns_lut(v->beat_ph1[k]) + v->beat_a2 * sin_turns_lut(v->beat_ph2[k]);

            v->p_prompt[k] *= v->d_prompt_blk[k];
            v->p_mid[k] *= v->d_mid_blk[k];
            v->p_after[k] *= v->d_after_blk[k];
            p1 = v->p_prompt[k] + v->p_mid[k] + v->p_after[k];
            energy_sum += p1 * dm0;
            a_cur = p0 * b0 * dm0;
            a_step = (p1 * b1 * dm0 - a_cur) * inv_n;

            u = v->mc_u[k];
            vv = v->mc_v[k];
            eps = v->mc_base_eps[k] * drift_mul;
            {
                float *dst = blk_ab[k & 1u];
                for (i = 0; i < n; i++) {
                    u += eps * vv;
                    vv -= eps * u;
                    dst[i] += u * a_cur;
                    a_cur += a_step;
                }
            }
            v->mc_u[k] = u;
            v->mc_v[k] = vv;
        }

        /* 3. Longitudinal sum-frequency phantom partials (m <= 64) */
        for (k = 0; k < PIANO_NPHANTOM; k++) {
            float pa0 = v->ph_amp[k] * v->damp_c[3];
            if (pa0 > 3.0e-5f) {
                float pa1, a_cur, a_step, u = v->ph_u[k], vv = v->ph_v[k], eps = v->ph_eps[k];
                v->ph_amp[k] *= v->ph_dec_blk[k];
                pa1 = v->ph_amp[k] * v->damp_c[3];
                a_cur = pa0;
                a_step = (pa1 - pa0) * inv_n;
                for (i = 0; i < n; i++) {
                    u += eps * vv;
                    vv -= eps * u;
                    blk_ab[k & 1u][i] += u * a_cur;
                    a_cur += a_step;
                }
                v->ph_u[k] = u;
                v->ph_v[k] = vv;
            }
        }

        /* 4. Upper-partial dispersion wavelet + attack envelope + 5-band NoisePool + stereo pan */
        {
            float att = v->att_env, att_m = v->att_mul;
            float da = v->disp_amp, dm = v->disp_mul;
            uint32_t dpos = v->disp_pos_q12, dinc = v->disp_inc_q12;
            const float *dtab = disp_wave[v->disp_bank];
            uint32_t npos = v->np_pos;
            float npf = v->np_fast, npfm = v->np_fast_mul;
            float npt = v->np_tail, nptm = v->np_tail_mul;
            uint32_t has_d = (da > 5.0e-5f) && ((dpos >> 12) < DISP_LEN - 2u);
            uint32_t has_n = (npf + npt) > 4.0e-5f;
            float gl_a = v->gl * 1.08f, gr_a = v->gr * 0.91f;
            float gl_b = v->gl * 0.91f, gr_b = v->gr * 1.08f;

            for (i = 0; i < n; i++) {
                float env_att = 1.0f - att;
                float sa = blk_ab[0][i] * env_att;
                float sb = blk_ab[1][i] * env_att;
                float s_ex = 0.0f;
                att *= att_m;

                if (has_d) {
                    uint32_t di = dpos >> 12;
                    if (di < DISP_LEN - 2u) {
                        float df = (float)(dpos & 4095u) * (1.0f / 4096.0f);
                        float hi_att = 1.0f - att * att;
                        s_ex += (dtab[di] + df * (dtab[di + 1u] - dtab[di])) * da * hi_att;
                        dpos += dinc;
                        da *= dm;
                    } else {
                        has_d = 0u;
                    }
                }
                if (has_n) {
                    s_ex += noise_pool[npos & NOISE_MASK] * (npf + npt) * env_att;
                    npos++;
                    npf *= npfm;
                    npt *= nptm;
                }

                out[2u * i] += sa * gl_a + sb * gl_b + s_ex * v->gl;
                out[2u * i + 1u] += sa * gr_a + sb * gr_b + s_ex * v->gr;
            }

            v->att_env = att;
            v->disp_amp = da;
            v->disp_pos_q12 = dpos;
            v->np_pos = npos;
            v->np_fast = npf;
            v->np_tail = npt;
        }

        {
            float ve = energy_sum * 4.5f;
            v->vis_env = ve < 0.0f ? 0.0f : ve > 1.0f ? 1.0f : ve;
            if (energy_sum < 1.2e-4f && v->np_tail < 4.0e-5f)
                v->active = 0;
        }
    }
}

PLAT_RAM_CODE static void cpu2_worker(uint32_t arg)
{
    (void)arg;
    render_voice_range(6u, PIANO_MAX_VOICES, buf_c1, cur_n);
}

static inline __attribute__((always_inline)) float soft_limit(float x, float knee)
{
    float ax = x < 0.0f ? -x : x;
    if (ax <= knee)
        return x;
    {
        float head = 0.90f - knee;
        float over = (ax - knee) / head;
        float lim = knee + head * (over / (1.0f + over));
        return x < 0.0f ? -lim : lim;
    }
}

static inline __attribute__((always_inline)) float ram_flush(float x)
{
    return (x > -1e-20f && x < 1e-20f) ? 0.0f : x;
}

PLAT_RAM_CODE static void mix_stage_ram(int32_t *lr, uint32_t n, int hp,
                                        float gain_target, float hall_wet, float hall_fb, float lp_c,
                                        float body_amt, float tilt_c, float dry_mix, float sym_fb)
{
    uint32_t i, m;
    float rms_acc = 0.0f;

    for (i = 0; i < n; i++) {
        float dry_l = buf_c0[2u * i] + buf_c1[2u * i];
        float dry_r = buf_c0[2u * i + 1u] + buf_c1[2u * i + 1u];
        float str_mono = (dry_l + dry_r) * 0.5f;
        float body_l = 0.0f, body_r = 0.0f;
        float er_l, er_r, in_m, sy0, sy1;
        float d0, d1, d2, d3, s_all, y_l, y_r;

        rms_acc += str_mono * str_mono;
        gain_smooth += (gain_target - gain_smooth) * 0.004f;

        /* 1. 16-mode Structure Resonator Bank (per-key hammer knock + transient soundboard bloom) */
        {
            for (m = 0; m < PIANO_NSTRUCT; m++) {
                float eps = st_eps[m], dec = st_dec[m];
                float q_drive = str_mono * st_sdrv[m];
                float uL = st_uL[m] + eps * st_vL[m];
                float vL = (st_vL[m] - eps * uL) * dec + q_drive;
                float uR = st_uR[m] + eps * st_vR[m];
                float vR = (st_vR[m] - eps * uR) * dec + q_drive;
                st_uL[m] = ram_flush(uL); st_vL[m] = ram_flush(vL);
                st_uR[m] = ram_flush(uR); st_vR[m] = ram_flush(vR);
                body_l += uL;
                body_r += uR;
            }
            dry_l += body_l * body_amt;
            dry_r += body_r * body_amt;
        }

        /* 2. Audible pedal & damper felt landing/sigh mechanism */
        if (mech_env > 2.0e-5f) {
            float mn = noise_pool[mech_np_pos & NOISE_MASK] * mech_env;
            mech_np_pos++;
            mech_env *= mech_mul;
            dry_l += mn;
            dry_r += mn * 0.92f;
        }

        /* 3. Sympathetic open-string resonance (RES_DB = -19 dB when pedal down) */
        sy0 = sym_buf0[sym_idx0];
        sy1 = sym_buf1[sym_idx1];
        sym_buf0[sym_idx0] = ram_flush(dry_l * 0.09f + sy0 * sym_fb);
        sym_buf1[sym_idx1] = ram_flush(dry_r * 0.09f + sy1 * sym_fb);
        if (++sym_idx0 >= SYM_LEN0) sym_idx0 = 0;
        if (++sym_idx1 >= SYM_LEN1) sym_idx1 = 0;
        dry_l += sy0 * 0.24f;
        dry_r += sy1 * 0.24f;

        /* 4. Image-source early reflections + 4-line Hadamard FDN concert hall (HALL_DB = -8 dB) */
        er_l = er_buf0[er_idx0];
        er_r = er_buf1[er_idx1];
        er_buf0[er_idx0] = dry_l * 0.45f + dry_r * 0.25f;
        er_buf1[er_idx1] = dry_r * 0.45f + dry_l * 0.25f;
        if (++er_idx0 >= ER_LEN0) er_idx0 = 0;
        if (++er_idx1 >= ER_LEN1) er_idx1 = 0;

        in_m = (dry_l + dry_r) * 0.34f + (er_l + er_r) * 0.20f;
        d0 = fdn0[fdn_idx[0]];
        d1 = fdn1[fdn_idx[1]];
        d2 = fdn2[fdn_idx[2]];
        d3 = fdn3[fdn_idx[3]];

        fdn_lp[0] += (d0 - fdn_lp[0]) * (1.0f - lp_c);
        fdn_lp[1] += (d1 - fdn_lp[1]) * (1.0f - lp_c);
        fdn_lp[2] += (d2 - fdn_lp[2]) * (1.0f - lp_c);
        fdn_lp[3] += (d3 - fdn_lp[3]) * (1.0f - lp_c);

        s_all = (fdn_lp[0] + fdn_lp[1] + fdn_lp[2] + fdn_lp[3]) * 0.5f;
        fdn0[fdn_idx[0]] = ram_flush((s_all - fdn_lp[0]) * hall_fb + in_m);
        fdn1[fdn_idx[1]] = ram_flush((s_all - fdn_lp[1]) * hall_fb - in_m * 0.8f);
        fdn2[fdn_idx[2]] = ram_flush((s_all - fdn_lp[2]) * hall_fb + in_m * 0.7f);
        fdn3[fdn_idx[3]] = ram_flush((s_all - fdn_lp[3]) * hall_fb - in_m * 0.6f);

        if (++fdn_idx[0] >= FDN_0) fdn_idx[0] = 0;
        if (++fdn_idx[1] >= FDN_1) fdn_idx[1] = 0;
        if (++fdn_idx[2] >= FDN_2) fdn_idx[2] = 0;
        if (++fdn_idx[3] >= FDN_3) fdn_idx[3] = 0;

        y_l = dry_l * dry_mix + (er_l * 0.36f + (d0 - d1 + d2) * 0.40f) * hall_wet;
        y_r = dry_r * dry_mix + (er_r * 0.36f + (d1 - d2 + d3) * 0.40f) * hall_wet;

        /* 5. Listening-distance treble tilt (render_v4.py TILT) */
        if (tilt_c > 0.01f) {
            tilt_lp[0] += (y_l - tilt_lp[0]) * (1.0f - tilt_c);
            tilt_lp[1] += (y_r - tilt_lp[1]) * (1.0f - tilt_c);
            y_l = tilt_lp[0];
            y_r = tilt_lp[1];
        }

        if (hp != 1) {
            float hp1_l = 0.9885f * (spk_hpf_y[0] + y_l - spk_hpf_x[0]);
            float hp1_r = 0.9885f * (spk_hpf_y[1] + y_r - spk_hpf_x[1]);
            spk_hpf_x[0] = y_l; spk_hpf_y[0] = ram_flush(hp1_l);
            spk_hpf_x[1] = y_r; spk_hpf_y[1] = ram_flush(hp1_r);
            y_l = hp1_l;
            y_r = hp1_r;
        }

        y_l = soft_limit(y_l, hp == 1 ? 0.76f : 0.68f);
        y_r = soft_limit(y_r, hp == 1 ? 0.76f : 0.68f);

        if ((i & 7u) == 0u) {
            int32_t sv = (int32_t)((y_l + y_r) * 280.0f);
            if (sv < -127) sv = -127;
            if (sv > 127) sv = 127;
            scope_buf[scope_idx] = (int8_t)sv;
            if (++scope_idx >= SCOPE_N) scope_idx = 0;
        }

        lr[2u * i] = (int32_t)(y_l * gain_smooth);
        lr[2u * i + 1u] = (int32_t)(y_r * gain_smooth);
    }

    string_rms_sq = string_rms_sq * 0.75f + (rms_acc / (float)n) * 0.25f;
}

void dsp_piano_render(int32_t *lr, uint32_t n, uint32_t master_q12, const piano_cfg_t *cfg)
{
    uint32_t half = 6u;
    int c2, hp;
    float gain_target, hall_wet, hall_fb, lp_c, sym_fb;
    float body_amt, tilt_c, dry_mix;

    if (n > PIANO_BLOCK_MAX)
        n = PIANO_BLOCK_MAX;
    cur_n = n;

    c2 = plat_cpu2_on() && plat_cpu2_run(cpu2_worker, 0u);
    PLAT_FAR(render_voice_range)(0u, half, buf_c0, n);
    if (!c2 || plat_cpu2_wait() != 0)
        PLAT_FAR(render_voice_range)(half, PIANO_MAX_VOICES, buf_c1, n);

    hp = plat_headphones();
    gain_target = ((float)master_q12 / 4096.0f) * (hp == 1 ? 5600000.0f : 4300000.0f);

    {
        static const float ROOM_FB[4]  = {0.48f, 0.65f, 0.79f, 0.87f};
        static const float ROOM_WET[4] = {0.14f, 0.24f, 0.32f, 0.44f};
        static const float SEAT_WET[4] = {0.55f, 0.85f, 1.15f, 1.45f};
        static const float SEAT_DRY[4] = {1.05f, 0.98f, 0.88f, 0.76f};
        static const float SEAT_LP[4]  = {0.02f, 0.05f, 0.14f, 0.26f};
        static const float B_AMT[4]    = {0.24f, 0.28f, 0.36f, 0.46f};
        uint32_t rm = cfg->room & 3u, st = cfg->seat & 3u, bd = cfg->body & 3u;
        hall_fb = ROOM_FB[rm] + (pedal_state ? 0.035f : 0.0f);
        if (hall_fb > 0.90f) hall_fb = 0.90f;
        hall_wet = ROOM_WET[rm] * SEAT_WET[st];
        dry_mix = SEAT_DRY[st];
        lp_c = 0.16f + (float)rm * 0.04f + (pedal_state ? 0.04f : 0.0f);
        tilt_c = SEAT_LP[st];
        sym_fb = pedal_state ? 0.72f : 0.18f;
        body_amt = B_AMT[bd];
    }

    PLAT_FAR(mix_stage_ram)(lr, n, hp, gain_target, hall_wet, hall_fb, lp_c,
                            body_amt, tilt_c, dry_mix, sym_fb);
}

uint32_t dsp_piano_active_count(void)
{
    uint32_t i, c = 0;
    for (i = 0; i < PIANO_MAX_VOICES; i++)
        if (voices[i].active) c++;
    return c;
}

float dsp_piano_peak_env(void)
{
    uint32_t i;
    float mx = 0.0f;
    for (i = 0; i < PIANO_MAX_VOICES; i++)
        if (voices[i].active && voices[i].vis_env > mx) mx = voices[i].vis_env;
    return mx;
}

void dsp_piano_key_vis(uint8_t out_88[88])
{
    uint32_t i;
    for (i = 0; i < 88u; i++) out_88[i] = 0;
    for (i = 0; i < PIANO_MAX_VOICES; i++) {
        if (voices[i].active && voices[i].pitch >= PIANO_M_MIN && voices[i].pitch <= PIANO_M_MAX) {
            uint32_t ki = (uint32_t)(voices[i].pitch - PIANO_M_MIN);
            uint8_t val = (uint8_t)fm_clampf(voices[i].vis_env * 255.0f, 1.0f, 255.0f);
            if (val > out_88[ki]) out_88[ki] = val;
        }
    }
}

int dsp_piano_scope_get(int8_t *out, uint32_t n)
{
    uint32_t i, base = scope_idx;
    if (n > SCOPE_N) n = SCOPE_N;
    for (i = 0; i < n; i++)
        out[i] = scope_buf[(base + i) % SCOPE_N];
    return (int)n;
}
