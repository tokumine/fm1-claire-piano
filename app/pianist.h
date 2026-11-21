/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stdint.h>
#include "piano_data.h"
#include "dsp_piano.h"

#define PIANIST_NSLOTS (SCORE_NPIECES + 4u)
#define PIANIST_NBUILTIN SCORE_NPIECES
#define PIANIST_USER_FIRST PIANIST_NBUILTIN
#define PIANIST_NUSER 4u

#define USER_SONG_MAGIC 0x55534E31u /* "USN1" */
#define USER_MAX_NOTES 440u
#define USER_MAX_PEDAL 32u

enum { PEDAL_DRY = 0, PEDAL_HALF = 1, PEDAL_AUTO = 2, PEDAL_LUSH = 3, PEDAL_NMODES = 4 };
enum { PLAY_STOP = 0, PLAY_RUN = 1, PLAY_REC_ARMED = 2, PLAY_REC = 3 };

enum {
    KEYMD_PIANO = 0, /* Pure 27-key FM Grand Piano */
    KEYMD_CHORD = 1, /* Keys 0..6 = modifiers (5,s2,6,s4,7,dim,9), keys 7..26 = FM Grand Piano chords */
    KEYMD_TAP   = 2, /* Tap Pianist: tap any key to perform the loaded score with your timing & velocity */
    KEYMD_COUNT = 3
};

enum {
    MOD_AUTO = 0, /* Quick tap (<360ms) = toggle lock; hold + chord = momentary */
    MOD_LOCK = 1,
    MOD_HOLD = 2,
    MOD_MODE_COUNT = 3
};

enum {
    SPREAD_CLOSE = 0, /* Tight block chord */
    SPREAD_OPEN  = 1, /* Wide concert voicing */
    SPREAD_ROLL  = 2, /* Gentle pianist rolled chord */
    SPREAD_COUNT = 3
};

/* Chord modifier bits (keys 0..6 in KEYMD_CHORD: F3..B3) */
#define CMOD_5TH  (1u << 0) /* k=0 F3  */
#define CMOD_SUS2 (1u << 1) /* k=1 F#3 */
#define CMOD_6TH  (1u << 2) /* k=2 G3  */
#define CMOD_SUS4 (1u << 3) /* k=3 G#3 */
#define CMOD_7TH  (1u << 4) /* k=4 A3  */
#define CMOD_DIM  (1u << 5) /* k=5 A#3 */
#define CMOD_9TH  (1u << 6) /* k=6 B3  */

enum {
    CAT_ALL = 0,
    CAT_DEBUSSY = 1,
    CAT_CHOPIN = 2,
    CAT_BEETHOVEN = 3,
    CAT_BAROQUE = 4,
    CAT_CLASSICAL = 5,
    CAT_ROMANTIC = 6,
    CAT_RAGTIME = 7,
    CAT_USER = 8,
    CAT_COUNT = 9
};

typedef struct {
    uint16_t dt_ms;
    uint16_t dur_ms;
    uint8_t pitch;
    uint8_t vel;
    uint8_t flags;
    uint8_t bar;
} score_note_t;

typedef struct {
    uint16_t down_ds;
    uint16_t up_ds;
} score_pedal_t;

typedef struct {
    uint32_t magic;
    uint16_t nnotes;
    uint16_t npedal;
    uint16_t duration_10ms;
    uint16_t rsv;
    score_note_t notes[USER_MAX_NOTES];
    score_pedal_t pedal[USER_MAX_PEDAL];
} user_song_t;

typedef struct {
    uint8_t tempo_pct;  /* 50 .. 150 */
    uint8_t rubato;     /* 0 .. 100 */
    uint8_t voicing;    /* 0 .. 100 */
    uint8_t pedal_mode; /* PEDAL_DRY..PEDAL_LUSH */
    uint8_t soft_pedal; /* una corda override */
} pianist_cfg_t;

typedef struct {
    uint8_t key_mode;   /* KEYMD_PIANO, KEYMD_CHORD, KEYMD_TAP */
    uint8_t mod_mode;   /* MOD_AUTO, MOD_LOCK, MOD_HOLD */
    uint8_t spread;     /* SPREAD_CLOSE, SPREAD_OPEN, SPREAD_ROLL */
    uint8_t guide_led;  /* 0=OFF, 1=SCALE, 2=CHORD */
} keys_cfg_t;

typedef struct {
    uint8_t mode;          /* PLAY_* */
    uint8_t song_idx;
    uint16_t bar;
    uint16_t total_bars;
    uint32_t elapsed_ms;
    uint32_t total_ms;
    uint32_t note_idx;
    uint32_t total_notes;
    uint8_t pedal_down;
    uint8_t soft_on;
    float lh_pos;
    float rh_pos;
    float energy;
    uint8_t pianist_key[88];
    uint8_t live_key[88];
} pianist_vis_t;

void pianist_init(void);
void pianist_set_song(uint8_t idx);
void pianist_step_in_cat(uint8_t cat, int32_t delta);
void pianist_play_toggle(void);
void pianist_stop(void);
void pianist_rec_toggle(void);
void pianist_clear_user_song(uint8_t user_slot);
void pianist_seek_bar(int32_t delta_bars);

/* Direct piano notes */
void pianist_live_note_on(uint8_t pitch, uint8_t vel, const piano_cfg_t *pcfg, const pianist_cfg_t *perf);
void pianist_live_note_off(uint8_t pitch);
void pianist_live_pedal(uint8_t down, const pianist_cfg_t *perf);

/* Tap Pianist + Piano Chord Mode on the 27 FM-1 keys */
void pianist_key_event(uint8_t key_idx, uint8_t down, uint8_t vel, int8_t oct_shift,
                       const piano_cfg_t *pcfg, const pianist_cfg_t *perf, const keys_cfg_t *kcfg);
void pianist_clear_mods(void);

void pianist_tick_block(uint32_t n_frames, const pianist_cfg_t *perf, const piano_cfg_t *pcfg);

/* UI & LED queries */
const char *pianist_song_name(uint8_t idx);
const char *pianist_composer_name(uint8_t idx);
const char *pianist_cat_name(uint8_t cat);
uint8_t pianist_song_cat(uint8_t idx);
const char *pianist_chord_badge(void);
void pianist_mod_label(char *out, uint32_t cap);
uint8_t pianist_active_mod_mask(void);
uint8_t pianist_key_guide_bright(uint8_t key_idx, int8_t oct_shift, const keys_cfg_t *kcfg);
uint8_t pianist_user_dirty(uint8_t slot);
const char *pianist_fx_badge(void);
uint32_t pianist_active_fx_mask(void);
void pianist_clear_dirty(uint8_t slot);
user_song_t *pianist_user_buf(uint8_t slot);
const pianist_vis_t *pianist_vis(void);
