/* SPDX-License-Identifier: MPL-2.0 */
/* Configurable Pianist, Tap Pianist, Piano Chord Mode & LZ4 Repertoire Engine (v2). */
#include "plat.h"
#include "fastmath.h"
#include "pianist.h"

#define MAX_SCHED_OFF 32u
#define MAX_SCHED_ON 16u
#define ACTIVE_MAX_NOTES 7600u
#define ACTIVE_MAX_PEDAL 240u
#define DECOMP_BUF_SZ (ACTIVE_MAX_NOTES * 4u + ACTIVE_MAX_PEDAL * 4u)

typedef struct {
    uint8_t active;
    uint8_t pitch;
    uint32_t off_sample;
} sched_off_t;

typedef struct {
    uint8_t active;
    uint8_t pitch;
    uint8_t vel;
    uint8_t role;
    uint8_t soft;
    uint32_t fire_sample;
    uint32_t dur_samples;
} sched_on_t;

static user_song_t user_songs[PIANIST_NUSER] PLAT_POOL;
static uint8_t decomp_buf[DECOMP_BUF_SZ] PLAT_POOL;

static const uint8_t *act_dts;
static const uint8_t *act_durs;
static const uint8_t *act_ps;
static const uint8_t *act_vrs;
static const uint16_t *act_ped;
static uint32_t act_nn;
static uint32_t act_np;
static uint32_t act_dur_ms;
static uint16_t act_nbars;
static uint8_t act_is_user;

static uint8_t user_dirty[PIANIST_NUSER];
static pianist_vis_t vis;

static sched_off_t sched_off[MAX_SCHED_OFF];
static sched_on_t sched_on[MAX_SCHED_ON];

static uint32_t rng_state = 0x19051893u;
static float score_pos_ms;
static float next_note_ms;
static uint32_t audio_sample_now;
static uint32_t ped_idx;
static uint8_t score_pedal_down;
static uint8_t live_pedal_down;
static uint32_t live_vis_decay;

/* Lightweight harmonic chord badge & LED guide tracker */
static float pc_energy[12];
static float bass_energy[12];
static uint8_t cur_root;
static uint8_t cur_scale_id;
static uint16_t cur_scale_mask;
static uint8_t cur_chord_pcs[4];
static uint8_t harmony_dirty;
static uint32_t harmony_timer_ms;
static char cur_chord_name[12];

/* Chord modifier & Tap Pianist per-key release tracking */
static uint8_t mod_lock_mask;
static uint8_t mod_held_mask;
static uint8_t mod_used_chord[7];
static uint32_t mod_down_ms[7];
static uint32_t jam_clock_ms;

static uint8_t key_notes[27][8];
static uint8_t key_note_cnt[27];

/* Recording state */
static uint32_t rec_sample;
static uint32_t rec_last_on_sample;
static int16_t rec_open_idx[128];
static uint32_t rec_open_sample[128];
static int16_t rec_ped_open;

static const char *const NOTE_NAMES[12] = {
    "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
};

static const uint16_t SCALE_MASKS[5] = {
    0x0AB5u, /* Major */
    0x05ADu, /* Nat Minor */
    0x06ADu, /* Dorian */
    0x06B5u, /* Mixolydian */
    0x09ADu  /* Harm Minor */
};

static const char *const CAT_NAMES[CAT_COUNT] = {
    "ALL (91)", "DEBUSSY/SATIE", "CHOPIN", "BEETHOVEN",
    "BAROQUE", "CLASSICAL", "ROMANTIC", "RAGTIME/WALTZ",
    "USER SLOTS"
};

static const char *const USER_NAMES[PIANIST_NUSER] = {
    "USER SONG 1", "USER SONG 2", "USER SONG 3", "USER SONG 4"
};

static void str_copy_cat(char *dst, uint32_t cap, const char *a, const char *b)
{
    uint32_t i = 0;
    while (i + 1u < cap && a && *a) dst[i++] = *a++;
    while (i + 1u < cap && b && *b) dst[i++] = *b++;
    dst[i] = '\0';
}

static void append_tok(char *dst, uint32_t cap, const char *tok)
{
    uint32_t i = 0;
    while (dst[i] && i + 1u < cap) i++;
    if (i > 0u && i + 1u < cap) dst[i++] = ' ';
    while (i + 1u < cap && *tok) dst[i++] = *tok++;
    dst[i] = '\0';
}

static inline uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static inline float rand_sym(void)
{
    return (float)(int32_t)(xorshift32() >> 8) * (1.0f / 8388608.0f);
}

static uint16_t rotate_mask(uint16_t mask, uint8_t root)
{
    uint16_t lo = (uint16_t)((mask << root) & 0x0FFFu);
    uint16_t hi = (uint16_t)(mask >> (12u - root));
    return (uint16_t)(lo | hi);
}

static void update_harmony_model(void)
{
    uint8_t r, s, best_r = 0, best_s = 0;
    float best_score = -1e9f;

    for (r = 0; r < 12u; r++) {
        float root_bonus = bass_energy[r] * 1.9f + pc_energy[r] * 0.7f;
        for (s = 0; s < 5u; s++) {
            uint16_t m = rotate_mask(SCALE_MASKS[s], r);
            float sc = root_bonus;
            uint8_t pc;
            for (pc = 0; pc < 12u; pc++) {
                if (m & (1u << pc)) sc += pc_energy[pc];
                else sc -= pc_energy[pc] * 1.4f;
            }
            if (r == cur_root) sc += 0.28f;
            if (s == cur_scale_id) sc += 0.15f;
            if (sc > best_score) {
                best_score = sc;
                best_r = r;
                best_s = s;
            }
        }
    }

    cur_root = best_r;
    cur_scale_id = best_s;
    cur_scale_mask = rotate_mask(SCALE_MASKS[best_s], best_r);

    {
        uint8_t degs[7], dc = 0, off;
        for (off = 0; off < 12u && dc < 7u; off++) {
            uint8_t pc = (uint8_t)((cur_root + off) % 12u);
            if (cur_scale_mask & (1u << pc))
                degs[dc++] = pc;
        }
        cur_chord_pcs[0] = degs[0];
        cur_chord_pcs[1] = dc > 2 ? degs[2] : (uint8_t)((cur_root + 4u) % 12u);
        cur_chord_pcs[2] = dc > 4 ? degs[4] : (uint8_t)((cur_root + 7u) % 12u);
        cur_chord_pcs[3] = dc > 6 ? degs[6] : (uint8_t)((cur_root + 11u) % 12u);

        {
            uint8_t i3 = (uint8_t)((cur_chord_pcs[1] + 12u - cur_root) % 12u);
            uint8_t i5 = (uint8_t)((cur_chord_pcs[2] + 12u - cur_root) % 12u);
            uint8_t i7 = (uint8_t)((cur_chord_pcs[3] + 12u - cur_root) % 12u);
            const char *qual = "";
            if (i3 == 3 && i5 == 6) qual = "m7b5";
            else if (i3 == 3 && i7 == 10) qual = "m7";
            else if (i3 == 3) qual = "m";
            else if (i3 == 4 && i7 == 11) qual = "maj7";
            else if (i3 == 4 && i7 == 10) qual = "7";
            str_copy_cat(cur_chord_name, sizeof(cur_chord_name), NOTE_NAMES[cur_root], qual);
        }
    }
    harmony_dirty = 0u;
}

static void reset_harmony(uint8_t hint_root_pc, uint8_t hint_minor)
{
    uint32_t i;
    for (i = 0; i < 12u; i++) {
        pc_energy[i] = 0.0f;
        bass_energy[i] = 0.0f;
    }
    cur_root = (uint8_t)(hint_root_pc % 12u);
    cur_scale_id = hint_minor ? 1u : 0u;
    pc_energy[cur_root] = 1.5f;
    bass_energy[cur_root] = 1.8f;
    pc_energy[(cur_root + (hint_minor ? 3u : 4u)) % 12u] = 0.9f;
    pc_energy[(cur_root + 7u) % 12u] = 1.0f;
    harmony_timer_ms = 0u;
    update_harmony_model();
}

static void feed_harmony(uint8_t pitch, uint8_t vel, uint8_t role)
{
    uint8_t pc = (uint8_t)(pitch % 12u);
    float w = 0.25f + (float)vel * (1.0f / 127.0f);
    if (role == 3u || pitch < 54u)
        bass_energy[pc] += w * 1.45f;
    pc_energy[pc] += w;
    harmony_dirty = 1u;
}

/* Raw LZ4 block decompressor (matches tools/gen_piano_data.py) */
static uint32_t lz4_decompress_raw(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t max_dst)
{
    uint32_t i = 0, o = 0;
    while (i < slen) {
        uint8_t tok = src[i++];
        uint32_t ll = tok >> 4;
        uint32_t ml, off, k;
        if (ll == 15u) {
            uint8_t b;
            do { b = src[i++]; ll += b; } while (b == 255u && i < slen);
        }
        if (i + ll > slen || o + ll > max_dst) break;
        for (k = 0; k < ll; k++) dst[o++] = src[i++];
        if (i >= slen) break;
        off = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8);
        i += 2u;
        if (off == 0u || off > o) break;
        ml = (tok & 15u);
        if (ml == 15u) {
            uint8_t b;
            do { b = src[i++]; ml += b; } while (b == 255u && i < slen);
        }
        ml += 4u;
        if (o + ml > max_dst) ml = max_dst - o;
        for (k = 0; k < ml; k++) {
            dst[o] = dst[o - off];
            o++;
        }
    }
    return o;
}

static void unpack_active_song(uint8_t idx)
{
    if (idx < PIANIST_NBUILTIN) {
        const score_meta_t *m = &SCORE_CATALOG[idx];
        uint32_t nn = m->nnotes;
        uint32_t np = m->npedal;
        uint32_t raw_sz = nn * 4u + np * 4u;
        if (raw_sz <= DECOMP_BUF_SZ) {
            lz4_decompress_raw(&SCORE_BLOB[m->off], m->clen, decomp_buf, raw_sz);
            act_dts = decomp_buf;
            act_durs = decomp_buf + nn;
            act_ps = decomp_buf + nn * 2u;
            act_vrs = decomp_buf + nn * 3u;
            act_ped = (const uint16_t *)(decomp_buf + nn * 4u);
            act_nn = nn;
            act_np = np;
            act_dur_ms = (uint32_t)m->dur_10ms * 10u;
            act_nbars = (uint16_t)clamp((int32_t)(act_dur_ms / 2400u), 4, 240);
            act_is_user = 0;
        }
        reset_harmony(m->tonic, m->minor);
    } else {
        uint8_t u = (uint8_t)(idx - PIANIST_USER_FIRST);
        const user_song_t *s = &user_songs[u];
        act_dts = 0;
        act_durs = 0;
        act_ps = 0;
        act_vrs = 0;
        act_ped = 0;
        act_nn = s->magic == USER_SONG_MAGIC ? s->nnotes : 0u;
        act_np = s->magic == USER_SONG_MAGIC ? s->npedal : 0u;
        act_dur_ms = s->magic == USER_SONG_MAGIC ? (uint32_t)s->duration_10ms * 10u : 0u;
        act_nbars = 16u;
        act_is_user = 1;
        reset_harmony(0u, 0u);
    }
}

static void clear_sched(void)
{
    uint32_t i;
    for (i = 0; i < MAX_SCHED_OFF; i++) sched_off[i].active = 0;
    for (i = 0; i < MAX_SCHED_ON; i++) sched_on[i].active = 0;
}

static void sched_note_off(uint8_t pitch, uint32_t off_sample)
{
    uint32_t i, best = 0;
    uint32_t earliest = 0xFFFFFFFFu;
    for (i = 0; i < MAX_SCHED_OFF; i++) {
        if (sched_off[i].active && sched_off[i].pitch == pitch) {
            sched_off[i].off_sample = off_sample;
            return;
        }
    }
    for (i = 0; i < MAX_SCHED_OFF; i++) {
        if (!sched_off[i].active) { best = i; break; }
        if (sched_off[i].off_sample < earliest) {
            earliest = sched_off[i].off_sample;
            best = i;
        }
    }
    sched_off[best].active = 1;
    sched_off[best].pitch = pitch;
    sched_off[best].off_sample = off_sample;
}

static void sched_note_on(uint8_t pitch, uint8_t vel, uint8_t role, uint8_t soft,
                          uint32_t fire_sample, uint32_t dur_samples)
{
    uint32_t i, best = 0;
    uint32_t earliest = 0xFFFFFFFFu;
    for (i = 0; i < MAX_SCHED_ON; i++) {
        if (!sched_on[i].active) { best = i; break; }
        if (sched_on[i].fire_sample < earliest) {
            earliest = sched_on[i].fire_sample;
            best = i;
        }
    }
    sched_on[best].active = 1;
    sched_on[best].pitch = pitch;
    sched_on[best].vel = vel;
    sched_on[best].role = role;
    sched_on[best].soft = soft;
    sched_on[best].fire_sample = fire_sample;
    sched_on[best].dur_samples = dur_samples;
}

static uint32_t get_note_dt_ms(uint32_t idx)
{
    if (!act_is_user) return (uint32_t)act_dts[idx] * 10u;
    return user_songs[vis.song_idx - PIANIST_USER_FIRST].notes[idx].dt_ms;
}

static void get_score_note(uint32_t idx, uint8_t *pitch, uint8_t *vel, uint8_t *role, uint8_t *soft, uint32_t *dur_ms)
{
    if (!act_is_user) {
        uint8_t dur20 = act_durs[idx];
        uint8_t pb = act_ps[idx];
        uint8_t vrb = act_vrs[idx];
        *pitch = pb & 127u;
        *soft = (pb >> 7) & 1u;
        *role = vrb & 7u;
        *vel = (uint8_t)((vrb >> 3) << 2);
        *dur_ms = (uint32_t)dur20 * 20u;
    } else {
        const score_note_t *n = &user_songs[vis.song_idx - PIANIST_USER_FIRST].notes[idx];
        *pitch = n->pitch;
        *soft = (n->flags >> 3) & 1u;
        *role = n->flags & 7u;
        *vel = n->vel;
        *dur_ms = n->dur_ms;
    }
}

static void reset_playback_pos(void)
{
    clear_sched();
    score_pos_ms = 0.0f;
    vis.note_idx = 0;
    vis.total_notes = act_nn;
    vis.total_bars = act_nbars;
    vis.total_ms = act_dur_ms;
    vis.elapsed_ms = 0;
    vis.bar = 1u;
    ped_idx = 0;
    score_pedal_down = 0;
    next_note_ms = act_nn > 0u ? (float)get_note_dt_ms(0u) : 0.0f;
}

void pianist_init(void)
{
    uint32_t i;
    for (i = 0; i < PIANIST_NUSER; i++) {
        user_songs[i].magic = 0;
        user_songs[i].nnotes = 0;
        user_songs[i].npedal = 0;
        user_songs[i].duration_10ms = 0;
        user_dirty[i] = 0;
    }
    vis.mode = PLAY_STOP;
    vis.song_idx = 0;
    vis.lh_pos = 28.0f;
    vis.rh_pos = 52.0f;
    vis.energy = 0.0f;
    for (i = 0; i < 88u; i++) {
        vis.pianist_key[i] = 0;
        vis.live_key[i] = 0;
    }
    for (i = 0; i < 27u; i++) key_note_cnt[i] = 0;
    mod_lock_mask = 0;
    mod_held_mask = 0;
    for (i = 0; i < 7u; i++) { mod_used_chord[i] = 0; mod_down_ms[i] = 0; }
    jam_clock_ms = 0;
    for (i = 0; i < 128u; i++) rec_open_idx[i] = -1;
    rec_ped_open = -1;
    audio_sample_now = 0;
    live_pedal_down = 0;
    unpack_active_song(0u);
    reset_playback_pos();
}

void pianist_set_song(uint8_t idx)
{
    if (idx >= PIANIST_NSLOTS)
        idx = 0;
    if (vis.mode == PLAY_REC || vis.mode == PLAY_REC_ARMED)
        pianist_rec_toggle();
    vis.mode = PLAY_STOP;
    vis.song_idx = idx;
    dsp_piano_all_off();
    unpack_active_song(idx);
    reset_playback_pos();
}

void pianist_step_in_cat(uint8_t cat, int32_t delta)
{
    int32_t dir = delta >= 0 ? 1 : -1;
    int32_t steps = delta >= 0 ? delta : -delta;
    uint32_t cur = vis.song_idx;
    int32_t s;
    for (s = 0; s < steps; s++) {
        uint32_t tries = 0;
        do {
            cur = (uint32_t)(((int32_t)cur + dir + (int32_t)PIANIST_NSLOTS) % (int32_t)PIANIST_NSLOTS);
            tries++;
            if (cat == CAT_ALL || pianist_song_cat((uint8_t)cur) == cat)
                break;
        } while (tries < PIANIST_NSLOTS);
    }
    pianist_set_song((uint8_t)cur);
}

static void finalize_user_recording(uint8_t u)
{
    user_song_t *s = &user_songs[u];
    uint32_t i, t_abs[USER_MAX_NOTES];
    uint32_t cur_t = 0;

    for (i = 0; i < 128u; i++) {
        if (rec_open_idx[i] >= 0 && (uint16_t)rec_open_idx[i] < s->nnotes) {
            uint32_t dur = (rec_sample - rec_open_sample[i]) * 1000u / 44100u;
            if (dur < 80u) dur = 80u;
            if (dur > 65000u) dur = 65000u;
            s->notes[rec_open_idx[i]].dur_ms = (uint16_t)dur;
            rec_open_idx[i] = -1;
        }
    }
    if (rec_ped_open >= 0 && (uint16_t)rec_ped_open < s->npedal) {
        uint32_t u_10 = (rec_sample * 100u) / 44100u;
        if (u_10 > 65500u) u_10 = 65500u;
        s->pedal[rec_ped_open].up_ds = (uint16_t)u_10;
        rec_ped_open = -1;
    }

    if (s->nnotes == 0u) {
        s->magic = 0;
        s->duration_10ms = 0;
        return;
    }

    s->magic = USER_SONG_MAGIC;
    for (i = 0; i < s->nnotes; i++) {
        cur_t += s->notes[i].dt_ms;
        t_abs[i] = cur_t;
        s->notes[i].bar = (uint8_t)clamp((int32_t)(cur_t / 2000u) + 1, 1, 255);
    }

    i = 0;
    while (i < s->nnotes) {
        uint32_t j = i + 1u;
        uint8_t max_p = s->notes[i].pitch, min_p = s->notes[i].pitch;
        while (j < s->nnotes && t_abs[j] - t_abs[i] <= 45u) {
            if (s->notes[j].pitch > max_p) max_p = s->notes[j].pitch;
            if (s->notes[j].pitch < min_p) min_p = s->notes[j].pitch;
            j++;
        }
        if (j - i == 1u) {
            s->notes[i].flags = s->notes[i].pitch >= 60u ? 0u : 3u;
        } else {
            for (uint32_t k = i; k < j; k++) {
                uint8_t p = s->notes[k].pitch;
                uint8_t role = 5u;
                if (p == max_p && p >= 58u) role = 0u;
                else if (p == min_p && p < 62u) role = 3u;
                s->notes[k].flags = role;
            }
        }
        i = j;
    }

    {
        uint32_t end_ms = cur_t + s->notes[s->nnotes - 1u].dur_ms + 300u;
        uint32_t d10 = end_ms / 10u;
        if (d10 > 65500u) d10 = 65500u;
        s->duration_10ms = (uint16_t)d10;
    }
    user_dirty[u] = 1u;
}

void pianist_play_toggle(void)
{
    uint32_t f = plat_lock();
    if (vis.mode == PLAY_REC || vis.mode == PLAY_REC_ARMED) {
        uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        finalize_user_recording(u);
        vis.mode = PLAY_STOP;
        dsp_piano_all_off();
        unpack_active_song(vis.song_idx);
        reset_playback_pos();
        plat_unlock(f);
        return;
    }
    if (vis.mode == PLAY_RUN) {
        vis.mode = PLAY_STOP;
        dsp_piano_all_off();
        clear_sched();
    } else {
        if (vis.note_idx >= vis.total_notes && vis.total_notes > 0u)
            reset_playback_pos();
        if (vis.total_notes > 0u)
            vis.mode = PLAY_RUN;
    }
    plat_unlock(f);
}

void pianist_stop(void)
{
    uint32_t f = plat_lock();
    if (vis.mode == PLAY_REC || vis.mode == PLAY_REC_ARMED) {
        uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        finalize_user_recording(u);
    }
    vis.mode = PLAY_STOP;
    dsp_piano_all_off();
    unpack_active_song(vis.song_idx);
    reset_playback_pos();
    plat_unlock(f);
}

void pianist_rec_toggle(void)
{
    uint32_t i;
    uint32_t f = plat_lock();
    if (vis.mode == PLAY_REC || vis.mode == PLAY_REC_ARMED) {
        uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        finalize_user_recording(u);
        vis.mode = PLAY_STOP;
        dsp_piano_all_off();
        unpack_active_song(vis.song_idx);
        reset_playback_pos();
    } else {
        uint8_t u;
        if (vis.song_idx < PIANIST_USER_FIRST)
            vis.song_idx = PIANIST_USER_FIRST;
        u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        dsp_piano_all_off();
        clear_sched();
        user_songs[u].magic = 0;
        user_songs[u].nnotes = 0;
        user_songs[u].npedal = 0;
        user_songs[u].duration_10ms = 0;
        for (i = 0; i < 128u; i++) rec_open_idx[i] = -1;
        rec_ped_open = -1;
        rec_sample = 0;
        rec_last_on_sample = 0;
        unpack_active_song(vis.song_idx);
        vis.note_idx = 0;
        vis.total_notes = 0;
        vis.elapsed_ms = 0;
        vis.total_ms = 0;
        vis.bar = 1u;
        vis.mode = PLAY_REC_ARMED;
    }
    plat_unlock(f);
}

void pianist_clear_user_song(uint8_t user_slot)
{
    if (user_slot >= PIANIST_NUSER) return;
    uint32_t f = plat_lock();
    if (vis.song_idx == PIANIST_USER_FIRST + user_slot) {
        vis.mode = PLAY_STOP;
        dsp_piano_all_off();
    }
    user_songs[user_slot].magic = 0;
    user_songs[user_slot].nnotes = 0;
    user_songs[user_slot].npedal = 0;
    user_songs[user_slot].duration_10ms = 0;
    user_dirty[user_slot] = 1u;
    if (vis.song_idx == PIANIST_USER_FIRST + user_slot) {
        unpack_active_song(vis.song_idx);
        reset_playback_pos();
    }
    plat_unlock(f);
}

void pianist_seek_bar(int32_t delta_bars)
{
    uint32_t i, t_acc = 0, target_ms = 0;
    int32_t target_bar;
    uint32_t f = plat_lock();
    if (vis.mode == PLAY_REC || vis.mode == PLAY_REC_ARMED || act_nn == 0u || act_nbars == 0u) {
        plat_unlock(f);
        return;
    }
    target_bar = clamp((int32_t)vis.bar + delta_bars, 1, (int32_t)act_nbars);
    target_ms = (uint32_t)(((uint64_t)(target_bar - 1) * act_dur_ms) / act_nbars);

    dsp_piano_all_off();
    clear_sched();
    for (i = 0; i < act_nn; i++) {
        t_acc += get_note_dt_ms(i);
        if (t_acc >= target_ms) break;
    }
    vis.note_idx = i;
    vis.bar = (uint16_t)target_bar;
    score_pos_ms = (float)target_ms;
    next_note_ms = i < act_nn ? (float)t_acc : (float)act_dur_ms;
    vis.elapsed_ms = target_ms;
    ped_idx = 0;
    if (!act_is_user) {
        while (ped_idx < act_np && (uint32_t)act_ped[ped_idx * 2u + 1u] * 10u <= target_ms)
            ped_idx++;
    } else {
        const user_song_t *us = &user_songs[vis.song_idx - PIANIST_USER_FIRST];
        while (ped_idx < act_np && (uint32_t)us->pedal[ped_idx].up_ds * 10u <= target_ms)
            ped_idx++;
    }
    score_pedal_down = 0;
    plat_unlock(f);
}

static uint8_t apply_voicing(uint8_t base_vel, uint8_t role, uint8_t voicing)
{
    static const int8_t ROFS[7] = {3, -4, -8, -6, -7, -14, -19};
    int32_t ofs = ROFS[role < 7u ? role : 5u];
    float scale = (float)voicing * (1.0f / 75.0f);
    int32_t v = (int32_t)base_vel + (int32_t)((float)ofs * scale);
    if (voicing < 30u) {
        float flat_blend = (float)(30u - voicing) / 30.0f;
        v = (int32_t)((float)v * (1.0f - flat_blend) + 64.0f * flat_blend);
    }
    return (uint8_t)clamp(v, 14, 125);
}

static void record_note_on(uint8_t pitch, uint8_t vel, const pianist_cfg_t *perf)
{
    if (vis.mode == PLAY_REC_ARMED || vis.mode == PLAY_REC) {
        uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        user_song_t *s = &user_songs[u];
        if (vis.mode == PLAY_REC_ARMED) {
            vis.mode = PLAY_REC;
            rec_sample = 0;
            rec_last_on_sample = 0;
        }
        if (s->nnotes < USER_MAX_NOTES) {
            uint32_t dt_ms = (rec_sample - rec_last_on_sample) * 1000u / 44100u;
            uint16_t idx = s->nnotes++;
            if (dt_ms > 65535u) dt_ms = 65535u;
            s->notes[idx].dt_ms = (uint16_t)dt_ms;
            s->notes[idx].dur_ms = 400u;
            s->notes[idx].pitch = pitch;
            s->notes[idx].vel = vel;
            s->notes[idx].flags = perf->soft_pedal ? 8u : 0u;
            s->notes[idx].bar = (uint8_t)clamp((int32_t)((rec_sample * 1000u / 44100u) / 2000u) + 1, 1, 255);
            rec_last_on_sample = rec_sample;
            rec_open_idx[pitch & 127u] = (int16_t)idx;
            rec_open_sample[pitch & 127u] = rec_sample;
            vis.total_notes = s->nnotes;
        }
    }
}

void pianist_live_note_on(uint8_t pitch, uint8_t vel, const piano_cfg_t *pcfg, const pianist_cfg_t *perf)
{
    uint32_t f = plat_lock();
    if (pitch >= PIANO_M_MIN && pitch <= PIANO_M_MAX) {
        vis.live_key[pitch - PIANO_M_MIN] = 255u;
        if (pitch < 60u)
            vis.lh_pos = vis.lh_pos * 0.5f + (float)(pitch - PIANO_M_MIN) * 0.5f;
        else
            vis.rh_pos = vis.rh_pos * 0.5f + (float)(pitch - PIANO_M_MIN) * 0.5f;
        vis.energy = fm_minf(1.0f, vis.energy + 0.25f);
    }

    dsp_piano_note_on(pitch, vel, 0u, perf->soft_pedal, pcfg);
    if (vis.mode != PLAY_RUN)
        feed_harmony(pitch, vel, pitch < 58u ? 3u : 0u);
    record_note_on(pitch, vel, perf);
    plat_unlock(f);
}

void pianist_live_note_off(uint8_t pitch)
{
    uint32_t f = plat_lock();
    dsp_piano_note_off(pitch);
    if (vis.mode == PLAY_REC) {
        uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        user_song_t *s = &user_songs[u];
        int16_t idx = rec_open_idx[pitch & 127u];
        if (idx >= 0 && (uint16_t)idx < s->nnotes) {
            uint32_t dur = (rec_sample - rec_open_sample[pitch & 127u]) * 1000u / 44100u;
            if (dur < 80u) dur = 80u;
            if (dur > 65535u) dur = 65535u;
            s->notes[idx].dur_ms = (uint16_t)dur;
            rec_open_idx[pitch & 127u] = -1;
        }
    }
    plat_unlock(f);
}

void pianist_live_pedal(uint8_t down, const pianist_cfg_t *perf)
{
    uint32_t f = plat_lock();
    live_pedal_down = down ? 1u : 0u;
    if (vis.mode != PLAY_RUN || perf->pedal_mode == PEDAL_DRY) {
        dsp_piano_set_pedal(live_pedal_down);
        vis.pedal_down = live_pedal_down;
    }
    if (vis.mode == PLAY_REC) {
        uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
        user_song_t *s = &user_songs[u];
        uint32_t t10 = (rec_sample * 100u) / 44100u;
        if (t10 > 65500u) t10 = 65500u;
        if (down && rec_ped_open < 0 && s->npedal < USER_MAX_PEDAL) {
            rec_ped_open = (int16_t)(s->npedal++);
            s->pedal[rec_ped_open].down_ds = (uint16_t)t10;
            s->pedal[rec_ped_open].up_ds = (uint16_t)(t10 + 200u);
        } else if (!down && rec_ped_open >= 0) {
            s->pedal[rec_ped_open].up_ds = (uint16_t)t10;
            rec_ped_open = -1;
        }
    }
    plat_unlock(f);
}

/* Build real FM Grand Piano chord pitches for KEYMD_CHORD */
static uint8_t build_chord_pitches(uint8_t chord_key, int8_t oct_shift, uint8_t mods, uint8_t spread, uint8_t out[8])
{
    int root = 53 + (int)chord_key + (int)oct_shift * 12;
    int third_semi = 4, fifth_semi = 7, seventh_semi = 11;
    uint8_t cnt = 0;

    while (root < 36) root += 12;
    while (root > 76) root -= 12;

    /* Natural diatonic 3rd/5th/7th quality from active harmony mask */
    {
        uint8_t r_pc = (uint8_t)(root % 12);
        uint8_t has_m3 = (cur_scale_mask & (1u << ((r_pc + 3u) % 12u))) != 0;
        uint8_t has_M3 = (cur_scale_mask & (1u << ((r_pc + 4u) % 12u))) != 0;
        uint8_t has_d5 = (cur_scale_mask & (1u << ((r_pc + 6u) % 12u))) != 0;
        uint8_t has_p5 = (cur_scale_mask & (1u << ((r_pc + 7u) % 12u))) != 0;
        uint8_t has_M7 = (cur_scale_mask & (1u << ((r_pc + 11u) % 12u))) != 0;

        third_semi = (has_m3 && !has_M3) ? 3 : 4;
        fifth_semi = (has_d5 && !has_p5) ? 6 : 7;
        seventh_semi = has_M7 ? 11 : 10;
    }

    if (mods & CMOD_DIM) {
        third_semi = 3;
        fifth_semi = 6;
        seventh_semi = 9;
    }
    if (mods & CMOD_SUS2) third_semi = 2;
    if (mods & CMOD_SUS4) third_semi = 5;

    out[cnt++] = (uint8_t)root;
    if (mods & CMOD_5TH) {
        out[cnt++] = (uint8_t)(root + fifth_semi);
        out[cnt++] = (uint8_t)(root + 12);
    } else if (spread == SPREAD_OPEN) {
        out[cnt++] = (uint8_t)(root + fifth_semi);
        out[cnt++] = (uint8_t)(root + 12 + third_semi);
    } else {
        out[cnt++] = (uint8_t)(root + third_semi);
        out[cnt++] = (uint8_t)(root + fifth_semi);
    }
    if ((mods & CMOD_6TH) && cnt < 6u) out[cnt++] = (uint8_t)(root + 9);
    if ((mods & CMOD_7TH) && cnt < 6u) out[cnt++] = (uint8_t)(root + seventh_semi);
    if ((mods & CMOD_9TH) && cnt < 6u) out[cnt++] = (uint8_t)(root + 14);
    return cnt;
}

static void update_score_pedal_at(float pos_ms, const pianist_cfg_t *perf)
{
    if (perf->pedal_mode == PEDAL_DRY) {
        if (score_pedal_down && !live_pedal_down) {
            score_pedal_down = 0;
            dsp_piano_set_pedal(live_pedal_down);
            vis.pedal_down = live_pedal_down;
        }
        return;
    }
    if (act_np > 0u) {
        uint8_t want_ped = 0;
        uint32_t pos_10ms = (uint32_t)(pos_ms * 0.1f);
        if (!act_is_user) {
            while (ped_idx + 1u < act_np && pos_10ms >= act_ped[ped_idx * 2u + 1u])
                ped_idx++;
            if (ped_idx < act_np) {
                uint32_t d = act_ped[ped_idx * 2u];
                uint32_t u = act_ped[ped_idx * 2u + 1u];
                if (perf->pedal_mode == PEDAL_HALF && u > d + 25u) u -= 22u;
                else if (perf->pedal_mode == PEDAL_LUSH) u += 14u;
                if (pos_10ms >= d && pos_10ms < u) want_ped = 1u;
            }
        } else {
            const user_song_t *us = &user_songs[vis.song_idx - PIANIST_USER_FIRST];
            while (ped_idx + 1u < act_np && pos_10ms >= us->pedal[ped_idx].up_ds)
                ped_idx++;
            if (ped_idx < act_np && pos_10ms >= us->pedal[ped_idx].down_ds && pos_10ms < us->pedal[ped_idx].up_ds)
                want_ped = 1u;
        }
        want_ped |= live_pedal_down;
        if (want_ped != score_pedal_down) {
            score_pedal_down = want_ped;
            dsp_piano_set_pedal(score_pedal_down);
            vis.pedal_down = score_pedal_down;
        }
    } else {
        uint32_t cyc = ((uint32_t)pos_ms) % 2400u;
        uint8_t want_ped = (cyc < 2220u ? 1u : 0u) | live_pedal_down;
        if (want_ped != score_pedal_down) {
            score_pedal_down = want_ped;
            dsp_piano_set_pedal(score_pedal_down);
            vis.pedal_down = score_pedal_down;
        }
    }
}

/* Tap Pianist: advance & perform the next chord/cluster in the loaded score at the player's tap timing & velocity! */
static void tap_pianist_advance(uint8_t key_idx, uint8_t vel, const piano_cfg_t *pcfg, const pianist_cfg_t *perf)
{
    uint8_t fired = 0;
    float tap_scale = 0.35f + (float)vel * (0.95f / 127.0f);

    if (act_nn == 0u)
        return;

    /* Pause auto-play if running so the player takes over seamlessly */
    if (vis.mode == PLAY_RUN) {
        vis.mode = PLAY_STOP;
        clear_sched();
    }

    if (vis.note_idx >= act_nn)
        reset_playback_pos();

    /* Skip leading silent rest chunks */
    while (vis.note_idx < act_nn) {
        uint8_t p, v, r, s;
        uint32_t dm;
        get_score_note(vis.note_idx, &p, &v, &r, &s, &dm);
        if (dm > 0u) break;
        score_pos_ms += (float)get_note_dt_ms(vis.note_idx);
        vis.note_idx++;
    }
    if (vis.note_idx >= act_nn) {
        reset_playback_pos();
        while (vis.note_idx < act_nn) {
            uint8_t p, v, r, s;
            uint32_t dm;
            get_score_note(vis.note_idx, &p, &v, &r, &s, &dm);
            if (dm > 0u) break;
            score_pos_ms += (float)get_note_dt_ms(vis.note_idx);
            vis.note_idx++;
        }
    }

    key_note_cnt[key_idx] = 0u;
    if (vis.note_idx < act_nn) {
        score_pos_ms = next_note_ms;
        vis.elapsed_ms = (uint32_t)score_pos_ms;
        if (act_dur_ms > 0u && act_nbars > 0u) {
            uint32_t b = 1u + (vis.elapsed_ms * act_nbars) / act_dur_ms;
            vis.bar = (uint16_t)(b > act_nbars ? act_nbars : b);
        }
        update_score_pedal_at(score_pos_ms, perf);
    }

    while (vis.note_idx < act_nn && fired < 8u) {
        uint8_t pitch, base_vel, role, soft_f;
        uint32_t n_dur_ms;
        uint32_t dt_next = 0u;

        if (fired > 0u) {
            dt_next = get_note_dt_ms(vis.note_idx);
            if (dt_next > 42u)
                break;
            score_pos_ms += (float)dt_next;
        }

        get_score_note(vis.note_idx, &pitch, &base_vel, &role, &soft_f, &n_dur_ms);
        if (n_dur_ms > 0u) {
            uint8_t soft = soft_f | perf->soft_pedal;
            int32_t sv = (int32_t)((float)base_vel * tap_scale);
            uint8_t eff_vel = apply_voicing((uint8_t)clamp(sv, 18, 125), role, perf->voicing);

            if (fired == 0u || dt_next < 12u) {
                dsp_piano_note_on(pitch, eff_vel, role, soft, pcfg);
            } else {
                uint32_t delay_samp = (uint32_t)(fired * 9u * 44.1f);
                uint32_t dur_samp = (uint32_t)(n_dur_ms * 44.1f);
                if (dur_samp < 3000u) dur_samp = 3000u;
                sched_note_on(pitch, eff_vel, role, soft, audio_sample_now + delay_samp, dur_samp);
            }
            feed_harmony(pitch, eff_vel, role);
            key_notes[key_idx][key_note_cnt[key_idx]++] = pitch;

            if (pitch >= PIANO_M_MIN && pitch <= PIANO_M_MAX) {
                uint8_t ki = (uint8_t)(pitch - PIANO_M_MIN);
                vis.pianist_key[ki] = 255u;
                if (role == 3u || pitch < 56u)
                    vis.lh_pos = vis.lh_pos * 0.55f + (float)ki * 0.45f;
                else
                    vis.rh_pos = vis.rh_pos * 0.55f + (float)ki * 0.45f;
                vis.energy = fm_minf(1.0f, vis.energy + (role == 0u ? 0.24f : 0.12f));
            }
            fired++;
        }

        vis.note_idx++;
        if (vis.note_idx < act_nn)
            next_note_ms = score_pos_ms + (float)get_note_dt_ms(vis.note_idx);
        else
            next_note_ms = (float)act_dur_ms;
    }
}

void pianist_key_event(uint8_t key_idx, uint8_t down, uint8_t vel, int8_t oct_shift,
                       const piano_cfg_t *pcfg, const pianist_cfg_t *perf, const keys_cfg_t *kcfg)
{
    uint32_t f;
    if (key_idx >= 27u) return;

    f = plat_lock();

    /* In KEYMD_CHORD, keys 0..6 (F3..B3) are Chord Modifiers */
    if (kcfg->key_mode == KEYMD_CHORD && key_idx < 7u) {
        uint8_t bit = (uint8_t)(1u << key_idx);
        if (down) {
            if (kcfg->mod_mode == MOD_LOCK) {
                mod_lock_mask ^= bit;
            } else {
                mod_held_mask |= bit;
                mod_used_chord[key_idx] = 0;
                mod_down_ms[key_idx] = jam_clock_ms;
            }
        } else {
            if (kcfg->mod_mode == MOD_AUTO) {
                uint32_t held_dur = jam_clock_ms - mod_down_ms[key_idx];
                mod_held_mask &= (uint8_t)~bit;
                if (!mod_used_chord[key_idx] && held_dur < 360u)
                    mod_lock_mask ^= bit;
            } else if (kcfg->mod_mode == MOD_HOLD) {
                mod_held_mask &= (uint8_t)~bit;
            }
        }
        plat_unlock(f);
        return;
    }

    if (!down) {
        uint8_t i;
        for (i = 0; i < key_note_cnt[key_idx]; i++) {
            uint8_t p = key_notes[key_idx][i];
            dsp_piano_note_off(p);
            if (vis.mode == PLAY_REC) {
                uint8_t u = (uint8_t)(vis.song_idx - PIANIST_USER_FIRST);
                user_song_t *s = &user_songs[u];
                int16_t idx = rec_open_idx[p & 127u];
                if (idx >= 0 && (uint16_t)idx < s->nnotes) {
                    uint32_t dur = (rec_sample - rec_open_sample[p & 127u]) * 1000u / 44100u;
                    if (dur < 80u) dur = 80u;
                    if (dur > 65535u) dur = 65535u;
                    s->notes[idx].dur_ms = (uint16_t)dur;
                    rec_open_idx[p & 127u] = -1;
                }
            }
            if (kcfg->key_mode != KEYMD_TAP)
                plat_midi_send(0x08u | (0x80u << 8) | ((uint32_t)p << 16), TO_USB | TO_BLE);
        }
        key_note_cnt[key_idx] = 0u;
        plat_unlock(f);
        return;
    }

    if (kcfg->key_mode == KEYMD_TAP) {
        tap_pianist_advance(key_idx, vel, pcfg, perf);
        plat_unlock(f);
        return;
    }

    if (kcfg->key_mode == KEYMD_CHORD) {
        uint8_t notes[8], cnt, i;
        uint8_t mods = (uint8_t)(mod_lock_mask | mod_held_mask);
        for (i = 0; i < 7u; i++) {
            if (mod_held_mask & (1u << i))
                mod_used_chord[i] = 1u;
        }
        cnt = build_chord_pitches(key_idx, oct_shift, mods, kcfg->spread, notes);
        key_note_cnt[key_idx] = 0u;
        for (i = 0; i < cnt && i < 8u; i++) {
            uint8_t p = notes[i];
            uint8_t nv = (i == 0u) ? vel : (uint8_t)((vel * (i == cnt - 1u ? 84u : 74u)) / 100u);
            if (kcfg->spread == SPREAD_ROLL && i > 0u) {
                uint32_t fire = audio_sample_now + (uint32_t)(i * 22u * 44.1f);
                sched_note_on(p, nv, i == 0u ? 3u : 5u, perf->soft_pedal, fire, 35280u);
            } else {
                dsp_piano_note_on(p, nv, i == 0u ? 3u : 5u, perf->soft_pedal, pcfg);
                if (p >= PIANO_M_MIN && p <= PIANO_M_MAX)
                    vis.live_key[p - PIANO_M_MIN] = 255u;
            }
            key_notes[key_idx][key_note_cnt[key_idx]++] = p;
            record_note_on(p, nv, perf);
            plat_midi_send(0x09u | (0x90u << 8) | ((uint32_t)p << 16) | ((uint32_t)nv << 24), TO_USB | TO_BLE);
        }
        vis.energy = fm_minf(1.0f, vis.energy + 0.28f);
        plat_unlock(f);
        return;
    }

    /* KEYMD_PIANO: Pure 27-key calibrated FM Grand Piano */
    {
        int32_t note = 53 + (int32_t)key_idx + 12 * oct_shift;
        if (note >= PIANO_M_MIN && note <= PIANO_M_MAX) {
            uint8_t p = (uint8_t)note;
            vis.live_key[p - PIANO_M_MIN] = 255u;
            if (p < 60u)
                vis.lh_pos = vis.lh_pos * 0.5f + (float)(p - PIANO_M_MIN) * 0.5f;
            else
                vis.rh_pos = vis.rh_pos * 0.5f + (float)(p - PIANO_M_MIN) * 0.5f;
            vis.energy = fm_minf(1.0f, vis.energy + 0.25f);
            dsp_piano_note_on(p, vel, 0u, perf->soft_pedal, pcfg);
            if (vis.mode != PLAY_RUN)
                feed_harmony(p, vel, p < 58u ? 3u : 0u);
            key_notes[key_idx][0] = p;
            key_note_cnt[key_idx] = 1u;
            record_note_on(p, vel, perf);
            plat_midi_send(0x09u | (0x90u << 8) | ((uint32_t)p << 16) | ((uint32_t)vel << 24), TO_USB | TO_BLE);
        }
    }
    plat_unlock(f);
}

void pianist_clear_mods(void)
{
    mod_lock_mask = 0;
    mod_held_mask = 0;
}

void pianist_tick_block(uint32_t n_frames, const pianist_cfg_t *perf, const piano_cfg_t *pcfg)
{
    uint32_t i;
    float block_ms = (float)n_frames * (1000.0f / 44100.0f);
    uint32_t dt_ms = (uint32_t)block_ms;
    if (dt_ms == 0u) dt_ms = 5u;

    audio_sample_now += n_frames;
    jam_clock_ms += dt_ms;
    harmony_timer_ms += dt_ms;
    {
        float dec = fm_expf(-(float)dt_ms * 0.0011f);
        float bdec = fm_expf(-(float)dt_ms * 0.0015f);
        for (i = 0; i < 12u; i++) {
            pc_energy[i] *= dec;
            bass_energy[i] *= bdec;
        }
    }
    if (harmony_dirty && harmony_timer_ms >= 45u) {
        harmony_timer_ms = 0u;
        update_harmony_model();
    }

    live_vis_decay += n_frames;
    if (live_vis_decay >= 1024u) {
        live_vis_decay = 0;
        for (i = 0; i < 88u; i++) {
            if (vis.pianist_key[i] > 18u) vis.pianist_key[i] -= 18u; else vis.pianist_key[i] = 0;
            if (vis.live_key[i] > 18u) vis.live_key[i] -= 18u; else vis.live_key[i] = 0;
        }
        vis.energy *= 0.90f;
    }

    if (vis.mode == PLAY_REC) {
        rec_sample += n_frames;
        vis.elapsed_ms = (rec_sample * 1000u) / 44100u;
    }

    for (i = 0; i < MAX_SCHED_ON; i++) {
        if (sched_on[i].active && (int32_t)(audio_sample_now - sched_on[i].fire_sample) >= 0) {
            sched_on_t *so = &sched_on[i];
            dsp_piano_note_on(so->pitch, so->vel, so->role, so->soft, pcfg);
            feed_harmony(so->pitch, so->vel, so->role);
            sched_note_off(so->pitch, audio_sample_now + so->dur_samples);
            if (so->pitch >= PIANO_M_MIN && so->pitch <= PIANO_M_MAX) {
                uint8_t ki = (uint8_t)(so->pitch - PIANO_M_MIN);
                vis.pianist_key[ki] = 255u;
                if (so->role == 3u || so->pitch < 56u)
                    vis.lh_pos = vis.lh_pos * 0.6f + (float)ki * 0.4f;
                else
                    vis.rh_pos = vis.rh_pos * 0.6f + (float)ki * 0.4f;
                vis.energy = fm_minf(1.0f, vis.energy + (so->role == 0u ? 0.22f : 0.10f));
            }
            so->active = 0;
        }
    }

    for (i = 0; i < MAX_SCHED_OFF; i++) {
        if (sched_off[i].active && (int32_t)(audio_sample_now - sched_off[i].off_sample) >= 0) {
            dsp_piano_note_off(sched_off[i].pitch);
            sched_off[i].active = 0;
        }
    }

    if (vis.mode != PLAY_RUN)
        return;

    if (act_nn == 0u) {
        vis.mode = PLAY_STOP;
        return;
    }

    {
        float t_sec = score_pos_ms * 0.001f;
        float rub = (float)perf->rubato * 0.01f;
        float drift = 0.055f * rub * (0.65f * fm_sinf(t_sec * 0.43f) + 0.35f * fm_sinf(t_sec * 0.19f + 1.1f));
        float tempo_mul = fm_clampf(((float)perf->tempo_pct * 0.01f) * (1.0f + drift), 0.35f, 1.85f);
        score_pos_ms += block_ms * tempo_mul;
        vis.elapsed_ms = (uint32_t)score_pos_ms;
    }

    if (act_dur_ms > 0u && act_nbars > 0u) {
        uint32_t b = 1u + (vis.elapsed_ms * act_nbars) / act_dur_ms;
        if (b > act_nbars) b = act_nbars;
        vis.bar = (uint16_t)b;
    }

    update_score_pedal_at(score_pos_ms, perf);

    while (vis.note_idx < act_nn && score_pos_ms >= next_note_ms) {
        uint8_t pitch, base_vel, role, soft_f;
        uint32_t n_dur_ms;

        get_score_note(vis.note_idx, &pitch, &base_vel, &role, &soft_f, &n_dur_ms);
        if (n_dur_ms == 0u) {
            vis.note_idx++;
            if (vis.note_idx < act_nn) next_note_ms += (float)get_note_dt_ms(vis.note_idx);
            continue;
        }

        {
            uint8_t soft = soft_f | perf->soft_pedal;
            uint8_t vel = apply_voicing(base_vel, role, perf->voicing);
            float rub = (float)perf->rubato * 0.01f;
            float delay_ms = 0.0f;
            float tempo_scale = 100.0f / (float)fm_maxf(40.0f, (float)perf->tempo_pct);
            uint32_t dur_samp = (uint32_t)((float)n_dur_ms * tempo_scale * 44.1f);
            if (dur_samp < 2200u) dur_samp = 2200u;

            if (rub > 0.01f) {
                if (role == 3u) delay_ms = 0.0f;
                else if (role == 0u) delay_ms = 12.0f * rub;
                else delay_ms = 5.0f * rub;
                delay_ms += 6.0f * rub * (rand_sym() + 1.0f) * 0.5f;
            }
            vis.soft_on = soft;

            if (delay_ms < 2.5f) {
                dsp_piano_note_on(pitch, vel, role, soft, pcfg);
                feed_harmony(pitch, vel, role);
                sched_note_off(pitch, audio_sample_now + dur_samp);
                if (pitch >= PIANO_M_MIN && pitch <= PIANO_M_MAX) {
                    uint8_t ki = (uint8_t)(pitch - PIANO_M_MIN);
                    vis.pianist_key[ki] = 255u;
                    if (role == 3u || pitch < 56u)
                        vis.lh_pos = vis.lh_pos * 0.6f + (float)ki * 0.4f;
                    else
                        vis.rh_pos = vis.rh_pos * 0.6f + (float)ki * 0.4f;
                    vis.energy = fm_minf(1.0f, vis.energy + (role == 0u ? 0.22f : 0.10f));
                }
            } else {
                sched_note_on(pitch, vel, role, soft, audio_sample_now + (uint32_t)(delay_ms * 44.1f), dur_samp);
            }
        }

        vis.note_idx++;
        if (vis.note_idx < act_nn)
            next_note_ms += (float)get_note_dt_ms(vis.note_idx);
    }

    if (vis.note_idx >= act_nn && score_pos_ms > (float)act_dur_ms + 1200.0f) {
        vis.mode = PLAY_STOP;
        dsp_piano_all_off();
        reset_playback_pos();
    }
}

const char *pianist_song_name(uint8_t idx)
{
    if (idx < PIANIST_NBUILTIN) return SCORE_CATALOG[idx].title;
    idx -= PIANIST_USER_FIRST;
    return idx < PIANIST_NUSER ? USER_NAMES[idx] : USER_NAMES[0];
}

const char *pianist_composer_name(uint8_t idx)
{
    if (idx < PIANIST_NBUILTIN) return SCORE_CATALOG[idx].composer;
    return "USER RECORDING";
}

const char *pianist_cat_name(uint8_t cat)
{
    return cat < CAT_COUNT ? CAT_NAMES[cat] : CAT_NAMES[0];
}

uint8_t pianist_song_cat(uint8_t idx)
{
    if (idx < PIANIST_NBUILTIN) return SCORE_CATALOG[idx].cat;
    return CAT_USER;
}

const char *pianist_chord_badge(void) { return cur_chord_name; }

void pianist_mod_label(char *out, uint32_t cap)
{
    uint8_t m = (uint8_t)(mod_lock_mask | mod_held_mask);
    out[0] = '\0';
    if (m & CMOD_5TH)  append_tok(out, cap, "5");
    if (m & CMOD_SUS2) append_tok(out, cap, "s2");
    if (m & CMOD_SUS4) append_tok(out, cap, "s4");
    if (m & CMOD_DIM)  append_tok(out, cap, "dim");
    if (m & CMOD_6TH)  append_tok(out, cap, "6");
    if (m & CMOD_7TH)  append_tok(out, cap, "7");
    if (m & CMOD_9TH)  append_tok(out, cap, "9");
    if (out[0] == '\0') str_copy_cat(out, cap, "TRIAD", "");
}

uint8_t pianist_active_mod_mask(void) { return (uint8_t)(mod_lock_mask | mod_held_mask); }

uint8_t pianist_key_guide_bright(uint8_t key_idx, int8_t oct_shift, const keys_cfg_t *kcfg)
{
    if (key_idx >= 27u) return 0u;
    if (kcfg->key_mode == KEYMD_CHORD && key_idx < 7u) {
        uint8_t bit = (uint8_t)(1u << key_idx);
        if ((mod_lock_mask | mod_held_mask) & bit)
            return 255u;
        return 42u;
    }
    if (kcfg->guide_led == 0u || kcfg->key_mode == KEYMD_TAP) return 0u;
    {
        uint8_t pc = (uint8_t)((53u + key_idx + (uint32_t)(oct_shift + 4) * 12u) % 12u);
        uint32_t i;
        if (pc == cur_root) return 140u;
        for (i = 0; i < 4u; i++) {
            if (pc == cur_chord_pcs[i]) return 88u;
        }
        if (kcfg->guide_led == 1u && (cur_scale_mask & (1u << pc)))
            return 26u;
    }
    return 0u;
}

uint8_t pianist_user_dirty(uint8_t slot) { return slot < PIANIST_NUSER ? user_dirty[slot] : 0u; }
void pianist_clear_dirty(uint8_t slot) { if (slot < PIANIST_NUSER) user_dirty[slot] = 0u; }
user_song_t *pianist_user_buf(uint8_t slot) { return slot < PIANIST_NUSER ? &user_songs[slot] : &user_songs[0]; }
const pianist_vis_t *pianist_vis(void) { return &vis; }
