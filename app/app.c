/* SPDX-License-Identifier: MPL-2.0 */
/* PIANOLA — Tap-Played Physical-Modal Grand Piano, 87-Roll Classical Library,
 * Basic Piano & Chord Mode for M-VAVE FM-1.
 */
#include "fastmath.h"
#include "dsp_piano.h"
#include "pianist.h"

#define SET_MAGIC 0x504E4C31u /* "PNL1" */

enum { PAGE_PIANIST = 0, PAGE_BODY = 1, PAGE_HALL = 2, PAGE_KEYS = 3, PAGE_COUNT = 4 };
enum { SCENE_PIANIST = 0, SCENE_GRAND = 1, SCENE_TAPE = 2, SCENE_COUNT = 3 };

typedef struct {
    uint32_t magic;
    int8_t octave;
    uint8_t song_idx;
    uint8_t cat;
    uint8_t page;
    uint8_t scene;
    uint8_t theme;
    uint8_t ble_off;
    pianist_cfg_t perf;
    piano_cfg_t piano;
    keys_cfg_t keys;
} settings_t;

static settings_t set;
static uint8_t set_dirty;

/* OP-1 signature colors */
#define OP_CORAL  RGB(232, 93, 74)
#define OP_MINT   RGB(88, 196, 114)
#define OP_CYAN   RGB(78, 168, 222)
#define OP_AMBER  RGB(244, 185, 66)
#define OP_CREAM  RGB(242, 238, 228)
#define OP_INK    RGB(24, 26, 32)

static const char *const SCENE_NAMES[SCENE_COUNT]  = {"STAGE", "HARP", "TAPE"};
static const char *const PEDAL_NAMES[PEDAL_NMODES] = {"DRY", "HALF", "AUTO", "LUSH"};
static const char *const LID_NAMES[4]              = {"CLOSE", "HALF", "OPEN", "CONCT"};
static const char *const BODY_NAMES[4]             = {"UPRGT", "BABY", "SALON", "CONCT"};
static const char *const STR_NAMES[4]              = {"PURE", "AGED", "HONKY", "FELT"};
static const char *const ACT_NAMES[4]              = {"LIGHT", "MED", "HEAVY", "NOISY"};
static const char *const ROOM_NAMES[4]             = {"PARLR", "RECIT", "HALL", "CATHD"};
static const char *const SEAT_NAMES[4]             = {"BENCH", "FRONT", "BALCN", "BACK"};
static const char *const KEYMD_NAMES[KEYMD_COUNT]  = {"PIANO", "CHORD", "PIANOLA"};
static const char *const MOD_NAMES[MOD_MODE_COUNT] = {"AUTO", "LOCK", "HOLD"};
static const char *const SPREAD_NAMES[SPREAD_COUNT]= {"CLOSE", "OPEN", "ROLL"};
static const char *const GUIDE_NAMES[3]            = {"OFF", "SCALE", "CHORD"};

static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint8_t arp_pedal;
    uint32_t edit_t;
    uint32_t rec_hold_t;
    uint32_t glo_t;
    uint32_t lfo_t;
    uint32_t arp_t;
    uint32_t turned_at[NE];
    uint32_t last_draw_ms;
    uint32_t anim_tick;
    uint8_t redraw;
    uint8_t looking;
} ui;

#define N_SPARK 8u
typedef struct {
    int16_t x, y;
    int8_t vy;
    uint8_t life;
    uint16_t col;
} spark_t;
static spark_t sparks[N_SPARK];
static uint32_t spark_rng = 0x51A9u;

static void text_cat(char *d, const char *s)
{
    while (*d) d++;
    while (*s) *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0) *b++ = '-';
    while (n) *b++ = t[--n];
    *b = 0;
}

static void fmt_mmss(uint32_t ms, char *out)
{
    uint32_t s = ms / 1000u;
    uint32_t m = s / 60u;
    s %= 60u;
    out[0] = (char)('0' + (m / 10u) % 10u);
    out[1] = (char)('0' + m % 10u);
    out[2] = ':';
    out[3] = (char)('0' + s / 10u);
    out[4] = (char)('0' + s % 10u);
    out[5] = 0;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++) ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++) ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1200u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

int clair_ble_off(void) { return set.ble_off; }

/* ----------------------------------------------------------------- MIDI --- */
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu;
        uint32_t d2 = (pkt >> 24) & 0x7Fu;
        (void)port;
        if ((st & 0xF0u) == 0x90u && d2 > 0u) {
            pianist_live_note_on((uint8_t)d1, (uint8_t)d2, &set.piano, &set.perf);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            pianist_live_note_off((uint8_t)d1);
        } else if ((st & 0xF0u) == 0xB0u && d1 == 64u) {
            pianist_live_pedal(d2 >= 64u ? 1u : 0u, &set.perf);
        } else if ((st & 0xF0u) == 0xB0u && (d1 == 120u || d1 == 123u)) {
            dsp_piano_all_off();
        }
    }
}

/* ---------------------------------------------------------------- Audio --- */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    midi_in();
    pianist_tick_block(n, &set.perf, &set.piano);
    dsp_piano_render(lr, n, master_q12, &set.piano);
}

void app_panic(void)
{
    dsp_piano_panic();
}

void app_overload(void)
{
    dsp_piano_shed();
}

/* ------------------------------------------------------------- Settings --- */
static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.song_idx = 0;
    set.cat = CAT_ALL;
    set.page = PAGE_PIANIST;
    set.scene = SCENE_PIANIST;
    set.theme = 0;
    set.ble_off = 0;
    set.perf.tempo_pct = 100;
    set.perf.rubato = 65;
    set.perf.voicing = 75;
    set.perf.pedal_mode = PEDAL_AUTO;
    set.perf.soft_pedal = 0;
    set.piano.lid = LID_OPEN;
    set.piano.body = BODY_SALON;
    set.piano.str = STR_AGED;
    set.piano.action = ACT_MEDIUM;
    set.piano.room = ROOM_HALL;
    set.piano.seat = SEAT_FRONT;
    set.keys.key_mode = KEYMD_TAP;
    set.keys.mod_mode = MOD_AUTO;
    set.keys.spread = SPREAD_OPEN;
    set.keys.guide_led = 2; /* CHORD */
}

void app_init(void)
{
    uint32_t i;
    dsp_piano_init();
    pianist_init();
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC) {
        settings_defaults();
    } else {
        set.octave = (int8_t)clamp(set.octave, -2, 2);
        if (set.song_idx >= PIANIST_NSLOTS) set.song_idx = 0;
        if (set.cat >= CAT_COUNT) set.cat = CAT_ALL;
        if (set.page >= PAGE_COUNT) set.page = PAGE_PIANIST;
        if (set.scene >= SCENE_COUNT) set.scene = SCENE_PIANIST;
        if (set.theme > 1u) set.theme = 0;
        if (set.perf.tempo_pct < 50u || set.perf.tempo_pct > 150u) set.perf.tempo_pct = 100;
        if (set.perf.rubato > 100u) set.perf.rubato = 65;
        if (set.perf.voicing > 100u) set.perf.voicing = 75;
        if (set.perf.pedal_mode >= PEDAL_NMODES) set.perf.pedal_mode = PEDAL_AUTO;
        set.piano.lid &= 3u;
        set.piano.body &= 3u;
        set.piano.str &= 3u;
        set.piano.action &= 3u;
        set.piano.room &= 3u;
        set.piano.seat &= 3u;
        if (set.keys.key_mode >= KEYMD_COUNT) set.keys.key_mode = KEYMD_TAP;
        if (set.keys.mod_mode >= MOD_MODE_COUNT) set.keys.mod_mode = MOD_AUTO;
        if (set.keys.spread >= SPREAD_COUNT) set.keys.spread = SPREAD_OPEN;
        if (set.keys.guide_led > 2u) set.keys.guide_led = 2u;
    }

    for (i = 0; i < PIANIST_NUSER; i++) {
        user_song_t *ub = pianist_user_buf((uint8_t)i);
        if (plat_store_load(OBJ_SONG0 + i, ub, sizeof *ub) != (int)sizeof *ub || ub->magic != USER_SONG_MAGIC) {
            ub->magic = 0;
            ub->nnotes = 0;
            ub->npedal = 0;
        }
    }
    pianist_set_song(set.song_idx);
    (void)plat_cpu2_set(1);
}

static void save_all(void)
{
    uint32_t i;
    int rc = plat_store_save(OBJ_SETTINGS, &set, sizeof set);
    for (i = 0; i < PIANIST_NUSER; i++) {
        if (pianist_user_dirty((uint8_t)i)) {
            if (plat_store_save(OBJ_SONG0 + i, pianist_user_buf((uint8_t)i), sizeof(user_song_t)) == 0)
                pianist_clear_dirty((uint8_t)i);
            else
                rc = -1;
        }
    }
    if (rc == 0) {
        set_dirty = 0;
        app_say("SAVED", "");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static uint8_t any_dirty(void)
{
    uint32_t i;
    if (set_dirty) return 1u;
    for (i = 0; i < PIANIST_NUSER; i++)
        if (pianist_user_dirty((uint8_t)i)) return 1u;
    return 0u;
}

/* ---------------------------------------------------------------- Input --- */
static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave) return;
    set.octave = (int8_t)o;
    set_dirty = 1;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
}

static void adjust_knob(uint32_t k, int32_t e)
{
    char vbuf[16];
    int32_t step = e * 4;
    int32_t dir = e > 0 ? 1 : -1;

    if (set.page == PAGE_PIANIST) {
        if (k == EN_K1) {
            set.perf.tempo_pct = (uint8_t)clamp((int32_t)set.perf.tempo_pct + e * 2, 50, 150);
            int_text(set.perf.tempo_pct, vbuf); text_cat(vbuf, "%");
            app_say("TEMPO", vbuf);
        } else if (k == EN_K2) {
            set.perf.rubato = (uint8_t)clamp((int32_t)set.perf.rubato + step, 0, 100);
            int_text(set.perf.rubato, vbuf); text_cat(vbuf, "%");
            app_say("RUBATO", vbuf);
        } else if (k == EN_K3) {
            set.perf.voicing = (uint8_t)clamp((int32_t)set.perf.voicing + step, 0, 100);
            int_text(set.perf.voicing, vbuf); text_cat(vbuf, "%");
            app_say("VOICING", vbuf);
        } else if (k == EN_K4) {
            set.perf.pedal_mode = (uint8_t)clamp((int32_t)set.perf.pedal_mode + dir, 0, PEDAL_NMODES - 1);
            app_say("PEDAL", PEDAL_NAMES[set.perf.pedal_mode]);
        }
    } else if (set.page == PAGE_BODY) {
        if (k == EN_K1) {
            set.piano.lid = (uint8_t)clamp((int32_t)set.piano.lid + dir, 0, 3);
            app_say("LID", LID_NAMES[set.piano.lid]);
        } else if (k == EN_K2) {
            set.piano.body = (uint8_t)clamp((int32_t)set.piano.body + dir, 0, 3);
            app_say("BODY", BODY_NAMES[set.piano.body]);
        } else if (k == EN_K3) {
            set.piano.str = (uint8_t)clamp((int32_t)set.piano.str + dir, 0, 3);
            app_say("STRING", STR_NAMES[set.piano.str]);
        } else if (k == EN_K4) {
            set.piano.action = (uint8_t)clamp((int32_t)set.piano.action + dir, 0, 3);
            app_say("ACTION", ACT_NAMES[set.piano.action]);
        }
    } else if (set.page == PAGE_HALL) {
        if (k == EN_K1) {
            set.piano.room = (uint8_t)clamp((int32_t)set.piano.room + dir, 0, 3);
            app_say("ROOM", ROOM_NAMES[set.piano.room]);
        } else if (k == EN_K2) {
            set.piano.seat = (uint8_t)clamp((int32_t)set.piano.seat + dir, 0, 3);
            app_say("SEAT", SEAT_NAMES[set.piano.seat]);
        } else if (k == EN_K3) {
            set.perf.soft_pedal ^= 1u;
            app_say("SOFT", set.perf.soft_pedal ? "UNA CORDA" : "AUTO");
        } else if (k == EN_K4) {
            set.keys.guide_led = (uint8_t)clamp((int32_t)set.keys.guide_led + dir, 0, 2);
            app_say("GUIDE LED", GUIDE_NAMES[set.keys.guide_led]);
        }
    } else { /* PAGE_KEYS */
        if (k == EN_K1) {
            set.keys.key_mode = (uint8_t)((set.keys.key_mode + dir + KEYMD_COUNT) % KEYMD_COUNT);
            dsp_piano_all_off();
            app_say("KEY MODE", KEYMD_NAMES[set.keys.key_mode]);
        } else if (k == EN_K2) {
            set.keys.mod_mode = (uint8_t)((set.keys.mod_mode + dir + MOD_MODE_COUNT) % MOD_MODE_COUNT);
            app_say("MODS", MOD_NAMES[set.keys.mod_mode]);
        } else if (k == EN_K3) {
            set.keys.spread = (uint8_t)((set.keys.spread + dir + SPREAD_COUNT) % SPREAD_COUNT);
            app_say("SPREAD", SPREAD_NAMES[set.keys.spread]);
        } else if (k == EN_K4) {
            set.keys.guide_led = (uint8_t)((set.keys.guide_led + dir + 3) % 3);
            app_say("GUIDE", GUIDE_NAMES[set.keys.guide_led]);
        }
    }
    set_dirty = 1;
}

static void update_leds(uint32_t btn, uint32_t keys)
{
    const pianist_vis_t *vis = pianist_vis();
    uint32_t lbtn = 0, lkeys = keys, dbtn, dkeys = 0, mkeys = 0;
    uint32_t k;

    if (set.page == PAGE_PIANIST) lbtn |= (1u << B_FX);
    else if (set.page == PAGE_BODY) lbtn |= (1u << B_SEL);
    else if (set.page == PAGE_HALL) lbtn |= (1u << B_ENV);
    else lbtn |= (1u << B_LFO);

    if (vis->mode == PLAY_RUN) lbtn |= LED_PLAY_GREEN;
    if (vis->mode == PLAY_REC || vis->mode == PLAY_REC_ARMED) {
        lbtn |= (1u << B_REC) | (1u << B_PLAY);
        plat_play_red(1);
    } else {
        plat_play_red(0);
    }
    if (set.keys.key_mode == KEYMD_TAP) lbtn |= (1u << B_ARP);
    if (any_dirty()) lbtn |= (1u << B_SAVE);
    if (set.theme) lbtn |= (1u << B_GLO);

    for (k = 0; k < NKEYS; k++) {
        uint8_t gb = pianist_key_guide_bright((uint8_t)k, set.octave, &set.keys);
        int32_t note = 53 + (int32_t)k + 12 * set.octave;
        uint8_t pv = (note >= PIANO_M_MIN && note <= PIANO_M_MAX) ? vis->pianist_key[note - PIANO_M_MIN] : 0u;

        if (gb >= 200u || pv > 120u)
            lkeys |= (1u << k);
        else if (gb >= 80u)
            mkeys |= (1u << k);
        else if (gb > 15u || pv > 25u)
            dkeys |= (1u << k);
    }

    dbtn = (~lbtn) & ((1u << NB) - 1u);
    lbtn |= btn;
    plat_lights(lbtn, lkeys, dbtn, dkeys, 2);
    plat_lights_mid(0u, mkeys);
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k;
    int32_t e;
    uint32_t now = plat_ms();
    const pianist_vis_t *vis = pianist_vis();

    touch_poll();
    keys = touch_keys();

    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            uint8_t raw_v = touch_vel(k);
            uint8_t v = raw_v == 0u ? 86u : (uint8_t)(44u + ((uint32_t)raw_v * 83u) / 127u);
            pianist_key_event((uint8_t)k, 1u, v, set.octave, &set.piano, &set.perf, &set.keys);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            pianist_key_event((uint8_t)k, 0u, 0u, set.octave, &set.piano, &set.perf, &set.keys);
        }
    }

    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_OCTDN) {
            octave_step(-1);
        } else if (k == B_OCTUP) {
            octave_step(1);
        } else if (k == B_FX || k == B_HOME) {
            set.page = PAGE_PIANIST;
            set_dirty = 1;
            app_say("PIANOLA", "ROLL");
        } else if (k == B_SEL) {
            set.page = PAGE_BODY;
            set_dirty = 1;
            app_say("BODY", "");
        } else if (k == B_ENV) {
            set.page = PAGE_HALL;
            set_dirty = 1;
            app_say("HALL", "");
        } else if (k == B_LFO) {
            ui.lfo_t = now;
        } else if (k == B_PLAY) {
            pianist_play_toggle();
            app_say(pianist_vis()->mode == PLAY_RUN ? "PLAY" : "STOP", pianist_song_name(set.song_idx));
        } else if (k == B_REC) {
            ui.rec_hold_t = now;
        } else if (k == B_ARP) {
            ui.arp_t = now;
        } else if (k == B_EDIT) {
            ui.edit_t = now;
        } else if (k == B_GLO) {
            ui.glo_t = now;
        } else if (k == B_SAVE) {
            save_all();
        } else if (k == B_SEQ) {
            pianist_stop();
            app_say("REWIND", pianist_song_name(set.song_idx));
        }
    }

    /* LFO tap: open KEYS deck; if already on KEYS, toggle CHORD <-> PIANOLA; hold 700ms = clear modifier locks */
    if (ui.lfo_t) {
        if (btn & (1u << B_LFO)) {
            if (now - ui.lfo_t >= 700u) {
                ui.lfo_t = 0;
                pianist_clear_mods();
                app_say("CHORD MODS", "CLEARED");
            }
        } else {
            ui.lfo_t = 0;
            if (set.page != PAGE_KEYS) {
                set.page = PAGE_KEYS;
                set_dirty = 1;
                app_say("KEYS", KEYMD_NAMES[set.keys.key_mode]);
            } else {
                set.keys.key_mode = (set.keys.key_mode == KEYMD_CHORD) ? KEYMD_TAP : KEYMD_CHORD;
                dsp_piano_all_off();
                set_dirty = 1;
                app_say("KEY MODE", KEYMD_NAMES[set.keys.key_mode]);
            }
        }
    }

    /* ARP tap: toggle PIANOLA (tap-play) <-> BASIC PIANO; hold 450ms: toggle Sustain Pedal */
    if (ui.arp_t) {
        if (btn & (1u << B_ARP)) {
            if (now - ui.arp_t >= 450u) {
                ui.arp_t = 0;
                ui.arp_pedal = !ui.arp_pedal;
                pianist_live_pedal(ui.arp_pedal, &set.perf);
                app_say("PEDAL", ui.arp_pedal ? "DOWN" : "UP");
            }
        } else {
            ui.arp_t = 0;
            set.keys.key_mode = (set.keys.key_mode == KEYMD_TAP) ? KEYMD_PIANO : KEYMD_TAP;
            dsp_piano_all_off();
            set_dirty = 1;
            app_say(set.keys.key_mode == KEYMD_TAP ? "PIANOLA" : "BASIC PIANO", pianist_song_name(set.song_idx));
        }
    }

    if (ui.edit_t) {
        if (btn & (1u << B_EDIT)) {
            if (now - ui.edit_t >= 1000u) {
                ui.edit_t = 0;
                if (set.song_idx >= PIANIST_USER_FIRST) {
                    pianist_clear_user_song((uint8_t)(set.song_idx - PIANIST_USER_FIRST));
                    app_say("CLEARED", pianist_song_name(set.song_idx));
                } else {
                    app_say("BUILTIN", "PROTECTED");
                }
            }
        } else {
            ui.edit_t = 0;
            set.scene = (uint8_t)((set.scene + 1u) % SCENE_COUNT);
            set_dirty = 1;
            app_say("VIEW", SCENE_NAMES[set.scene]);
        }
    }

    if (ui.glo_t) {
        if (btn & (1u << B_GLO)) {
            if (now - ui.glo_t >= 1000u) {
                ui.glo_t = 0;
                set.ble_off ^= 1u;
                set_dirty = 1;
                plat_ble_enable(!set.ble_off);
                app_say("BLUETOOTH", set.ble_off ? "OFF" : "ON");
            }
        } else {
            ui.glo_t = 0;
            set.theme ^= 1u;
            set_dirty = 1;
            app_say("THEME", set.theme ? "OP-PAPER" : "OP-DARK");
        }
    }

    if (ui.rec_hold_t) {
        if (btn & (1u << B_REC)) {
            if (FM1B_BLE_CENTRAL && !set.ble_off && now - ui.rec_hold_t >= 1000u) {
                ui.rec_hold_t = 0;
                if (!btdev_busy() && !ui.looking) {
                    ui.looking = 1;
                    app_say("BLE", "SCANNING");
                } else {
                    int ev = btdev_save();
                    ui.looking = 0;
                    app_say("BLE", btdev_word(ev));
                }
            }
        } else {
            ui.rec_hold_t = 0;
            pianist_rec_toggle();
            {
                const pianist_vis_t *nv = pianist_vis();
                set.song_idx = nv->song_idx;
                app_say(nv->mode == PLAY_REC_ARMED ? "REC ARMED" : "REC STOP", pianist_song_name(set.song_idx));
            }
        }
    }

    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turned_at[k] = now;
        ui.redraw = 1;
        if (k == EN_PRESET) {
            pianist_step_in_cat(set.cat, e);
            set.song_idx = pianist_vis()->song_idx;
            set_dirty = 1;
            app_say(pianist_composer_name(set.song_idx), pianist_song_name(set.song_idx));
        } else if (k == EN_ALGO) {
            if (FM1B_BLE_CENTRAL && ui.looking) {
                btdev_choose(btdev_pick() + e);
            } else {
                set.cat = (uint8_t)(((int32_t)set.cat + (e > 0 ? 1 : -1) + (int32_t)CAT_COUNT) % (int32_t)CAT_COUNT);
                set_dirty = 1;
                if (set.cat != CAT_ALL && pianist_song_cat(set.song_idx) != set.cat) {
                    pianist_step_in_cat(set.cat, 1);
                    set.song_idx = pianist_vis()->song_idx;
                }
                app_say(pianist_cat_name(set.cat), pianist_song_name(set.song_idx));
            }
        } else if (k == EN_SELECT) {
            pianist_seek_bar(e);
            ui.redraw = 1;
        } else {
            adjust_knob(k, e);
        }
    }

    if (keys != ui.keys || btn != ui.btn || vis->mode == PLAY_RUN || vis->mode == PLAY_REC)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    update_leds(btn, keys);
}

void app_input_only(void) { input(); }

/* ----------------------------------------------------------- Pixel Art --- */
static uint16_t th_bg(void)   { return set.theme ? OP_CREAM : RGB(17, 18, 22); }
static uint16_t th_card(void) { return set.theme ? RGB(224, 219, 207) : RGB(28, 30, 38); }
static uint16_t th_line(void) { return set.theme ? RGB(192, 186, 173) : RGB(48, 52, 64); }
static uint16_t th_fg(void)   { return set.theme ? OP_INK : OP_CREAM; }
static uint16_t th_dim(void)  { return set.theme ? RGB(115, 112, 104) : RGB(118, 124, 140); }

static void draw_circle_pts(int32_t cx, int32_t cy, int32_t r, uint16_t c, int fill)
{
    int32_t x = r, y = 0, err = 1 - x;
    while (x >= y) {
        if (fill) {
            cv_rect(cx - x, cy + y, 2 * x + 1, 1, c);
            cv_rect(cx - x, cy - y, 2 * x + 1, 1, c);
            cv_rect(cx - y, cy + x, 2 * y + 1, 1, c);
            cv_rect(cx - y, cy - x, 2 * y + 1, 1, c);
        } else {
            cv_pset(cx + x, cy + y, c); cv_pset(cx - x, cy + y, c);
            cv_pset(cx + x, cy - y, c); cv_pset(cx - x, cy - y, c);
            cv_pset(cx + y, cy + x, c); cv_pset(cx - y, cy + x, c);
            cv_pset(cx + y, cy - x, c); cv_pset(cx - y, cy - x, c);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void draw_dial(int32_t cx, int32_t cy, int32_t val, int32_t vmax, uint16_t col, int lit)
{
    float frac = vmax > 0 ? fm_clampf((float)val / (float)vmax, 0.0f, 1.0f) : 0.0f;
    float ang = (0.75f * FM_PI) + frac * (1.5f * FM_PI);
    int32_t px = cx + (int32_t)(fm_cosf(ang) * 9.0f);
    int32_t py = cy + (int32_t)(fm_sinf(ang) * 9.0f);

    draw_circle_pts(cx, cy, 11, th_card(), 1);
    draw_circle_pts(cx, cy, 11, lit ? col : th_line(), 0);
    draw_circle_pts(cx, cy, 3, col, 1);
    cv_line(cx, cy, px, py, th_fg());
    cv_rect(px - 1, py - 1, 3, 3, col);
}

static void spawn_spark(int32_t x, int32_t y, uint16_t col)
{
    uint32_t i;
    for (i = 0; i < N_SPARK; i++) {
        if (sparks[i].life == 0u) {
            spark_rng = spark_rng * 1664525u + 1013904223u;
            sparks[i].x = (int16_t)(x + (int32_t)((spark_rng >> 16) & 15u) - 8);
            spark_rng = spark_rng * 1664525u + 1013904223u;
            sparks[i].y = (int16_t)y;
            sparks[i].vy = -1 - (int8_t)((spark_rng >> 20) & 1u);
            sparks[i].life = 18u;
            sparks[i].col = col;
            break;
        }
    }
}

static void update_sparks(const pianist_vis_t *vis)
{
    uint32_t i;
    static uint32_t prev_sum;
    uint32_t sum = 0;
    for (i = 0; i < 88u; i++)
        sum += (vis->pianist_key[i] > 235u) + (vis->live_key[i] > 235u);
    if (sum > prev_sum) {
        int32_t sx = 122 + (int32_t)(vis->rh_pos * 0.45f);
        spawn_spark(sx, 68, (sum & 1u) ? OP_AMBER : OP_CYAN);
    }
    prev_sum = sum;
    for (i = 0; i < N_SPARK; i++) {
        if (sparks[i].life) {
            sparks[i].y += sparks[i].vy;
            if ((ui.anim_tick & 1u) == 0u)
                sparks[i].x += (int16_t)((i & 1u) ? 1 : -1);
            sparks[i].life--;
        }
    }
}

static void draw_scene_pianist(const pianist_vis_t *vis)
{
    uint16_t bg = th_bg(), card = th_card(), line = th_line(), dim = th_dim();
    uint16_t p_dark = set.theme ? RGB(30, 32, 40) : RGB(10, 11, 15);
    uint16_t p_edge = set.theme ? RGB(70, 74, 88) : RGB(80, 88, 108);
    uint16_t p_wood = RGB(110, 68, 42);
    int32_t lean = (int32_t)(vis->energy * 4.5f);
    int32_t bob = (int32_t)(fm_sinf((float)ui.anim_tick * 0.16f) * (vis->energy * 2.5f));
    int32_t rh_y = 73 + ((vis->rh_pos > 0 && vis->energy > 0.15f && (ui.anim_tick & 2u)) ? 2 : 0);
    int32_t lh_y = 74 + ((vis->lh_pos > 0 && vis->energy > 0.15f && (ui.anim_tick & 4u)) ? 2 : 0);
    int32_t head_x = 58 + lean, head_y = 44 + bob;
    int32_t i;

    for (i = 20; i < 240; i += 20)
        cv_line(i, 4, i, 120, card);
    cv_rect(0, 108, 240, 2, line);

    /* Moonlit Studio Window */
    cv_rect(14, 10, 44, 52, set.theme ? RGB(34, 42, 62) : RGB(14, 20, 36));
    cv_rect(14, 10, 44, 1, line); cv_rect(14, 61, 44, 1, line);
    cv_rect(14, 10, 1, 52, line); cv_rect(57, 10, 1, 52, line);
    cv_rect(35, 10, 1, 52, line); cv_rect(14, 35, 44, 1, line);
    draw_circle_pts(26, 22, 6, OP_AMBER, 1);
    draw_circle_pts(29, 20, 5, set.theme ? RGB(34, 42, 62) : RGB(14, 20, 36), 1);
    if ((ui.anim_tick & 15u) < 12u) cv_pset(45, 18, OP_CREAM);
    if (((ui.anim_tick + 7u) & 15u) < 12u) cv_pset(49, 28, OP_CYAN);
    cv_pset(22, 48, OP_AMBER);

    /* Top-right stage badge: LIVE FX during playback, PIANOLA when stopped in tap mode, else chord */
    if (set.keys.key_mode == KEYMD_TAP) {
        if (vis->mode == PLAY_RUN) {
            const char *fx = pianist_fx_badge();
            uint8_t on = fx[0] != 0;
            cv_rect(164, 6, 68, 14, on ? OP_CORAL : OP_CYAN);
            cv_text(168, 8, &FONT_T, on ? fx : "LIVE FX", OP_INK);
        } else {
            cv_rect(170, 6, 62, 14, OP_AMBER);
            cv_text(177, 8, &FONT_T, "PIANOLA", OP_INK);
        }
    } else {
        cv_rect(170, 6, 62, 14, card);
        cv_text(174, 8, &FONT_T, pianist_chord_badge(), OP_MINT);
    }

    /* Grand Piano (lid height responds to set.piano.lid) */
    {
        int32_t lid_y = set.piano.lid == LID_CLOSED ? 60 :
                        set.piano.lid == LID_HALF ? 38 :
                        set.piano.lid == LID_OPEN ? 22 : 14;
        cv_line(110, 62, 212, lid_y, p_edge);
        cv_line(110, 63, 212, lid_y + 1, p_dark);
        if (set.piano.lid != LID_CLOSED)
            cv_line(178, (62 + lid_y) / 2, 178, 62, OP_AMBER);
    }
    if (vis->energy > 0.05f) {
        uint16_t glow = vis->energy > 0.45f ? OP_AMBER : RGB(90, 72, 42);
        cv_line(124, 62, 188, 36, glow);
        cv_line(130, 62, 194, 38, OP_CYAN);
    }
    cv_rect(104, 62, 118, 16, p_dark);
    cv_rect(104, 62, 118, 1, p_edge);
    cv_rect(112, 78, 104, 6, p_dark);
    cv_rect(210, 56, 12, 10, p_dark);
    cv_rect(210, 56, 12, 1, p_edge);
    cv_rect(105, 45, 16, 17, line);
    cv_rect(106, 46, 14, 15, OP_CREAM);
    cv_rect(108, 49, 10, 1, dim);
    cv_rect(108, 52, 8, 1, dim);
    cv_rect(108, 55, 10, 1, dim);
    cv_rect(90, 72, 22, 7, p_dark);
    cv_rect(88, 72, 20, 3, OP_CREAM);
    for (i = 0; i < 9; i++) {
        uint8_t on = vis->pianist_key[20 + i * 5] > 40u || vis->live_key[20 + i * 5] > 40u;
        cv_rect(88 + i * 2, 72, 2, 2, on ? OP_CORAL : OP_CREAM);
    }
    cv_rect(104, 79, 6, 27, p_dark);
    cv_rect(103, 105, 8, 3, OP_AMBER);
    cv_rect(206, 84, 6, 22, p_dark);
    cv_rect(205, 105, 8, 3, OP_AMBER);
    cv_rect(128, 84, 5, 21, p_dark);
    cv_rect(124, 104, 13, 3, p_wood);
    cv_rect(118, vis->pedal_down ? 106 : 103, 8, 2, OP_AMBER);

    /* Bench & Pianist */
    cv_rect(42, 86, 28, 5, p_wood);
    cv_rect(44, 91, 3, 17, p_wood);
    cv_rect(65, 91, 3, 17, p_wood);
    {
        uint16_t coat = RGB(36, 40, 52);
        uint16_t legc = RGB(48, 52, 66);
        cv_line(58, 84, 84, 90, legc);
        cv_line(84, 90, 114, vis->pedal_down ? 105 : 102, legc);
        cv_line(84, 91, 114, vis->pedal_down ? 106 : 103, p_edge);
        cv_rect(112, vis->pedal_down ? 104 : 101, 7, 4, p_edge);
        cv_rect(48 + lean / 2, 58 + bob, 16, 28, coat);
        cv_rect(48 + lean / 2, 58 + bob, 16, 1, p_edge);
        cv_rect(48 + lean / 2, 58 + bob, 1, 28, p_edge);
        cv_rect(51 + lean / 2, 60 + bob, 10, 24, p_dark);
        cv_line(48 + lean / 2, 80 + bob, 38, 96, coat);
        cv_line(49 + lean / 2, 80 + bob, 40, 96, p_edge);
        cv_rect(58 + lean, 59 + bob, 5, 6, OP_CREAM);
        cv_pset(61 + lean, 61 + bob, OP_CORAL);
    }
    draw_circle_pts(head_x, head_y, 7, RGB(236, 198, 170), 1);
    cv_rect(head_x - 7, head_y - 7, 13, 5, p_dark);
    cv_rect(head_x - 8, head_y - 5, 5, 9, p_dark);
    cv_pset(head_x + 4, head_y - 1, p_dark);

    {
        int32_t sh_x = 58 + lean, sh_y = 63 + bob;
        int32_t el_x = 72 + lean / 2, el_y = 75 + bob / 2;
        int32_t rx = 89 + (int32_t)(vis->rh_pos * 0.18f);
        int32_t lx = 86 + (int32_t)(vis->lh_pos * 0.14f);
        cv_line(sh_x, sh_y, el_x, el_y + 1, p_edge);
        cv_line(el_x, el_y + 1, lx, lh_y, p_edge);
        cv_rect(lx - 1, lh_y - 1, 5, 3, OP_MINT);
        cv_line(sh_x, sh_y, el_x, el_y, p_dark);
        cv_line(sh_x, sh_y + 1, el_x, el_y + 1, p_dark);
        cv_line(el_x, el_y, rx, rh_y, p_dark);
        cv_line(el_x, el_y + 1, rx, rh_y + 1, p_dark);
        cv_rect(rx - 1, rh_y - 1, 5, 3, RGB(236, 198, 170));
    }

    for (i = 0; i < (int32_t)N_SPARK; i++) {
        if (sparks[i].life) {
            int32_t sx = sparks[i].x, sy = sparks[i].y;
            cv_rect(sx, sy, 2, 3, sparks[i].col);
            cv_pset(sx + 2, sy, sparks[i].col);
        }
    }

    /* Mini 88-key strip */
    {
        uint8_t kvis[88];
        dsp_piano_key_vis(kvis);
        cv_rect(8, 113, 224, 10, p_dark);
        for (i = 0; i < 88; i++) {
            int32_t x = 10 + (i * 220) / 88;
            uint16_t kc = kvis[i] ? (vis->live_key[i] > 20u ? OP_MINT : OP_CYAN) : dim;
            if (kvis[i] || (i % 12 == 0))
                cv_rect(x, 114, 2, kvis[i] ? 8 : 3, kc);
        }
    }
    (void)bg;
}

static void draw_scene_grand(const pianist_vis_t *vis)
{
    uint16_t bg = th_bg(), card = th_card(), line = th_line(), fg = th_fg(), dim = th_dim();
    uint8_t kvis[88];
    int32_t i;

    dsp_piano_key_vis(kvis);
    cv_rect(12, 8, 216, 86, card);
    cv_rect(12, 8, 216, 2, line);
    cv_rect(12, 8, 2, 86, line);
    cv_rect(226, 8, 2, 86, line);
    cv_line(24, 20, 120, 26, OP_AMBER);
    cv_line(120, 26, 216, 54, OP_AMBER);

    for (i = 0; i < 44; i++) {
        int32_t x = 20 + i * 4;
        int32_t top_y = i < 22 ? (22 + (i * 4) / 22) : (26 + ((i - 22) * 28) / 22);
        uint8_t v_lo = kvis[i * 2], v_hi = kvis[i * 2 + 1];
        uint8_t act = v_lo > v_hi ? v_lo : v_hi;
        int32_t vib = act > 30u ? (((ui.anim_tick + (uint32_t)i) & 1u) ? 1 : -1) : 0;
        uint16_t scol = act > 80u ? OP_CYAN : act > 20u ? OP_AMBER : (i < 14 ? RGB(140, 92, 54) : dim);
        cv_line(x + vib, top_y, x, 86, scol);
        {
            int32_t hy = act > 150u ? 72 : 82;
            cv_rect(x - 1, hy, 3, 4, act > 150u ? OP_CORAL : fg);
        }
    }

    {
        int32_t dy = vis->pedal_down ? 58 : 68;
        cv_rect(16, dy, 172, 3, vis->pedal_down ? OP_MINT : line);
        cv_text(192, dy - 4, &FONT_T, vis->pedal_down ? "PED" : "DMP", vis->pedal_down ? OP_MINT : dim);
    }

    cv_rect(12, 96, 216, 24, card);
    for (i = 0; i < 88; i++) {
        int32_t x = 14 + (i * 212) / 88;
        uint8_t is_blk = ((i + 9) % 12 == 1 || (i + 9) % 12 == 3 || (i + 9) % 12 == 6 ||
                          (i + 9) % 12 == 8 || (i + 9) % 12 == 10);
        uint16_t kc = kvis[i] ? (vis->live_key[i] > 20u ? OP_MINT : OP_CORAL) : (is_blk ? bg : OP_CREAM);
        cv_rect(x, 98, 2, is_blk ? 11 : 19, kc);
    }
}

static void draw_scene_tape(const pianist_vis_t *vis)
{
    uint16_t card = th_card(), line = th_line(), fg = th_fg(), dim = th_dim();
    int32_t spin = (vis->mode == PLAY_RUN || vis->mode == PLAY_REC) ? (int32_t)(ui.anim_tick * 12u) : 0;
    uint16_t reel_col = (vis->mode == PLAY_REC || vis->mode == PLAY_REC_ARMED) ? OP_CORAL : OP_CYAN;
    int8_t sc[64];
    int32_t i, k;

    cv_rect(12, 10, 216, 84, card);
    cv_rect(12, 10, 216, 1, line);
    cv_rect(12, 93, 216, 1, line);

    for (k = 0; k < 2; k++) {
        int32_t cx = k == 0 ? 60 : 180, cy = 46;
        draw_circle_pts(cx, cy, 24, th_bg(), 1);
        draw_circle_pts(cx, cy, 24, reel_col, 0);
        draw_circle_pts(cx, cy, 10, line, 0);
        draw_circle_pts(cx, cy, 4, fg, 1);
        for (i = 0; i < 3; i++) {
            float a = ((float)spin + (float)i * 120.0f) * (FM_PI / 180.0f);
            cv_line(cx, cy, cx + (int32_t)(fm_cosf(a) * 21.0f), cy + (int32_t)(fm_sinf(a) * 21.0f), fg);
        }
    }

    dsp_piano_scope_get(sc, 64u);
    cv_rect(88, 24, 64, 44, th_bg());
    cv_rect(88, 24, 64, 1, line); cv_rect(88, 67, 64, 1, line);
    for (i = 0; i < 63; i++) {
        int32_t y0 = 46 - (sc[i] * 18) / 127;
        int32_t y1 = 46 - (sc[i + 1] * 18) / 127;
        cv_line(88 + i, y0, 89 + i, y1, OP_MINT);
    }

    cv_line(60, 70, 84, 84, dim);
    cv_line(84, 84, 156, 84, dim);
    cv_line(156, 84, 180, 70, dim);

    {
        int32_t pw = vis->total_ms > 0 ? (int32_t)((vis->elapsed_ms * 212u) / vis->total_ms) : 0;
        if (pw > 212) pw = 212;
        cv_rect(14, 102, 212, 8, card);
        cv_rect(14, 102, pw, 8, reel_col);
        {
            char nb[36], n1[12];
            nb[0] = 0;
            text_cat(nb, pianist_cat_name(set.cat));
            text_cat(nb, "  V:");
            int_text((int32_t)dsp_piano_active_count(), n1); text_cat(nb, n1);
            cv_text(14, 113, &FONT_T, nb, dim);
        }
    }
}

static void draw(void)
{
    char t1[24], t2[24];
    uint32_t now = plat_ms();
    const pianist_vis_t *vis = pianist_vis();
    uint16_t bg = th_bg(), card = th_card(), line = th_line(), fg = th_fg(), dim = th_dim();
    uint32_t pct = plat_master() * 100u / 4096u;
    int32_t i;

    /* ---- 1. Header Strip (240 x 34 at y=0) ---- */
    cv_begin(240, 34, bg);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 8, &FONT_B, ui.msg[0], OP_CORAL);
        cv_text(x + 8, 8, &FONT_B, ui.msg[1], fg);
    } else {
        const char *st = vis->mode == PLAY_RUN ? "AUTO" :
                         vis->mode == PLAY_REC ? "REC" :
                         vis->mode == PLAY_REC_ARMED ? "ARM" :
                         set.keys.key_mode == KEYMD_TAP ? "TAP" : "KEYS";
        uint16_t stc = vis->mode == PLAY_RUN ? OP_MINT :
                       (vis->mode == PLAY_REC || vis->mode == PLAY_REC_ARMED) ? OP_CORAL :
                       set.keys.key_mode == KEYMD_TAP ? OP_AMBER : dim;
        cv_rect(6, 6, 34, 18, card);
        cv_text(9, 9, &FONT_T, st, stc);
        cv_text(44, 4, &FONT_S, pianist_song_name(set.song_idx), fg);
        cv_text(44, 20, &FONT_T, pianist_composer_name(set.song_idx), dim);

        fmt_mmss(vis->elapsed_ms, t1);
        if (vis->total_bars > 0 && set.song_idx < PIANIST_USER_FIRST) {
            t2[0] = 'B';
            int_text((int32_t)vis->bar, t2 + 1);
            text_cat(t2, " ");
            text_cat(t2, t1);
        } else {
            t2[0] = 0;
            text_cat(t2, t1);
        }
        cv_text(234 - text_w(&FONT_T, t2), 4, &FONT_T, t2, fg);
        if (vis->pedal_down)
            cv_text(176, 19, &FONT_T, "PED", OP_AMBER);
        if (set.perf.soft_pedal)
            cv_text(198, 19, &FONT_T, "SFT", OP_CYAN);
        if (FM1B_BLE)
            cv_rect(226, 21, 6, 6, plat_ble_connected() ? OP_CYAN : dim);
    }
    cv_rect(0, 33, 240, 1, line);
    cv_blit(0, 0);

    /* ---- 2. Center Pixel Stage (240 x 126 at y=34) ---- */
    cv_begin(240, 126, bg);
    if (set.scene == SCENE_PIANIST)
        draw_scene_pianist(vis);
    else if (set.scene == SCENE_GRAND)
        draw_scene_grand(vis);
    else
        draw_scene_tape(vis);
    cv_blit(0, 34);

    /* ---- 3. Bottom OP-1 Control Deck (240 x 80 at y=160) ---- */
    cv_begin(240, 80, bg);
    cv_rect(0, 0, 240, 1, line);

    {
        static const char *const PNAMES[4] = {"ROLL", "BODY", "HALL", "KEYS"};
        static const uint16_t PCOL[4] = {OP_CORAL, OP_CYAN, OP_MINT, OP_AMBER};
        int32_t tx = 4;
        for (i = 0; i < 4; i++) {
            int32_t w = 36;
            cv_rect(tx, 3, w, 11, set.page == (uint8_t)i ? PCOL[i] : card);
            cv_text(tx + 4, 3, &FONT_T, PNAMES[i], set.page == (uint8_t)i ? OP_INK : dim);
            tx += w + 3;
        }
    }

    if (set.keys.key_mode == KEYMD_CHORD) {
        char mb[24], ml[16];
        pianist_mod_label(ml, sizeof(ml));
        mb[0] = 0;
        text_cat(mb, MOD_NAMES[set.keys.mod_mode]);
        text_cat(mb, ":");
        text_cat(mb, ml);
        cv_text(234 - text_w(&FONT_T, mb), 3, &FONT_T, mb, OP_AMBER);
    } else if (set.keys.key_mode == KEYMD_TAP) {
        const char *fx = pianist_fx_badge();
        const char *sub = vis->mode == PLAY_RUN ? (fx[0] ? fx : "LIVE FX") : "TAP TO PLAY";
        uint16_t subc = vis->mode == PLAY_RUN ? (fx[0] ? OP_CORAL : OP_CYAN) : OP_AMBER;
        cv_text(234 - text_w(&FONT_T, sub), 3, &FONT_T, sub, subc);
    } else {
        char ob[16], nb[8];
        ob[0] = 'O'; ob[1] = set.octave < 0 ? '-' : '+';
        ob[2] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave));
        ob[3] = ' '; ob[4] = 0;
        int_text((int32_t)pct, nb); text_cat(ob, nb); text_cat(ob, "%");
        cv_text(234 - text_w(&FONT_T, ob), 3, &FONT_T, ob, fg);
    }

    {
        static const uint16_t KCOL[4] = {OP_CORAL, OP_MINT, OP_CYAN, OP_AMBER};
        const char *lbl[4];
        char val[4][10];
        int32_t knum[4], kmax[4];

        if (set.page == PAGE_PIANIST) {
            lbl[0] = "TEMPO"; lbl[1] = "RUBATO"; lbl[2] = "VOICE"; lbl[3] = "PEDAL";
            knum[0] = set.perf.tempo_pct - 50; kmax[0] = 100;
            knum[1] = set.perf.rubato; kmax[1] = 100;
            knum[2] = set.perf.voicing; kmax[2] = 100;
            knum[3] = set.perf.pedal_mode; kmax[3] = PEDAL_NMODES - 1;
            int_text(set.perf.tempo_pct, val[0]); text_cat(val[0], "%");
            int_text(set.perf.rubato, val[1]); text_cat(val[1], "%");
            int_text(set.perf.voicing, val[2]); text_cat(val[2], "%");
            val[3][0] = 0; text_cat(val[3], PEDAL_NAMES[set.perf.pedal_mode]);
        } else if (set.page == PAGE_BODY) {
            lbl[0] = "LID"; lbl[1] = "BODY"; lbl[2] = "STRING"; lbl[3] = "ACTION";
            knum[0] = set.piano.lid; kmax[0] = 3;
            knum[1] = set.piano.body; kmax[1] = 3;
            knum[2] = set.piano.str; kmax[2] = 3;
            knum[3] = set.piano.action; kmax[3] = 3;
            val[0][0] = 0; text_cat(val[0], LID_NAMES[set.piano.lid]);
            val[1][0] = 0; text_cat(val[1], BODY_NAMES[set.piano.body]);
            val[2][0] = 0; text_cat(val[2], STR_NAMES[set.piano.str]);
            val[3][0] = 0; text_cat(val[3], ACT_NAMES[set.piano.action]);
        } else if (set.page == PAGE_HALL) {
            lbl[0] = "ROOM"; lbl[1] = "SEAT"; lbl[2] = "SOFT"; lbl[3] = "GUIDE";
            knum[0] = set.piano.room; kmax[0] = 3;
            knum[1] = set.piano.seat; kmax[1] = 3;
            knum[2] = set.perf.soft_pedal; kmax[2] = 1;
            knum[3] = set.keys.guide_led; kmax[3] = 2;
            val[0][0] = 0; text_cat(val[0], ROOM_NAMES[set.piano.room]);
            val[1][0] = 0; text_cat(val[1], SEAT_NAMES[set.piano.seat]);
            val[2][0] = 0; text_cat(val[2], set.perf.soft_pedal ? "UNA" : "AUTO");
            val[3][0] = 0; text_cat(val[3], GUIDE_NAMES[set.keys.guide_led]);
        } else {
            lbl[0] = "KEYMD"; lbl[1] = "MODS"; lbl[2] = "SPREAD"; lbl[3] = "GUIDE";
            knum[0] = set.keys.key_mode; kmax[0] = KEYMD_COUNT - 1;
            knum[1] = set.keys.mod_mode; kmax[1] = MOD_MODE_COUNT - 1;
            knum[2] = set.keys.spread; kmax[2] = SPREAD_COUNT - 1;
            knum[3] = set.keys.guide_led; kmax[3] = 2;
            val[0][0] = 0; text_cat(val[0], KEYMD_NAMES[set.keys.key_mode]);
            val[1][0] = 0; text_cat(val[1], MOD_NAMES[set.keys.mod_mode]);
            val[2][0] = 0; text_cat(val[2], SPREAD_NAMES[set.keys.spread]);
            val[3][0] = 0; text_cat(val[3], GUIDE_NAMES[set.keys.guide_led]);
        }

        for (i = 0; i < 4; i++) {
            int32_t cx = 30 + i * 60;
            int lit = ui.turned_at[EN_K1 + i] && now - ui.turned_at[EN_K1 + i] < 400u;
            draw_dial(cx, 28, knum[i], kmax[i], KCOL[i], lit);
            cv_text(cx - text_w(&FONT_T, lbl[i]) / 2, 41, &FONT_T, lbl[i], KCOL[i]);
            cv_text(cx - text_w(&FONT_T, val[i]) / 2, 51, &FONT_T, val[i], fg);
        }
    }

    /* Live 27-Key Strip */
    {
        static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
        static const uint8_t AT[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};
        uint8_t mods = pianist_active_mod_mask();
        uint32_t fxm = pianist_active_fx_mask();
        uint8_t live_fx = (set.keys.key_mode == KEYMD_TAP && vis->mode == PLAY_RUN) ? 1u : 0u;
        for (i = 0; i < 16; i++) {
            uint8_t k = WK[i];
            int32_t note = 53 + (int32_t)k + 12 * set.octave;
            uint8_t p_on = (note >= PIANO_M_MIN && note <= PIANO_M_MAX) ? vis->pianist_key[note - PIANO_M_MIN] > 50u : 0u;
            uint8_t u_on = (ui.keys >> k) & 1u;
            uint16_t c = u_on ? OP_MINT : p_on ? OP_CYAN : card;
            if (live_fx) {
                c = (fxm & (1u << k)) ? (k < 12u ? OP_CORAL : OP_AMBER) : (k < 12u ? RGB(58, 36, 38) : RGB(30, 44, 54));
            } else if (set.keys.key_mode == KEYMD_CHORD && k < 7u) {
                uint8_t bit = (uint8_t)(1u << k);
                c = (mods & bit) ? OP_AMBER : RGB(62, 54, 34);
            }
            cv_rect(8 + i * 14, 71, 12, 8, c);
        }
        for (i = 0; i < 11; i++) {
            uint8_t k = BK[i];
            int32_t note = 53 + (int32_t)k + 12 * set.octave;
            uint8_t p_on = (note >= PIANO_M_MIN && note <= PIANO_M_MAX) ? vis->pianist_key[note - PIANO_M_MIN] > 50u : 0u;
            uint8_t u_on = (ui.keys >> k) & 1u;
            uint16_t c = u_on ? OP_CORAL : p_on ? OP_AMBER : line;
            if (live_fx) {
                c = (fxm & (1u << k)) ? (k < 12u ? OP_CORAL : OP_AMBER) : (k < 12u ? RGB(74, 42, 46) : RGB(40, 58, 72));
            } else if (set.keys.key_mode == KEYMD_CHORD && k < 7u) {
                uint8_t bit = (uint8_t)(1u << k);
                c = (mods & bit) ? OP_CORAL : RGB(78, 44, 44);
            }
            cv_rect(8 + AT[i] * 14 + 8, 66, 9, 6, c);
        }
    }
    cv_blit(0, 160);
}

void app_ui_init(void)
{
    touch_on(1);
    touch_curve(-4);
    lcd_fill(0, 0, 240, 240, th_bg());
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    const pianist_vis_t *vis = pianist_vis();
    input();
    ui.anim_tick++;
    update_sparks(vis);

    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (now - ui.last_draw_ms >= 33u || ui.redraw) {
        ui.last_draw_ms = now;
        ui.redraw = 0;
        draw();
    }
}

void app_log(void (*line)(const char *t))
{
    char buf[42], nb[12];
    buf[0] = 0;
    text_cat(buf, "cpu=");
    int_text((int32_t)plat_cpu_pct(), nb); text_cat(buf, nb);
    text_cat(buf, "% xr=");
    int_text((int32_t)plat_xruns(), nb); text_cat(buf, nb);
    text_cat(buf, " hp=");
    int_text((int32_t)plat_headphones(), nb); text_cat(buf, nb);
    text_cat(buf, " v=");
    int_text((int32_t)dsp_piano_active_count(), nb); text_cat(buf, nb);
    line(buf);

    buf[0] = 0;
    text_cat(buf, "m=");
    int_text((int32_t)plat_master(), nb); text_cat(buf, nb);
    text_cat(buf, " c2=");
    int_text((int32_t)plat_cpu2_on(), nb); text_cat(buf, nb);
    text_cat(buf, " km=");
    text_cat(buf, KEYMD_NAMES[set.keys.key_mode]);
    line(buf);
}

#ifdef PLAT_HOST
static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int app_expect(const char *what, int *got)
{
    const pianist_vis_t *vis = pianist_vis();
    if (str_eq(what, "song")) *got = (int)set.song_idx;
    else if (str_eq(what, "cat")) *got = (int)set.cat;
    else if (str_eq(what, "scene")) *got = (int)set.scene;
    else if (str_eq(what, "page")) *got = (int)set.page;
    else if (str_eq(what, "mode")) *got = (int)vis->mode;
    else if (str_eq(what, "tempo")) *got = (int)set.perf.tempo_pct;
    else if (str_eq(what, "rubato")) *got = (int)set.perf.rubato;
    else if (str_eq(what, "voicing")) *got = (int)set.perf.voicing;
    else if (str_eq(what, "pedal_mode")) *got = (int)set.perf.pedal_mode;
    else if (str_eq(what, "lid")) *got = (int)set.piano.lid;
    else if (str_eq(what, "body")) *got = (int)set.piano.body;
    else if (str_eq(what, "room")) *got = (int)set.piano.room;
    else if (str_eq(what, "key_mode")) *got = (int)set.keys.key_mode;
    else if (str_eq(what, "spread")) *got = (int)set.keys.spread;
    else if (str_eq(what, "mods")) *got = (int)pianist_active_mod_mask();
    else if (str_eq(what, "fx_mask")) *got = (int)pianist_active_fx_mask();
    else if (str_eq(what, "note_idx")) *got = (int)vis->note_idx;
    else if (str_eq(what, "bar")) *got = (int)vis->bar;
    else if (str_eq(what, "voices")) *got = (int)dsp_piano_active_count();
    else if (str_eq(what, "dirty")) *got = (int)any_dirty();
    else return 0;
    return 1;
}
#endif
