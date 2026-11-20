/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>

#define PIANO_FS 44100.0f
#define PIANO_MAX_VOICES 14u
#define PIANO_BLOCK_MAX 256u
#define PIANO_VPART 16u
#define PIANO_NPHANTOM 2u

/* Semantic Piano Body & Hall/Stage parameters */
enum { LID_CLOSED = 0, LID_HALF = 1, LID_OPEN = 2, LID_CONCERT = 3 };
enum { BODY_UPRIGHT = 0, BODY_BABY = 1, BODY_SALON = 2, BODY_CONCERT = 3 };
enum { STR_PURE = 0, STR_AGED = 1, STR_HONKY = 2, STR_FELT = 3 };
enum { ACT_LIGHT = 0, ACT_MEDIUM = 1, ACT_HEAVY = 2, ACT_NOISY = 3 };
enum { ROOM_PARLOR = 0, ROOM_RECITAL = 1, ROOM_HALL = 2, ROOM_CATHEDRAL = 3 };
enum { SEAT_BENCH = 0, SEAT_FRONT = 1, SEAT_BALCONY = 2, SEAT_BACK = 3 };

typedef struct {
    uint8_t lid;     /* 0..3: CLOSED, HALF, OPEN, CONCERT */
    uint8_t body;    /* 0..3: UPRIGHT, BABY, SALON, CONCERT */
    uint8_t str;     /* 0..3: PURE, AGED, HONKY, FELT */
    uint8_t action;  /* 0..3: LIGHT, MEDIUM, HEAVY, NOISY */
    uint8_t room;    /* 0..3: PARLOR, RECITAL, HALL, CATHEDRAL */
    uint8_t seat;    /* 0..3: BENCH, FRONT, BALCONY, BACK */
} piano_cfg_t;

typedef struct {
    uint32_t age;
    uint8_t active;
    uint8_t damped;
    uint8_t pitch;
    uint8_t role;
    uint8_t soft;
    uint8_t vel;
    uint8_t key_down;
    uint8_t npart;   /* active stiff-string partials <= 16 */
    uint8_t ns;      /* 2 or 3 unison strings */

    /* Block-Envelope Magic-Circle Stiff-String Modal Bank (zero-LUT, 2 muls + 3 adds/sample/partial) */
    float mc_u[PIANO_VPART];
    float mc_v[PIANO_VPART];
    float mc_eps[PIANO_VPART];
    float mc_base_eps[PIANO_VPART];
    /* 3-stage v5 unison envelopes: prompt (r1), bridge-coupled 3rd string (w3), aftersound (w2) */
    float p_prompt[PIANO_VPART];
    float p_mid[PIANO_VPART];
    float p_after[PIANO_VPART];
    float d_prompt_blk[PIANO_VPART];
    float d_mid_blk[PIANO_VPART];
    float d_after_blk[PIANO_VPART];

    /* 3-string unison beating & slow v5 organic phase wander per partial */
    float beat_ph1[PIANO_VPART];
    float beat_ph2[PIANO_VPART];
    float beat_inc1[PIANO_VPART];
    float beat_inc2[PIANO_VPART];
    float beat_a1, beat_a2;
    float wander_ph, wander_inc, wander_depth;
    float sharp_env, sharp_depth;

    /* Longitudinal sum-frequency phantom partials (Bank & Sujbert, m <= 64) */
    float ph_u[PIANO_NPHANTOM];
    float ph_v[PIANO_NPHANTOM];
    float ph_eps[PIANO_NPHANTOM];
    float ph_amp[PIANO_NPHANTOM];
    float ph_dec_blk[PIANO_NPHANTOM];

    /* Upper-partial stiff-string dispersion wavelet (partials 17..48 travelling pulse) */
    uint8_t disp_bank;
    uint32_t disp_pos_q12;
    uint32_t disp_inc_q12;
    float disp_amp, disp_mul;

    /* Attack & partial-class damper release (DAMP_CLASS: 1.0, 0.75, 0.55, 0.40) */
    float att_env, att_mul;
    float damp_c[4];
    float damp_blk[4];

    /* v5 NoisePool: 5-band hammer felt thud + diffuse soundboard/case air */
    uint32_t np_pos;
    float np_fast, np_fast_mul;
    float np_tail, np_tail_mul;

    /* Stereo pan + Haas interaural sample offset */
    float gl, gr;

    /* Current display energy (0..1) */
    float vis_env;
} piano_voice_t;

void dsp_piano_init(void);
void dsp_piano_note_on(uint8_t pitch, uint8_t vel, uint8_t role, uint8_t soft, const piano_cfg_t *cfg);
void dsp_piano_note_off(uint8_t pitch);
void dsp_piano_set_pedal(uint8_t down);
void dsp_piano_all_off(void);
void dsp_piano_panic(void);
void dsp_piano_shed(void);

/* Render v5 modal piano voices + 16-mode key-bridge structure + pedal mechanism + sympathetic strings + hall */
void dsp_piano_render(int32_t *lr, uint32_t n, uint32_t master_q12, const piano_cfg_t *cfg);

uint32_t dsp_piano_active_count(void);
float dsp_piano_peak_env(void);
void dsp_piano_key_vis(uint8_t out_88[88]);
int dsp_piano_scope_get(int8_t *out, uint32_t n);
