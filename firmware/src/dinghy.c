/* SPDX-License-Identifier: GPL-3.0-only */
/* PLUMB: an FM-1 firmware on Dinghy, where the rhythm is a pendulum wave.
 *
 * Twelve pendulums hang in a row. PLAY releases them together, from the same side. Pendulum k swings SWINGS + k times
 * in one CYCLE: the long ones slowly, the short ones fast. They leave in a line, turn into a snake, split into two
 * rows, three, scatter into what looks like chaos, and meet again, all in a line, at the end of the cycle. Each time
 * a bob passes its STRIKE point it plays its note. Nothing is random and nothing repeats until the cycle does.
 *
 *   THE ROW (HOME)     a note for each pendulum, a scale step; SELECT picks one, REC plays new notes into the row
 *   THE SWING (SEQ)    CYCLE, SWINGS, STRIKE, GRID: the polyrhythm
 *   THE BOBS (EDIT)    FOLD, PHASE or PULSAR (ALGORITHM), shaped by ENV and LFO, then FX
 *   THE ARP            held keys replace the row: the pendulums play your chord
 *
 * The files: plumb.h (the pendulums, the engines, the ENV, the LFO; the audio interrupt), scale.h (48 scales),
 * effects.h (the SWING ECHO and the reverb that BLOOMs, in parallel), this file (the keys, the presets, the pages). platform.c and the
 * kit_*.h are Dinghy's. */
#include <stdint.h>
#define APP_NAME "PLUMB"
static void app_idle(void);
static void clock_rt(uint32_t st);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in */
#define MIDI_RT_HOOK(st) clock_rt(st)           /* clock and transport as they arrive: kit_clock.h */
#define LCD_MIRROR 1                            /* FM-1 Mirror: kit_mirror.h */
#define CV_MAX (240u * 150u)
#define BACKUP_TAG0 'P'
#define BACKUP_TAG1 'L'
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

static void app_tick(uint32_t tick, uint32_t ofs);
#define CLOCK_TICK(tick, ofs) app_tick(tick, ofs)
#include "kit_clock.h"
#include "scale.h"
#include "plumb.h"
#include "effects.h"

static void live_note(uint32_t on, uint32_t note, uint32_t vel);
static void live_panic(void);
static void midi_program(uint32_t p);
#define MIDI_NOTE(on, note, vel) live_note(on, note, vel)
#define MIDI_PANIC() live_panic()
#define MIDI_PROGRAM(p) midi_program(p)
#include "kit_midi.h"

/* ------------------------------------------------------------------------------------------------ the sound */
static int32_t output_level(int32_t sample)
{
    int32_t m = sample < 0 ? -sample : sample;  /* a soft ceiling (as LOOPHEADS) */
    if (m > 20000) {
        int32_t excess = m - 20000;
        m = 20000 + 12000 * excess / (12000 + excess);
    }
    return sample < 0 ? -m : m;
}

static void app_audio(int32_t *out, uint32_t frames)
{
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks();
#endif
    clock_audio(frames);                        /* the transport first: its ticks move the pendulums */
    plumb_render(out, frames);
    effects_process(out, frames);
    int32_t vol = midi_vol_q15();
    for (uint32_t i = 0; i < 2u * frames; i++) out[i] = (output_level(out[i]) * vol) >> 15;
#ifndef DINGHY_HOST
    /* A safety net (as BUOY): the sound has 5.8 ms a block. If it ever takes more than 3.8 ms, new strikes get only
     * five voices until it has been calm for 2 s: a little less sound instead of crackles. */
    static uint16_t calm;
    uint32_t us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (us > 3800u) { voice_cap = 5; calm = 0; }
    else if (voice_cap < NVOICE && us < 2500u && ++calm > 350u) voice_cap = NVOICE;
#endif
}

#include "kit_mirror.h"
#define BACKUP_STOP() (live_panic(), clock_stop())
#include "kit_backup.h"

/* ---------------------------------------------------------------------------------------------- settings */
#define LONG_MS 700u                            /* a press this long is a "hold" */
#define KEY0 53u                                /* key 0 = F3 */
#define SET_VER 1u
static struct { uint8_t ver, oct, ch, sync_int, tempo, pad[3]; } set = {SET_VER, 0, 0, 0, 96, {0}};

static char *put_num(char *p, int32_t v, int sign)
{
    char tmp[12];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *p++ = '-';
    else if (sign && v > 0) *p++ = '+';
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

/* ------------------------------------------------------------------------------------------------ the row */
static uint8_t picked;                          /* SELECT: the pendulum the knobs and SEL work on */
static uint32_t pend_flash[PENDULUMS];          /* when each one last struck (ms), for the screen */
static uint8_t rec_on;                          /* REC: the keys write into the row, from the picked pendulum */
static uint32_t row_rnd = 0x9e3779b9u;

static void row_shape(uint8_t shape)            /* ROW: a shape for all twelve notes at once */
{
    static const int8_t arch[PENDULUMS] = {0, 2, 4, 6, 8, 10, 11, 9, 7, 5, 3, 1};
    static const int8_t zig[PENDULUMS] = {0, 2, 1, 3, 2, 4, 3, 5, 4, 6, 5, 7};
    for (uint32_t k = 0; k < PENDULUMS; k++) {
        int32_t s;
        switch (shape) {
        case ROW_RISE: s = (int32_t)k; break;
        case ROW_FALL: s = 11 - (int32_t)k; break;
        case ROW_ARCH: s = arch[k]; break;
        case ROW_VALLEY: s = 11 - arch[k]; break;
        case ROW_ZIGZAG: s = zig[k]; break;
        case ROW_CHORD: s = 2 * (int32_t)k - 4; break;                     /* stacked thirds */
        case ROW_FIFTHS: s = (int32_t)((k * 4u) % 14u) - 2; break;        /* fifths, folded into two octaves */
        default: s = (int32_t)(rnd_next(&row_rnd) % 15u) - 2; break;      /* SCATTER */
        }
        pat.notes[k] = (int8_t)s;
    }
    pat.row = shape;
}

static int32_t octave_steps(void) { return scale_now.octave * (int32_t)scale_now.size; }

static void pendulums_retune(void)              /* the row through the scale: every pass (the scale may change) */
{
    for (uint32_t k = 0; k < PENDULUMS; k++) pend_pitch[k] = scale_step_pitch(pat.notes[k]);
}

/* ------------------------------------------------------------------------------------------------- the ARP */
static struct { uint8_t on, pattern, range, chance, latch; } arp = {0, PAT_UP, 1, 100, 0};
static uint8_t held[16], held_n, latched;       /* the keys (and MIDI notes) held, in the order pressed */
static uint8_t arp_hist[16], arp_hist_n;        /* what the ARP played last (0..255 high; 0xFF rest), for the screen */

static void arp_publish(void)                   /* the held notes, low to high, RANGE octaves of them -> the ISR */
{
    int32_t base[16], list[32];
    uint32_t n = 0, total = 0;
    for (uint32_t i = 0; i < held_n; i++) {
        int32_t s = scale_note_step(held[i]), j = (int32_t)n;
        uint8_t dup = 0;
        for (uint32_t q = 0; q < n; q++) dup |= base[q] == s;
        if (dup) continue;
        while (j > 0 && base[j - 1] > s) { base[j] = base[j - 1]; j--; }
        base[j] = s;
        n++;
    }
    for (uint32_t o = 0; o < arp.range; o++)
        for (uint32_t i = 0; i < n && total < 32u; i++) list[total++] = scale_step_pitch(base[i] + (int32_t)(o * scale_now.size));
    for (uint32_t i = 0; i < total; i++) arp_list[i] = list[i];
    arp_n = arp.on ? total : 0u;
    arp_on = arp.on;
    arp_pattern = arp.pattern;
    arp_chance = arp.chance;
}

/* ---------------------------------------------------------------------------------- the keys and MIDI in */
static uint8_t page_is_glo;
static const char *message;
static uint32_t message_at;
static void say(const char *text) { message = text; message_at = fm1_ms; }

static void live_note(uint32_t on, uint32_t note, uint32_t vel)
{
    if (on) {
        if (arp.on && arp.latch && latched) { held_n = 0; latched = 0; }   /* a new chord after a latched one */
        if (held_n < 16u) held[held_n++] = (uint8_t)note;
        if (rec_on) {                           /* REC: into the row, then on to the next pendulum */
            pat.notes[picked] = (int8_t)clampi(scale_note_step((int32_t)note), -24, 40);
            uint32_t count = pat.count ? pat.count : 1u;
            picked = (uint8_t)((picked + 1u) % count);
        }
        if (!arp.on || rec_on) cmd_put(1, (uint8_t)(note + 1u), scale_step_pitch(scale_note_step((int32_t)note)), (int32_t)vel * 258);
    } else {
        cmd_put(0, (uint8_t)(note + 1u), 0, 0);
        if (arp.on && arp.latch) {              /* LATCH: the chord stays once every key is up */
            uint32_t down = 0;
            for (uint32_t k = 0; k < 27u; k++) down += fm1_in.notes >> k & 1u;
            if (!down) { latched = 1; return; }
        }
        uint32_t j = 0;
        for (uint32_t i = 0; i < held_n; i++) if (held[i] != note) held[j++] = held[i];
        held_n = (uint8_t)j;
    }
}

static void live_panic(void)
{
    for (uint32_t n = 0; n < 128u; n++) cmd_put(0, (uint8_t)(n + 1u), 0, 0);
    held_n = 0;
    latched = 0;
}

/* ---------------------------------------------------------------------------------------------- presets
 * 8 to start from, and 8 USER slots that SAVE fills (kept in flash). A preset is the whole instrument: the bobs, the
 * swing, the row and the scale. */
#define FACTORY 8u
#define USERS 8u
static const char *const factory_names[FACTORY] = {
    "PLUMB LINE", "MUSIC BOX", "FOUCAULT", "TIDE CLOCK", "LEAD LINE", "SHIP'S BELL", "SLACK WATER", "SEXTANT"
};
/*  eng          k1  k2 spark width  A   D   S   R   lfo target, wave, rate, depth  rev echo time bloom
 *  cycle swings strike          grid count damp row        scale */
static const patch_t factory[FACTORY] = {
    {ENG_FOLD,   25, 50, 45, 70,   1, 18,  0, 20, LFO_TIMBRE, LW_SINE, 20, 20,     45, 20, 4, 50,
     7, 3, STRIKE_MIDDLE, 0, 12, 0, ROW_RISE, 3, {0}},
    {ENG_PULSAR, 30, 60, 50, 80,   1, 16,  0, 18, LFO_PITCH, LW_SINE, 40, 6,       35, 25, 3, 40,
     4, 8, STRIKE_WALLS, 3, 12, 0, ROW_ARCH, 1, {0}},
    {ENG_PHASE,  20, SHAPE_SAW, 60, 60, 2, 21, 10, 22, LFO_GRAVITY, LW_SINE, 6, 40, 50, 15, 4, 60,
     7, 4, STRIKE_LEFT, 0, 10, 0, ROW_CHORD, 8, {0}},
    {ENG_FOLD,   10, 70, 70, 90,   3, 22, 20, 24, LFO_TIMBRE, LW_DRIFT, 14, 30,    60, 30, 6, 70,
     8, 3, STRIKE_ONE_WAY, 0, 12, 0, ROW_VALLEY, 19, {0}},
    {ENG_PHASE,  35, SHAPE_PULSE, 40, 50, 0, 15, 0, 16, LFO_LEVEL, LW_TRIANGLE, 30, 15, 30, 35, 1, 30,
     4, 9, STRIKE_MIDDLE, 3, 12, 0, ROW_ZIGZAG, 4, {0}},
    {ENG_PULSAR, 55, 30, 30, 60,   1, 24, 0, 26, LFO_TIMBRE, LW_SINE, 10, 35,      65, 20, 5, 60,
     6, 2, STRIKE_LEFT, 0, 8, 6, ROW_FIFTHS, 25, {0}},
    {ENG_FOLD,   40, 30, 20, 100,  12, 20, 40, 27, LFO_GRAVITY, LW_DRIFT, 8, 60,   70, 10, 4, 80,
     8, 1, STRIKE_MIDDLE, 0, 12, 0, ROW_RISE, 24, {0}},
    {ENG_PHASE,  15, SHAPE_SQUARE, 70, 80, 1, 19, 0, 21, LFO_PITCH, LW_RANDOM, 36, 4, 40, 30, 2, 40,
     5, 5, STRIKE_WALLS, 5, 12, 0, ROW_SCATTER, 29, {0}},
};
static struct { uint32_t magic; patch_t user[USERS]; } store;
#define PRESET_MAGIC 0x504C4D31u                /* "PLM1" */
#define PRESET_BLOB 0u
static uint8_t preset, last_user = FACTORY;

static void preset_name(uint8_t p, char *b)
{
    if (p < FACTORY) { put_str(b, factory_names[p]); return; }
    put_str(b, "USER ");
    b[5] = (char)('1' + p - FACTORY);
    b[6] = 0;
}

/* a patch read from flash is checked value by value: a damaged slot never points past the end of a table */
static int patch_ok(const patch_t *s)
{
    if (!(s->engine < ENGINES && s->k1 <= 100 && s->k2 <= 100 && s->spark <= 100 && s->width <= 100 &&
          s->attack < ENV_STEPS && s->decay < ENV_STEPS && s->sustain <= 100 && s->release < ENV_STEPS &&
          s->lfo_target < LFO_TARGETS && s->lfo_wave < LFO_WAVES && s->lfo_rate < LFO_RATES && s->lfo_depth <= 100 &&
          s->reverb <= 100 && s->echo <= 100 && s->echo_time < ECHOS && s->bloom <= 100 &&
          s->cycle < CYCLES && s->swings >= 1 && s->swings <= 24 && s->strike < STRIKES && s->grid < GRIDS &&
          s->count >= 1 && s->count <= PENDULUMS && s->damp < DAMPS && s->row < ROWS && s->scale < SCALE_COUNT))
        return 0;
    for (uint32_t k = 0; k < PENDULUMS; k++) if (s->notes[k] < -24 || s->notes[k] > 40) return 0;
    return 1;
}

static void preset_load(uint8_t p)
{
    if (p >= FACTORY + USERS) p = 0;
    preset = p;
    patch_t next = p < FACTORY ? factory[p] : store.user[p - FACTORY];
    if (!patch_ok(&next) && p >= FACTORY) next = factory[p - FACTORY];
    uint8_t keep_row = p >= FACTORY;
    pat = next;
    if (!keep_row) {                            /* a factory preset: its row from its shape */
        row_rnd = 0x9e3779b9u * (p + 1u);
        row_shape(pat.row);
    }
    scale_select(pat.scale);
    if (p >= FACTORY) last_user = p;
    if (picked >= pat.count) picked = 0;
}

static void presets_init(void)
{
    if (blob_load(PRESET_BLOB, &store, sizeof store) < 0 || store.magic != PRESET_MAGIC) {
        store.magic = PRESET_MAGIC;
        for (uint32_t i = 0; i < USERS; i++) { preset_load((uint8_t)i); store.user[i] = pat; }
    }
    for (uint32_t i = 0; i < USERS; i++)
        if (!patch_ok(&store.user[i])) { preset_load((uint8_t)i); store.user[i] = pat; }
}

static void midi_program(uint32_t p) { preset_load((uint8_t)(p % (FACTORY + USERS))); }

/* -------------------------------------------------------------------------------------------- the screen
 * As LOOPHEADS, Endless Memory, BUOY: IBM Plex Mono, the title and the battery on top, then the CYCLE line (on every
 * page: where the pendulums are in their cycle, and when they meet), a picture, the four knobs numbered underneath,
 * a hint at the bottom. Two colours carry the idea: brass is the bobs (the notes, the sound), chalk blue is the swing
 * (time). */
#include "ui_font.h"
#include "plumb_boot_logo.h"

#define C_BG RGB(10, 16, 22)
#define C_BOB RGB(226, 176, 84)
#define C_BOB_DIM RGB(110, 86, 44)
#define C_TIME RGB(122, 168, 240)
#define C_TIME_DIM RGB(48, 70, 108)
#define C_KEY RGB(236, 232, 222)
#define C_OFF RGB(140, 152, 160)
#define C_RULE RGB(44, 58, 68)
#define C_GRAPH RGB(22, 33, 42)
#define C_REC RGB(255, 96, 96)
#define UI_LEFT 15u
#define UI_WIDTH 210u
#define UI_FOOTER_Y 218u
#define UI_BATTERY_WIDTH 27u
#define LINE_Y 46u                              /* the cycle line: 210 x 16 */
#define PIC_Y 68u                               /* the picture: 224 x 60 */
#define PIC_H 60u
#define ROW_Y 134u                              /* the four knob rows */
#define ROW_H 19u
#define BOOT_LOGO_MS 1800u
#define BOX_X 8u
#define BOX_W 224u

enum { P_HOME, P_ARP, P_SEQ, P_EDIT, P_ENV, P_LFO, P_FX, P_GLO };

static void ui_text(uint32_t x, uint32_t y, uint32_t width, const felucca_font_t *font,
                    const char *text, uint16_t color, int align)
{
    draw_text_box_bg(x, y, width, font, text, color, align, C_BG);
}

static void ui_divider(uint32_t y, uint16_t color) { lcd_fill(UI_LEFT, y, UI_WIDTH, 1, color); }
static void ui_footer(const char *hint, uint16_t color) { ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, hint, color, 1); }

static void ui_row(uint32_t knob, const char *label, const char *value, uint16_t value_colour)
{
    char n[3] = {(char)('1' + knob), ' ', 0};
    cv_begin(UI_WIDTH, UI_BODY.h, C_BG);
    if (*label) {
        int32_t x = cv_text(0, 0, &UI_BODY, n, C_OFF, C_BG);
        cv_text(x, 0, &UI_BODY, label, C_KEY, C_BG);
        cv_text((int32_t)UI_WIDTH - text_w(&UI_BODY, value), 0, &UI_BODY, value, value_colour, C_BG);
    }
    cv_blit(UI_LEFT, ROW_Y + knob * ROW_H);
}

#if !BOARD_SMK37
static uint8_t read_battery_level(uint32_t now)
{
    static uint32_t last_read;
    static int32_t average;
    static uint8_t level = UINT8_MAX;
    if (now - last_read >= 250u) {
        last_read = now;
        int32_t raw = fm1_adc_read(FM1_ADC_BATT);
        if (raw > 0) {
            average = average ? average + (raw * 16 - average) / 4 : raw * 16;
            level = average >= 591 * 16 ? 3 : average >= 561 * 16 ? 2 : average >= 531 * 16 ? 1 : 0;
        }
    }
    return level;
}

static void draw_battery(uint8_t level)
{
    uint16_t outline = level == UINT8_MAX ? C_RULE : C_OFF;
    cv_begin(UI_BATTERY_WIDTH, 12, C_BG);
    cv_rect(0, 0, 24, 12, outline);
    cv_rect(1, 1, 22, 10, C_BG);
    cv_rect(24, 4, 3, 4, outline);
    if (level != UINT8_MAX)
        for (uint32_t i = 0; i < level; i++) cv_rect(3 + i * 6, 3, 5, 6, C_BOB);
    cv_blit(UI_LEFT + UI_WIDTH - UI_BATTERY_WIDTH, 17);
}
#endif

static void draw_boot_logo(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    uint32_t run = 0, remaining = 0;
    uint16_t color = 0;
    for (uint32_t y = 0; y < BOOT_LOGO_HEIGHT; y += cv_h) {
        cv_begin(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT - y, C_BG);
        uint16_t *pixel = cv_px;
        uint32_t pixels = cv_w * cv_h;
        while (pixels) {
            if (!remaining) {
                remaining = boot_logo_runs[run++];
                color = swap16(boot_logo_runs[run++]);
            }
            uint32_t count = remaining < pixels ? remaining : pixels;
            remaining -= count;
            pixels -= count;
            while (count--) *pixel++ = color;
        }
        cv_blit(BOOT_LOGO_X, BOOT_LOGO_Y + y);
    }
    ui_text(UI_LEFT, 214, UI_WIDTH, &UI_SMALL, "v" APP_VER, C_OFF, 1);
}

static void fmt(char *b, int32_t v, const char *unit) { put_str(put_num(b, v, 0), unit); }

static void fmt_ms(char *b, uint32_t ms)
{
    if (ms < 1000u) { fmt(b, (int32_t)ms, " MS"); return; }
    char *p = put_num(b, (int32_t)(ms / 1000u), 0);
    if (ms < 10000u && ms % 1000u) { *p++ = '.'; *p++ = (char)('0' + ms % 1000u / 100u); }
    put_str(p, " S");
}

static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *pitch_name(char *b, int32_t pitch)  /* 1/256 semitones -> "G4" */
{
    int32_t n = (pitch + 128) >> 8;
    char *p = put_str(b, note_names[n % 12]);
    return put_num(p, n / 12 - 1, 0);
}

static void cv_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* LOOPHEADS' frame */
{
    cv_rect(x, y, w, 2, c);
    cv_rect(x, y + h - 2, w, 2, c);
    cv_rect(x, y + 2, 2, h - 4, c);
    cv_rect(x + w - 2, y + 2, 2, h - 4, c);
}

static void cv_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    for (int32_t dy = -r; dy <= r; dy++)
        for (int32_t dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r / 2) cv_pset(cx + dx, cy + dy, c);
}

static void cv_circle(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    for (uint32_t a = 0; a < 64u; a++) {
        uint32_t ph = a << 26;
        cv_pset(cx + sinq(ph + 0x40000000u) * r / 32768, cy + sinq(ph) * r / 32768, c);
    }
}

static uint32_t cycle_ticks(void) { return (uint32_t)cycle_beats[pat.cycle] * CLK_PPQN; }

/* --- the CYCLE line, on every page: the beat of the cycle, a bar, and when they meet */
static void draw_cycle_line(uint8_t time_page)
{
    char b[16];
    uint32_t beats = cycle_beats[pat.cycle], running = pd_running;
    uint32_t beat = (uint32_t)(((uint64_t)pd_c32 * beats) >> 32);
    char *p = put_str(b, "");
    if (clk_run && running) { p = put_num(p, (int32_t)beat + 1, 0); *p++ = '/'; put_num(p, (int32_t)beats, 0); }
    else put_str(b, clk_run ? "STILL" : "STOPPED");
    uint16_t c = time_page ? C_TIME : C_TIME_DIM, ct = clk_run && running ? C_TIME : C_OFF;
    cv_begin(UI_WIDTH, 16, C_BG);
    int32_t wl = cv_text(0, 1, &UI_SMALL, b, ct, C_BG);
    const char *meet = "MEET";
    int32_t wm = text_w(&UI_SMALL, meet);
    int32_t x0 = 62 > wl + 6 ? 62 : wl + 6, x1 = (int32_t)UI_WIDTH - wm - 8;
    cv_rect(x0, 8, x1 - x0, 1, C_RULE);
    uint32_t every = beats > 64u ? 8u : beats > 32u ? 4u : beats > 16u ? 2u : 1u;
    for (uint32_t i = 0; i <= beats; i += every) cv_rect(x0 + (int32_t)i * (x1 - x0) / (int32_t)beats, 6, 1, 5, C_RULE);
    int32_t px = x0 + (int32_t)(((uint64_t)pd_c32 * (uint32_t)(x1 - x0)) >> 32);
    if (clk_run && running) {
        cv_rect(x0, 7, px - x0, 3, c);
        cv_disc(px, 8, 3, C_BOB);
    }
    uint8_t near = clk_run && running && (pd_c32 < 0x0A000000u || pd_c32 > 0xF6000000u);
    cv_text((int32_t)UI_WIDTH - wm, 1, &UI_SMALL, meet, near ? C_KEY : C_OFF, C_BG);
    cv_blit(UI_LEFT, LINE_Y);
}

/* --- HOME: the row, seen from above. Each bob swings across its lane; the blue line is where it strikes. */
static int32_t lane_x(uint32_t k) { return 12 + (int32_t)k * 200 / (int32_t)(PENDULUMS - 1u); }

static void draw_row_pic(void)
{
    cv_begin(BOX_W, PIC_H, C_BG);
    uint32_t count = pat.count, amp = pd_amp, c32 = pd_c32;
    int32_t A = 24 * (int32_t)(amp >> 8) / 256;
    /* where they strike */
    if (pat.strike == STRIKE_MIDDLE || pat.strike == STRIKE_ONE_WAY) {
        for (int32_t x = 0; x < (int32_t)BOX_W; x += 4) cv_rect(x, 30, 2, 1, C_TIME_DIM);
        if (pat.strike == STRIKE_ONE_WAY) { cv_line(BOX_W - 6, 27, BOX_W - 3, 30, C_TIME_DIM); cv_line(BOX_W - 6, 33, BOX_W - 3, 30, C_TIME_DIM); }
    } else {
        cv_rect(0, 5, BOX_W, 1, C_TIME_DIM);
        if (pat.strike == STRIKE_WALLS) cv_rect(0, 55, BOX_W, 1, C_TIME_DIM);
    }
    int32_t ys[PENDULUMS];
    for (uint32_t k = 0; k < PENDULUMS; k++) {
        uint32_t ph = pend_phase(c32, pd.grav, k);
        ys[k] = 30 - sinq(ph + 0x40000000u) * A / 32768;   /* 0 = at the left wall: up */
        if (pend_caught[k] || !pd_running) ys[k] = pend_caught[k] ? 30 : ys[k];
    }
    for (uint32_t k = 0; k < PENDULUMS; k++) {
        int32_t x = lane_x(k);
        for (int32_t y = 2; y < 58; y += 3) cv_pset(x, y, C_GRAPH);   /* the lane */
        if (k == picked) cv_box(x - 9, 0, 18, PIC_H, rec_on ? C_REC : C_BOB_DIM);
    }
    for (uint32_t k = 0; k + 1u < count; k++)  /* the snake */
        if (!pend_caught[k] && !pend_caught[k + 1u]) cv_line(lane_x(k), ys[k], lane_x(k + 1u), ys[k + 1u], C_TIME_DIM);
    for (uint32_t k = 0; k < PENDULUMS; k++) {
        int32_t x = lane_x(k);
        uint32_t age = fm1_ms - pend_flash[k];
        if (k >= count) { cv_circle(x, 30, 2, C_RULE); continue; }
        if (pend_caught[k]) { cv_circle(x, 30, 4, C_OFF); continue; }
        if (age < 220u) cv_circle(x, ys[k], 6 + (int32_t)age / 40, age < 110u ? C_KEY : C_BOB_DIM);   /* it struck */
        cv_disc(x, ys[k], 4, age < 90u ? C_KEY : C_BOB);
    }
    cv_blit(BOX_X, PIC_Y);
}

static void draw_home_rows(void)
{
    char v[24], *p = v;
    *p++ = (char)(picked < 9 ? '1' + picked : '1');
    if (picked >= 9) *p++ = (char)('0' + picked - 9);
    p = put_str(p, ": ");
    pitch_name(p, scale_step_pitch(pat.notes[picked]));
    ui_row(0, "NOTE", v, C_BOB);
    ui_row(1, "ROW", row_names[pat.row], C_BOB);
    fmt(v, pat.count, "");
    ui_row(2, "PENDULUMS", v, C_TIME);
    if (pat.damp) fmt(v, damp_cycles[pat.damp], damp_cycles[pat.damp] == 1 ? " CYCLE" : " CYCLES");
    ui_row(3, "DAMP", pat.damp ? v : "NEVER", C_TIME);
}

/* --- SEQ: the whole cycle at once. A lane a pendulum, a mark at each strike; the blue line is now. The columns
 * where many marks line up are where they meet. */
static void draw_cycle_pic(void)
{
    static const uint32_t ths[STRIKES][2] = {{0x40000000u, 0xC0000000u}, {0x40000000u, 0}, {0, 0}, {0, 0x80000000u}};
    uint32_t nth = pat.strike == STRIKE_MIDDLE || pat.strike == STRIKE_WALLS ? 2u : 1u;
    uint32_t count = pat.count, T = cycle_ticks(), g = grid_ticks[pat.grid];
    int32_t W = (int32_t)BOX_W - 4, h = (int32_t)PIC_H / (int32_t)count;
    int32_t now = 2 + (int32_t)(((uint64_t)pd_c32 * (uint32_t)W) >> 32);
    cv_begin(BOX_W, PIC_H, C_BG);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t n = pat.swings + k, step = 0xFFFFFFFFu / n;
        int32_t y = (int32_t)k * h + (PIC_H - h * (int32_t)count) / 2;
        uint16_t lit = pend_caught[k] ? C_RULE : C_BOB, dim = pend_caught[k] ? C_GRAPH : C_BOB_DIM;
        if (k == picked) cv_rect(0, y + h / 2, 2, 1, C_BOB);
        for (uint32_t m = 0; m < n; m++)
            for (uint32_t i = 0; i < nth; i++) {
                uint32_t c = m * step + ths[pat.strike][i] / n;
                if (g) {                        /* GRID: on the next grid line */
                    uint32_t tick = (uint32_t)(((uint64_t)c * T) >> 32);
                    tick = (tick + g) / g * g;
                    if (ths[pat.strike][i] == 0 && m == 0) tick = 0;
                    if (tick >= T) tick -= T;
                    c = tick * (0xFFFFFFFFu / T);
                }
                int32_t x = 2 + (int32_t)(((uint64_t)c * (uint32_t)W) >> 32);
                cv_rect(x, y + 1, 1, h > 3 ? h - 1 : h, x <= now && clk_run ? lit : dim);
            }
    }
    if (clk_run && pd_running) cv_rect(now, 0, 1, PIC_H, C_TIME);
    cv_blit(BOX_X, PIC_Y);
}

static void draw_seq_rows(void)
{
    char v[20];
    fmt(v, cycle_beats[pat.cycle], " BEATS");
    ui_row(0, "CYCLE", v, C_TIME);
    char *p = put_num(v, pat.swings, 0);
    p = put_str(p, " TO ");
    put_num(p, pat.swings + (int32_t)pat.count - 1, 0);
    ui_row(1, "SWINGS", v, C_TIME);
    ui_row(2, "STRIKE", strike_names[pat.strike], C_TIME);
    ui_row(3, "GRID", grid_names[pat.grid], C_TIME);
}

/* --- ARP: what the pendulums played from your keys (height: the note), and the notes held */
static void draw_arp_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < 16u; i++) {
        int32_t x = (int32_t)UI_WIDTH - 13 - (int32_t)i * 13;
        if (i >= arp_hist_n) { cv_rect(x, 40, 10, 1, C_RULE); continue; }
        int32_t y = 40 - arp_hist[i] * 34 / 255;
        cv_rect(x, y, 10, 3, i == 0 && arp.on ? C_BOB : C_BOB_DIM);
    }
    char b[48], *p = b;
    *p = 0;
    if (!held_n) put_str(b, arp.on ? "HOLD KEYS" : "ARP OFF: PRESS ARP AGAIN");
    else for (uint32_t i = 0; i < held_n && i < 6u; i++) {
        p = pitch_name(p, scale_step_pitch(scale_note_step(held[i])));
        *p++ = ' '; *p = 0;
    }
    cv_text(0, 47, &UI_SMALL, b, held_n ? C_BOB : C_OFF, C_BG);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_arp_rows(void)
{
    char v[12];
    ui_row(0, "PATTERN", pattern_names[arp.pattern], C_BOB);
    fmt(v, arp.range, " OCT");
    ui_row(1, "RANGE", v, C_BOB);
    fmt(v, arp.chance, "%");
    ui_row(2, "CHANCE", v, C_BOB);
    ui_row(3, "LATCH", arp.latch ? "ON" : "OFF", C_BOB);
}

/* --- EDIT: one cycle of the bob's wave, at rest (dim) and at the strike (bright: SPARK opens it) */
static int32_t wave_at(uint32_t ph, int32_t tim)
{
    if (pat.engine == ENG_FOLD) {
        int32_t gain_q8 = 256 + tim * 9 / 128, sym = ((int32_t)pat.k2 - 50) * 400;
        int32_t v = (int32_t)(((int64_t)sinq(ph) * gain_q8) >> 8) + sym;
        uint32_t u = (uint32_t)(v + 32768) & 0x1FFFFu;
        if (u > 65536u) u = 131072u - u;
        return (int32_t)u - 32768;
    }
    if (pat.engine == ENG_PHASE) {
        int32_t d = mul15(tim, 31500);
        uint32_t shape = pat.k2 % SHAPES, p16 = ph >> 16, w, knee;
        if (shape == SHAPE_DOUBLE) p16 = (p16 << 1) & 0xFFFFu;
        if (shape == SHAPE_SQUARE) {
            knee = (uint32_t)(32768 - d); if (knee < 600u) knee = 600u;
            uint32_t h = p16 & 0x7FFFu, hold = 32768u - knee;
            uint32_t q = h < hold ? 0u : ((h - hold) * ((32768u << 8) / knee)) >> 8;
            w = (p16 & 0x8000u) + (q > 32767u ? 32767u : q);
        } else if (shape == SHAPE_PULSE) {
            knee = (uint32_t)(65536 - 2 * d); if (knee < 600u) knee = 600u;
            w = p16 < knee ? (p16 * ((65535u << 8) / knee)) >> 8 : 65535u;
        } else {
            knee = (uint32_t)(32768 - d); if (knee < 600u) knee = 600u;
            w = p16 < knee ? (p16 * ((32768u << 16) / knee)) >> 16 : 32768u + (((p16 - knee) * ((32768u << 16) / (65536u - knee))) >> 16);
        }
        int32_t s = sinq((w << 16) + 0x40000000u);
        return shape == SHAPE_DOUBLE && (ph >> 31) ? s / 2 : s;
    }
    uint32_t du = 65536u - (uint32_t)pat.k2 * 620u;
    if (du < 2000u) du = 2000u;
    uint32_t rd = (65536u << 8) / du, local = ((ph >> 16) * rd) >> 8;
    if (local >= 65536u) return 0;
    uint32_t fq8 = 256u + (uint32_t)tim * 11u / 128u;
    return mul15(sinq((local * fq8) << 8), sinq(local << 15));
}

static void draw_edit_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 30, UI_WIDTH, 1, C_GRAPH);
    int32_t rest = clampi((int32_t)pat.k1 * 327, 0, 32767);
    int32_t hit = clampi((int32_t)pat.k1 * 327 + (int32_t)pat.spark * 327, 0, 32767);
    for (uint32_t pass = 0; pass < 2u; pass++) {
        int32_t tim = pass ? hit : rest, py = 30;
        for (uint32_t x = 0; x < UI_WIDTH; x++) {
            uint32_t ph = x * (0xFFFFFFFFu / UI_WIDTH);
            int32_t y = 30 - wave_at(ph, tim) * 26 / 32768;
            if (x) cv_line((int32_t)x - 1, py, (int32_t)x, y, pass ? C_BOB : C_BOB_DIM);
            py = y;
        }
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_edit_rows(void)
{
    char v[16];
    if (pat.engine == ENG_FOLD) {
        fmt(v, pat.k1, "%");
        ui_row(0, "FOLDS", v, C_BOB);
        int32_t s = (int32_t)pat.k2 - 50;
        put_num(v, s * 2, 1);
        ui_row(1, "SYMMETRY", s ? v : "CENTRE", C_BOB);
    } else if (pat.engine == ENG_PHASE) {
        fmt(v, pat.k1, "%");
        ui_row(0, "DCW", v, C_BOB);
        ui_row(1, "SHAPE", shape_names[pat.k2 % SHAPES], C_BOB);
    } else {
        uint32_t f = 100u + (uint32_t)pat.k1 * 11u;   /* x100 */
        char *p = put_num(v, (int32_t)(f / 100u), 0);
        *p++ = '.'; *p++ = (char)('0' + f % 100u / 10u);
        put_str(p, " X");
        ui_row(0, "FORMANT", v, C_BOB);
        uint32_t du = 65536u - (uint32_t)pat.k2 * 620u;
        fmt(v, (int32_t)(du * 100u / 65536u), "%");
        ui_row(1, "DUTY", v, C_BOB);
    }
    fmt(v, pat.spark, "%");
    ui_row(2, "SPARK", v, C_BOB);
    fmt(v, pat.width, "%");
    ui_row(3, "WIDTH", v, C_BOB);
}

/* --- ENV: each strike */
static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static void draw_env_pic(void)
{
    int32_t times[] = {env_ms[pat.attack] + 60, env_ms[pat.decay] + 60, 900, env_ms[pat.release] + 60};
    int32_t sq[4];
    for (uint32_t i = 0; i < 4; i++) sq[i] = (int32_t)isqrt32((uint32_t)times[i] * 64u);
    int32_t total = sq[0] + sq[1] + sq[2] + sq[3];
    int32_t x[5] = {0}, elapsed = 0;
    for (uint32_t i = 0; i < 4; i++) { elapsed += sq[i]; x[i + 1] = elapsed * (int32_t)(UI_WIDTH - 1u) / total; }
    int32_t sustain = 56 - (int32_t)pat.sustain * 50 / 100;
    int32_t y[] = {56, 6, sustain, sustain, 56};
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < 4; i++) {
        int32_t width = x[i + 1] - x[i];
        for (int32_t col = 0; col < width; col++) {
            int32_t top = y[i] + (y[i + 1] - y[i]) * col / width;
            cv_rect(x[i] + col, top, 1, 57 - top, C_GRAPH);
        }
    }
    for (uint32_t i = 1; i < 4; i++)
        for (int32_t row = y[i] + 3; row < 56; row += 4) cv_pset(x[i], row, C_RULE);
    for (uint32_t i = 0; i < 4; i++) cv_line(x[i], y[i], x[i + 1], y[i + 1], C_BOB);
    cv_disc(x[0], 56, 3, C_TIME);              /* the strike */
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_env_rows(void)
{
    char v[12];
    fmt_ms(v, env_ms[pat.attack]);
    ui_row(0, "ATTACK", v, C_BOB);
    fmt_ms(v, env_ms[pat.decay]);
    ui_row(1, "DECAY", v, C_BOB);
    fmt(v, pat.sustain, "%");
    ui_row(2, "SUSTAIN", v, C_BOB);
    fmt_ms(v, env_ms[pat.release]);
    ui_row(3, "RELEASE", v, C_BOB);
}

/* --- LFO: the wave, and where it is now. TIMBRE is called after what it moves: FOLDS, DCW or FORMANT */
static const char *lfo_target_name(uint8_t t)
{
    return t == LFO_TIMBRE ? engine_k1_names[pat.engine % ENGINES] : lfo_target_names[t % LFO_TARGETS];
}

static void draw_lfo_pic(void)
{
    static const int16_t held_v[] = {18000, -11000, 29000, 7000, -28000, 13000, -3000, 24000};
    uint8_t grav = pat.lfo_target == LFO_GRAVITY;
    uint16_t c = grav ? C_TIME : C_BOB, cd = grav ? C_TIME_DIM : C_BOB_DIM;
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 30, UI_WIDTH, 1, C_RULE);
    int32_t depth = pat.lfo_depth, prev = 30;
    for (uint32_t x = 0; x < UI_WIDTH; x++) {
        uint32_t ph = x * (65536u * 2u / UI_WIDTH) % 65536u;
        int32_t s, m = (int32_t)ph - 32768, a = m < 0 ? -m : m;
        if (pat.lfo_wave == LW_SINE) s = m * (32768 - a) >> 13;
        else if (pat.lfo_wave == LW_TRIANGLE) s = 32767 - 2 * a;
        else if (pat.lfo_wave == LW_RANDOM) s = held_v[x * 8u / UI_WIDTH];
        else { int32_t i = (int32_t)(x * 8u / UI_WIDTH), f = (int32_t)(x * 8u % UI_WIDTH) * 256 / (int32_t)UI_WIDTH;
               s = held_v[i] + (held_v[(i + 1) & 7] - held_v[i]) * f / 256; }
        s = clampi(s, -32767, 32767);
        int32_t y = 30 - s * depth * 26 / (32768 * 100);
        if (x) cv_line((int32_t)x - 1, prev, (int32_t)x, y, depth ? c : cd);
        prev = y;
    }
    int32_t px = (int32_t)((lfo_ph >> 16) * (UI_WIDTH / 2u) >> 16);
    int32_t py = 30 - lfo_now * depth * 26 / (32768 * 100);
    cv_disc(px, py, 3, C_KEY);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_lfo_rows(void)
{
    char v[12];
    ui_row(0, "TARGET", lfo_target_name(pat.lfo_target), pat.lfo_target == LFO_GRAVITY ? C_TIME : C_BOB);
    ui_row(1, "WAVE", lfo_wave_names[pat.lfo_wave], C_BOB);
    uint16_t h = lfo_hundredths[pat.lfo_rate];
    char *p = put_num(v, h / 100u, 0);
    *p++ = '.';
    *p++ = (char)('0' + h % 100u / 10u);
    *p++ = (char)('0' + h % 10u);
    put_str(p, " HZ");
    ui_row(2, "RATE", v, C_BOB);
    fmt(v, pat.lfo_depth, "%");
    ui_row(3, "DEPTH", v, C_BOB);
}

/* BLOOM: how near the pendulums are to meeting, 0 .. 256 (256 at the meeting, 0 from a quarter cycle away) */
static int32_t bloom_near(uint32_t c32)
{
    uint32_t d = (c32 < 0x80000000u ? c32 : 0u - c32) >> 16;   /* 0 .. 32768: half a cycle */
    if (d >= 16384u) return 0;
    int32_t b = (int32_t)(16384u - d) / 64;     /* 0 .. 256 */
    return b * b >> 8;                          /* (eased in) */
}

/* --- FX: on the left the ROOM over one cycle (it opens as they meet, the dot is now); on the right the SWING ECHO,
 * its repeats going down in time, left, right, left, narrower and fainter until they rest in the middle */
static void draw_fx_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    const int32_t RW = 96, base = 46;
    cv_rect(0, base + 1, RW, 1, C_RULE);
    int32_t prev = 0, rest = 8 + pat.reverb * 14 / 100;
    for (int32_t x = 0; x < RW; x++) {
        uint32_t c = (uint32_t)x * (0xFFFFFFFFu / (uint32_t)RW);
        int32_t h = pat.reverb ? rest + bloom_near(c) * (int32_t)pat.bloom * 22 / (256 * 100) : 0;
        cv_rect(x, base - h, 1, h, C_GRAPH);
        if (x && pat.reverb) cv_line(x - 1, base - prev, x, base - h, C_TIME);
        prev = h;
    }
    if (pat.reverb && clk_run && pd_running) {
        int32_t x = (int32_t)(((uint64_t)pd_c32 * (uint32_t)RW) >> 32);
        int32_t h = rest + bloom_near(pd_c32) * (int32_t)pat.bloom * 22 / (256 * 100);
        cv_disc(x, base - h, 3, C_KEY);
    }
    cv_text(0, 48, &UI_SMALL, "ROOM", C_OFF, C_BG);
    /* the echo */
    const int32_t cx = 160;
    for (int32_t y = 0; y < 46; y += 3) cv_pset(cx, y, C_GRAPH);
    cv_disc(cx - 40, 3, 3, C_KEY);               /* the strike goes in on the left */
    int32_t w = 40 * 32768, level = pat.echo * 32768 / 100, px = cx - 40, py = 3;
    int32_t step = 4 + (int32_t)echo_clocks[pat.echo_time] / 3;
    for (int32_t i = 1; level > 1500 && 3 + i * step < 46; i++) {
        int32_t x = cx + ((i & 1) ? -1 : 1) * (w >> 15), y = 3 + i * step;
        cv_line(px, py, x, y, C_TIME_DIM);
        cv_disc(x, y, level > 16000 ? 3 : 2, level > 9000 ? C_BOB : C_BOB_DIM);
        px = x; py = y;
        w = w / 32768 * (32768 - 2 * 4096);     /* narrower each pass (ECHO_BLEED, both ways) */
        level = level * 55 / 100;
    }
    cv_text(RW + 18, 48, &UI_SMALL, "ECHO", C_OFF, C_BG);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_fx_rows(void)
{
    char v[12];
    fmt(v, pat.reverb, "%");
    ui_row(0, "REVERB", v, C_BOB);
    fmt(v, pat.bloom, "%");
    ui_row(1, "BLOOM", v, C_TIME);
    fmt(v, pat.echo, "%");
    ui_row(2, "ECHO", v, C_BOB);
    ui_row(3, "TIME", echo_names[pat.echo_time], C_TIME);
}

/* --- GLO: the scale, on a piano octave (the root in blue) */
static void draw_scale_pic(void)
{
    static const uint8_t white[] = {0, 2, 4, 5, 7, 9, 11};
    static const uint8_t black[] = {1, 3, 6, 8, 10};
    static const uint8_t black_x[] = {30, 60, 120, 150, 180};
    uint8_t root = scale_root_pc();
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_text(0, 0, &UI_BODY, scales[scale_now.scale].name, C_BOB, C_BG);
    for (uint32_t i = 0; i < 7; i++) {
        uint16_t c = scale_contains(white[i]) ? (white[i] == root ? C_TIME : C_BOB) : C_GRAPH;
        cv_rect((int32_t)i * 30, 20, 28, 40, C_RULE);
        cv_rect((int32_t)i * 30 + 1, 21, 26, 38, c);
    }
    for (uint32_t i = 0; i < 5; i++) {
        int32_t x = black_x[i] - 10;
        cv_rect(x, 20, 20, 24, C_RULE);
        cv_rect(x + 1, 20, 18, 23, C_BG);
        uint16_t c = scale_contains(black[i]) ? (black[i] == root ? C_TIME : C_BOB) : C_BG;
        cv_rect(x + 2, 21, 16, 21, c);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_glo_rows(void)
{
    char v[16];
    fmt(v, scale_now.size, scale_now.size == 1 ? " NOTE" : " NOTES");
    ui_row(0, "SCALE", v, C_BOB);
    uint32_t bpm10 = clock_bpm10();
    fmt(v, (int32_t)((bpm10 + 5u) / 10u), clk_ext() ? " MIDI" : " BPM");
    ui_row(1, "TEMPO", v, C_TIME);
    ui_row(2, "SYNC", set.sync_int ? "INTERNAL" : "AUTO", C_TIME);
    if (set.ch) fmt(v, set.ch, "");
    ui_row(3, "MIDI CH", set.ch ? v : "OMNI", C_KEY);
}

/* ------------------------------------------------------------------------------------- knobs and keys */
static int32_t step_clamp(int32_t v, int32_t d, int32_t lo, int32_t hi)
{
    v += d;
    return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t pct(uint8_t v, int32_t d) { return (uint8_t)step_clamp(v, d * 2, 0, 100); }

static void edit_page(uint8_t page, const int32_t s[4])
{
    switch (page) {
    case P_HOME:
        if (s[0]) pat.notes[picked] = (int8_t)step_clamp(pat.notes[picked], s[0], -24, 40);
        if (s[1]) row_shape((uint8_t)step_clamp(pat.row, s[1], 0, ROWS - 1));
        if (s[2]) { pat.count = (uint8_t)step_clamp(pat.count, s[2], 1, PENDULUMS); if (picked >= pat.count) picked = (uint8_t)(pat.count - 1u); }
        pat.damp = (uint8_t)step_clamp(pat.damp, s[3], 0, DAMPS - 1);
        break;
    case P_SEQ:
        pat.cycle = (uint8_t)step_clamp(pat.cycle, s[0], 0, CYCLES - 1);
        pat.swings = (uint8_t)step_clamp(pat.swings, s[1], 1, 24);
        pat.strike = (uint8_t)step_clamp(pat.strike, s[2], 0, STRIKES - 1);
        pat.grid = (uint8_t)step_clamp(pat.grid, s[3], 0, GRIDS - 1);
        break;
    case P_ARP:
        arp.pattern = (uint8_t)step_clamp(arp.pattern, s[0], 0, PATTERNS - 1);
        arp.range = (uint8_t)step_clamp(arp.range, s[1], 1, 4);
        arp.chance = (uint8_t)step_clamp(arp.chance, s[2] * 5, 0, 100);
        if (s[3]) { arp.latch = s[3] > 0; if (!arp.latch && latched) { held_n = 0; latched = 0; } }
        break;
    case P_EDIT:
        pat.k1 = pct(pat.k1, s[0]);
        if (pat.engine == ENG_PHASE) pat.k2 = (uint8_t)step_clamp(pat.k2 % SHAPES, s[1], 0, SHAPES - 1);
        else pat.k2 = pct(pat.k2, s[1]);
        pat.spark = pct(pat.spark, s[2]);
        pat.width = pct(pat.width, s[3]);
        break;
    case P_ENV:
        pat.attack = (uint8_t)step_clamp(pat.attack, s[0], 0, ENV_STEPS - 1);
        pat.decay = (uint8_t)step_clamp(pat.decay, s[1], 0, ENV_STEPS - 1);
        pat.sustain = (uint8_t)step_clamp(pat.sustain, s[2] * 5, 0, 100);
        pat.release = (uint8_t)step_clamp(pat.release, s[3], 0, ENV_STEPS - 1);
        break;
    case P_LFO:
        if (s[0]) {                             /* PITCH, FOLDS (DCW, FORMANT), SPARK, LEVEL, GRAVITY */
            int32_t at = 0;
            for (uint32_t i = 0; i < LFO_TARGETS; i++) if (lfo_order[i] == pat.lfo_target) at = (int32_t)i;
            pat.lfo_target = lfo_order[step_clamp(at, s[0], 0, LFO_TARGETS - 1)];
        }
        pat.lfo_wave = (uint8_t)step_clamp(pat.lfo_wave, s[1], 0, LFO_WAVES - 1);
        pat.lfo_rate = (uint8_t)step_clamp(pat.lfo_rate, s[2], 0, LFO_RATES - 1);
        pat.lfo_depth = pct(pat.lfo_depth, s[3]);
        break;
    case P_FX:
        pat.reverb = pct(pat.reverb, s[0]);
        pat.bloom = pct(pat.bloom, s[1]);
        pat.echo = pct(pat.echo, s[2]);
        pat.echo_time = (uint8_t)step_clamp(pat.echo_time, s[3], 0, ECHOS - 1);
        break;
    default:
        if (s[0]) { scale_select((uint8_t)step_clamp(scale_now.scale, s[0], 0, SCALE_COUNT - 1)); pat.scale = scale_now.scale; }
        if (s[1]) { set.tempo = (uint8_t)step_clamp(set.tempo, s[1], 40, 240); clk_tempo = set.tempo; }
        if (s[2]) { set.sync_int = s[2] > 0; clock_set_sync(set.sync_int); }
        if (s[3]) { set.ch = (uint8_t)step_clamp(set.ch, s[3], 0, 16); midi_ch = set.ch; }
        break;
    }
}

static void update_fx(void)
{
    static uint32_t previous = ~0u;
    uint32_t bpm = (clock_bpm10() + 5u) / 10u;
    if (bpm < 30u) bpm = 30u;
    if (bpm > 300u) bpm = 300u;
    /* BLOOM: the room is a little smaller and darker while they are scattered, and opens (longer, brighter, a
     * little louder) as they come back into a line */
    int32_t b = clk_run && pd_running ? bloom_near(pd_c32) * (int32_t)pat.bloom / 100 : 0;   /* 0 .. 256 */
    int32_t calm = clk_run && pd_running ? (int32_t)pat.bloom * 12 / 100 : 0;
    uint32_t decay = (uint32_t)(84 - calm + b * (30 + 12) / 256);     /* at most 114 (the hall stays stable < 117) */
    uint32_t damping = (uint32_t)(40 - b * 26 / 256);
    uint32_t mix = (uint32_t)pat.reverb + (uint32_t)(pat.reverb * b * 3 / (256 * 10));
    uint32_t state = bpm | (uint32_t)pat.echo << 9 | (uint32_t)pat.echo_time << 16 | decay << 19 | (damping & 63u) << 26;
    state ^= mix * 0x9e3779b9u;
    if (state == previous) return;
    previous = state;
    echo_configure(pat.echo != 0, echo_tempo_samples(bpm, echo_clocks[pat.echo_time]), 62, pat.echo);
    hall_configure(pat.reverb != 0, (uint8_t)decay, (uint8_t)damping, (uint8_t)mix);
}

static uint8_t button_page(uint32_t button)
{
    switch (button) {
    case BTN_HOME: return P_HOME;
    case BTN_ARP: return P_ARP;
    case BTN_SEQ: return P_SEQ;
    case BTN_EDIT: return P_EDIT;
    case BTN_ENV: return P_ENV;
    case BTN_LFO: return P_LFO;
    case BTN_FX: return P_FX;
    case BTN_GLO: return P_GLO;
    default: return 0xFF;
    }
}

static const char *page_title(uint8_t page, char *b)
{
    switch (page) {
    case P_HOME: return rec_on ? "HOME  REC" : "HOME";
    case P_ARP: return arp.on ? "ARP  ON" : "ARP  OFF";
    case P_SEQ: return clk_run ? "SEQ  SWINGING" : "SEQ  STOPPED";
    case P_EDIT: put_str(put_str(b, "EDIT  "), engine_names[pat.engine]); return b;
    case P_ENV: return "ENV";
    case P_LFO: return "LFO";
    case P_FX: return "FX";
    default: return "GLO  SCALE";
    }
}

static uint16_t title_colour(uint8_t page)
{
    switch (page) {
    case P_HOME: return rec_on ? C_REC : C_KEY;
    case P_SEQ: return C_TIME;
    case P_LFO: return pat.lfo_target == LFO_GRAVITY ? C_TIME : C_BOB;
    default: return C_BOB;
    }
}

static void draw_footer(uint8_t page)
{
    static char b[48];
    if (message) { ui_footer(message, C_BOB); return; }
    char *p;
    switch (page) {
    case P_HOME:
        if (rec_on) { p = put_str(b, "REC: KEYS INTO PENDULUM "); put_num(p, picked + 1, 0); ui_footer(b, C_REC); break; }
        if (!clk_run) { ui_footer("PLAY: RELEASE THEM", C_TIME); break; }
        p = put_str(b, pend_caught[picked] ? "SEL: LET " : "SEL: CATCH ");
        p = put_num(p, picked + 1, 0);
        put_str(p, pend_caught[picked] ? " SWING" : "   REC: ROW");
        ui_footer(b, C_OFF);
        break;
    case P_SEQ: {
        if (!clk_run) { ui_footer("PLAY: RELEASE THEM", C_OFF); break; }
        if (!pd_running) { ui_footer("THEY HAVE STOPPED: PLAY", C_OFF); break; }
        uint32_t beats = cycle_beats[pat.cycle], left = beats - (uint32_t)(((uint64_t)pd_c32 * beats) >> 32);
        p = put_str(b, "THEY MEET AGAIN IN ");
        p = put_num(p, (int32_t)left, 0);
        put_str(p, left == 1u ? " BEAT" : " BEATS");
        ui_footer(b, C_OFF);
        break;
    }
    case P_ARP: ui_footer(arp.on ? "HOLD KEYS: THE PENDULUMS PLAY" : "ARP: TURN THE ARP ON", C_OFF); break;
    case P_EDIT: ui_footer("SPARK: HOW THE STRIKE OPENS IT", C_OFF); break;
    case P_ENV: ui_footer("EVERY STRIKE", C_OFF); break;
    case P_LFO:
        ui_footer(pat.lfo_target == LFO_GRAVITY ? "GRAVITY BENDS THEIR TIME" :
                  pat.lfo_target == LFO_SPARK ? "SPARK: HOW EACH STRIKE OPENS" : "ONE LFO FOR ALL TWELVE", C_OFF);
        break;
    case P_FX: ui_footer("THE ROOM OPENS AS THEY MEET", C_OFF); break;
    default:
        p = put_str(b, "KEYS: TRANSPOSE   ROOT ");
        put_str(p, note_names[scale_root_pc()]);
        ui_footer(b, C_OFF);
    }
}

#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
static uint32_t page_state(uint8_t page, uint32_t now)
{
    uint32_t h = 2166136261u;
    MIX(picked); MIX(rec_on);
    switch (page) {
    case P_HOME:
        MIX(now / 30u); MIX(pat.count); MIX(pat.strike);
        for (uint32_t k = 0; k < PENDULUMS; k++) MIX(pend_caught[k]);
        break;
    case P_SEQ:
        MIX(pat.cycle); MIX(pat.swings); MIX(pat.strike); MIX(pat.grid); MIX(pat.count); MIX(clk_run); MIX(pd_running);
        MIX(pd_c32 >> 25);
        for (uint32_t k = 0; k < PENDULUMS; k++) MIX(pend_caught[k]);
        break;
    case P_EDIT: MIX(pat.engine); MIX(pat.k1); MIX(pat.k2); MIX(pat.spark); break;
    case P_ENV: MIX(pat.attack); MIX(pat.decay); MIX(pat.sustain); MIX(pat.release); break;
    case P_LFO: MIX(pat.lfo_wave); MIX(pat.lfo_depth); MIX(pat.lfo_target); MIX(now / 40u); break;
    case P_ARP:
        MIX(arp_hist_n); MIX(arp_hist[0]); MIX(arp_hist[1]); MIX(arp.on); MIX(held_n);
        for (uint32_t i = 0; i < held_n; i++) MIX(held[i]);
        MIX(scale_now.octave); MIX(scale_now.scale); MIX(scale_now.transpose);
        break;
    case P_FX: MIX(pat.reverb); MIX(pat.echo); MIX(pat.echo_time); MIX(pat.bloom); MIX(clk_run); MIX(pd_running); MIX(pd_c32 >> 26); break;
    default: MIX(scale_now.scale); MIX(scale_now.transpose); break;
    }
    return h;
}

static uint32_t row_state(uint8_t page)
{
    uint32_t h = 2166136261u;
    switch (page) {
    case P_HOME: MIX(picked); MIX(pat.notes[picked]); MIX(pat.row); MIX(pat.count); MIX(pat.damp); MIX(scale_now.scale); MIX(scale_now.transpose); break;
    case P_SEQ: MIX(pat.cycle); MIX(pat.swings); MIX(pat.strike); MIX(pat.grid); MIX(pat.count); break;
    case P_EDIT: MIX(pat.engine); MIX(pat.k1); MIX(pat.k2); MIX(pat.spark); MIX(pat.width); break;
    case P_ENV: MIX(pat.attack); MIX(pat.decay); MIX(pat.sustain); MIX(pat.release); break;
    case P_LFO: MIX(pat.lfo_target); MIX(pat.lfo_wave); MIX(pat.lfo_rate); MIX(pat.lfo_depth); MIX(pat.engine); break;
    case P_ARP: MIX(arp.pattern); MIX(arp.range); MIX(arp.chance); MIX(arp.latch); break;
    case P_FX: MIX(pat.reverb); MIX(pat.echo); MIX(pat.echo_time); MIX(pat.bloom); break;
    default: MIX(scale_now.scale); MIX(set.tempo); MIX(set.sync_int); MIX(set.ch); MIX(clock_bpm10()); MIX(clk_ext()); break;
    }
    return h;
}
#undef MIX

static void draw_page(uint8_t page, uint8_t whole)
{
    switch (page) {
    case P_HOME: draw_row_pic(); if (whole) draw_home_rows(); break;
    case P_SEQ: draw_cycle_pic(); if (whole) draw_seq_rows(); break;
    case P_ARP: draw_arp_pic(); if (whole) draw_arp_rows(); break;
    case P_EDIT: draw_edit_pic(); if (whole) draw_edit_rows(); break;
    case P_ENV: draw_env_pic(); if (whole) draw_env_rows(); break;
    case P_LFO: draw_lfo_pic(); if (whole) draw_lfo_rows(); break;
    case P_FX: draw_fx_pic(); if (whole) draw_fx_rows(); break;
    default: draw_scale_pic(); if (whole) draw_glo_rows(); break;
    }
}

static uint32_t idle_ok;
static void app_idle(void) { if (idle_ok) midi_poll(1); }

/* ----------------------------------------------------------------------------------------------- the loop */
static void app_main(void)
{
    uint32_t held_since[14] = {0};
    uint16_t long_done = 0, was_on_page = 0;
    uint8_t page = P_HOME, drawn_page = 0xFF, key_on[27] = {0};
    uint32_t prev_keys = 0, drawn_state = 0, drawn_rows = 0, drawn_footer = ~0u, title_state = ~0u, drawn_line = ~0u;
    uint32_t flash_until[27] = {0}, off_at[16] = {0};
    uint8_t off_note[16] = {0};
    static char msg[40];
#if !BOARD_SMK37
    uint8_t battery_drawn = UINT8_MAX;
#endif

    if (settings_load(&set, sizeof set) || set.ver != SET_VER) { set.ver = SET_VER; set.oct = 0; set.ch = 0; set.sync_int = 0; set.tempo = 96; }
    if ((int8_t)set.oct < -2 || (int8_t)set.oct > 2) set.oct = 0;
    if (set.ch > 16u) set.ch = 0;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 96;
    set.sync_int &= 1u;
    scale_now.octave = (int8_t)set.oct;
    midi_ch = set.ch;
    clk_tempo = set.tempo;
    clock_set_sync(set.sync_int);
    presets_init();
    preset_load(0);
    pendulums_retune();

    draw_boot_logo();
    uint32_t boot_started = fm1_ms;
    /* (nothing plays at power-on: PLAY releases them) */
    for (;;) {
        if (mirror_poll()) drawn_page = 0xFF;   /* FM-1 Mirror opened: everything again */
        if (backup_poll()) { platform_poll(); continue; }
        platform_poll();
        clock_poll();
        uint32_t now = fm1_ms, rel;
        uint32_t b = fm1_input_edges(&rel);
        uint32_t held_b = fm1_in.buttons;

        /* ---- buttons: pages on press; hold actions; taps on release */
        for (uint32_t i = 0; i < 14; i++) {
            uint32_t bit = 1u << i;
            if (b & bit) {
                held_since[i] = now;
                long_done &= (uint16_t)~bit;
                uint8_t to = button_page(i);
                if (to != 0xFF) {
                    if (page == to) was_on_page |= (uint16_t)bit; else was_on_page &= (uint16_t)~bit;
                    page = to;
                }
            }
            if ((held_b & bit) && !(long_done & bit) && now - held_since[i] >= LONG_MS && !(b & bit)) {
                long_done |= (uint16_t)bit;
                if (i == BTN_REC) {             /* hold REC: the row back to its shape */
                    rec_on = 0;
                    row_shape(pat.row);
                    say("ROW BACK TO ITS SHAPE");
                }
                if (i == BTN_SEL) {             /* hold SEL: every pendulum swings again */
                    for (uint32_t k = 0; k < PENDULUMS; k++) pend_caught[k] = 0;
                    say("ALL SWING");
                }
            }
            if ((rel & bit) && !(long_done & bit)) {
                if (i == BTN_ARP && (was_on_page & bit)) {
                    arp.on = !arp.on;
                    held_n = 0;
                    latched = 0;
                    live_panic();
                    say(arp.on ? "THE PENDULUMS PLAY YOUR KEYS" : "THE PENDULUMS PLAY THE ROW");
                }
                if (i == BTN_PLAY) clock_toggle();   /* PLAY releases them from the line; STOP catches them */
                if (i == BTN_REC) {
                    rec_on = !rec_on;
                    if (rec_on) { page = P_HOME; say("PLAY KEYS INTO THE ROW"); }
                }
                if (i == BTN_SEL) {             /* SEL: catch the picked pendulum, or let it swing */
                    pend_caught[picked] = !pend_caught[picked];
                    char *p = put_str(msg, "PENDULUM ");
                    p = put_num(p, picked + 1, 0);
                    put_str(p, pend_caught[picked] ? " CAUGHT" : " SWINGS");
                    say(msg);
                }
            }
        }
        /* SAVE: the whole instrument into a USER preset (and the settings) */
        if (b & (1u << BTN_SAVE)) {
            uint8_t slot = preset >= FACTORY ? preset : last_user;
            pat.scale = scale_now.scale;
            store.user[slot - FACTORY] = pat;
            store.magic = PRESET_MAGIC;
            set.oct = (uint8_t)scale_now.octave;
            int ok = blob_save(PRESET_BLOB, &store, sizeof store) >= 0;
            settings_save(&set, sizeof set);
            preset = last_user = slot;
            char *p = put_str(msg, ok ? "SAVED TO " : "COULD NOT SAVE ");
            preset_name(slot, p);
            say(msg);
        }
        if (b & (1u << BTN_OCT_DOWN) || b & (1u << BTN_OCT_UP)) {
            int32_t d = b & (1u << BTN_OCT_UP) ? 1 : -1;
            if (page == P_GLO) scale_now.transpose = (int8_t)step_clamp(scale_now.transpose, d * 12, -31, 43);
            else scale_now.octave = (int8_t)step_clamp(scale_now.octave, d, -2, 2);
        }

        /* ---- encoders: SELECT a pendulum (GLO: the scale); ALGORITHM the engine; PRESETS */
        int32_t e = fm1_enc_take(ENC_SELECT);
        if (e) {
            if (page == P_GLO) { scale_select((uint8_t)step_clamp(scale_now.scale, e, 0, SCALE_COUNT - 1)); pat.scale = scale_now.scale; }
            else picked = (uint8_t)step_clamp(picked, e, 0, (int32_t)pat.count - 1);
        }
        e = fm1_enc_take(ENC_ALGORITHM);
        if (e) {
            uint8_t was = pat.engine;
            pat.engine = (uint8_t)step_clamp(pat.engine, e, 0, ENGINES - 1);
            if (pat.engine != was) {
                if (pat.engine == ENG_PHASE) pat.k2 = SHAPE_SAW;
                else if (was == ENG_PHASE) pat.k2 = 50;
            }
            put_str(put_str(msg, "THE BOBS: "), engine_names[pat.engine]);
            say(msg);
        }
        e = fm1_enc_take(ENC_PRESETS);
        if (e) {
            preset_load((uint8_t)step_clamp(preset, e, 0, FACTORY + USERS - 1));
            char *p = put_str(msg, "PRESET ");
            preset_name(preset, p);
            say(msg);
        }
        int32_t steps[4];
        for (uint32_t k = 0; k < 4; k++) steps[k] = fm1_enc_take(ENC_KNOB1 + k);
        if (steps[0] || steps[1] || steps[2] || steps[3]) edit_page(page, steps);

        /* ---- the keys: they play (or feed the ARP, or write the row) everywhere but GLO, where they transpose */
        uint32_t keys = fm1_in.notes, down = keys & ~prev_keys, up = prev_keys & ~keys;
        prev_keys = keys;
        page_is_glo = page == P_GLO;
        for (uint32_t k = 0; k < 27u; k++) {
            uint32_t bit = 1u << k;
            if (down & bit) {
                if (page == P_GLO) { scale_now.transpose = (int8_t)((int32_t)k - 7); key_on[k] = 0; }   /* C4 = none */
                else {
                    key_on[k] = (uint8_t)(KEY0 + (int32_t)k + 12 * scale_now.octave);
                    key_note(1, key_on[k], 100);
                }
            }
            if ((up & bit) && key_on[k]) { key_note(0, key_on[k], 0); key_on[k] = 0; }
        }

        midi_poll(0);
        pendulums_retune();
        arp_publish();
        update_fx();
        if (message && now - message_at > 1500u) message = 0;

        /* ---- what the pendulums struck: MIDI out, the key lights, the ARP's history */
        int32_t base = (int32_t)KEY0 + 12 * scale_now.octave;
        while (str_r != str_w) {
            strike_t s = strq[str_r % 32u];
            str_r++;
            uint32_t ch = midi_out_ch();
            for (uint32_t i = 0; i < 16u; i++)  /* a note still on goes off first */
                if (off_at[i] && off_note[i] == s.note) { midi_send(0x80u | ch, s.note, 0); off_at[i] = 0; }
            midi_send(0x90u | ch, s.note, s.vel);
            for (uint32_t i = 0; i < 16u; i++) if (!off_at[i]) { off_at[i] = now + 120u; off_note[i] = s.note; break; }
            if (s.pend < PENDULUMS) pend_flash[s.pend] = now;
            int32_t k = (int32_t)s.note - base;
            if (k >= 0 && k < 27) flash_until[k] = now + 90u;
            if (arp.on && held_n) {
                for (uint32_t i = 15; i > 0; i--) arp_hist[i] = arp_hist[i - 1];
                arp_hist[0] = (uint8_t)clampi(((int32_t)s.note - 45) * 255 / 36, 0, 254);
                if (arp_hist_n < 16) arp_hist_n++;
            }
        }
        for (uint32_t i = 0; i < 16u; i++)
            if (off_at[i] && (int32_t)(now - off_at[i]) >= 0) { midi_send(0x80u | midi_out_ch(), off_note[i], 0); off_at[i] = 0; }

        /* ---- LEDs */
        uint32_t lit = keys;
        if (page == P_GLO) {
            int32_t k = scale_now.transpose + 7;
            while (k > 26) k -= 12;
            while (k < 0) k += 12;
            lit = 1u << k;
        } else {
            for (uint32_t k = 0; k < 27u; k++) if ((int32_t)(flash_until[k] - now) > 0) lit |= 1u << k;
        }
        for (uint32_t k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
        uint8_t any_caught = 0;
        for (uint32_t k = 0; k < PENDULUMS; k++) any_caught |= pend_caught[k];
        static uint32_t beat_seen, beat_t;
        if (clk_ui_beat != beat_seen) { beat_seen = clk_ui_beat; beat_t = now; }
        fm1_led_key(BTN_OCT_DOWN, scale_now.octave < 0);
        fm1_led_key(BTN_OCT_UP, scale_now.octave > 0);
        fm1_led_key(BTN_PLAY, clk_run && now - beat_t > 90u);
        fm1_led_key(BTN_REC, rec_on && (now / 200u) % 2u == 0);
        fm1_led_key(BTN_ARP, arp.on);
        fm1_led_key(BTN_SEL, any_caught);
        fm1_led_key(BTN_HOME, page == P_HOME);
        fm1_led_key(BTN_SEQ, page == P_SEQ);
        fm1_led_key(BTN_EDIT, page == P_EDIT);
        fm1_led_key(BTN_ENV, page == P_ENV);
        fm1_led_key(BTN_LFO, page == P_LFO);
        fm1_led_key(BTN_FX, page == P_FX);
        fm1_led_key(BTN_GLO, page == P_GLO);
        fm1_led_key(BTN_SAVE, message && message == msg && now - message_at < 1500u && preset >= FACTORY);

        /* ---- the screen */
        if (drawn_page == 0xFF && now - boot_started < BOOT_LOGO_MS && !b && !keys) continue;
        idle_ok = 1;
        uint8_t whole = page != drawn_page;
        if (whole) {
            lcd_fill(0, 0, 240, 240, C_BG);
            ui_divider(40, C_RULE);
            ui_divider(211, C_RULE);
            drawn_page = page;
            drawn_footer = drawn_line = title_state = ~0u;
#if !BOARD_SMK37
            battery_drawn = UINT8_MAX - 1u;
#endif
        }
        uint32_t ts = (uint32_t)page | (uint32_t)arp.on << 4 | (uint32_t)pat.engine << 5 | (uint32_t)rec_on << 8 |
                      clk_run << 9 | (uint32_t)(pat.lfo_target == LFO_GRAVITY) << 10;
        if (ts != title_state) {
            char tb[24];
            lcd_fill(UI_LEFT, 10, UI_WIDTH - UI_BATTERY_WIDTH - 4u, 26, C_BG);
            ui_text(UI_LEFT, 13, UI_WIDTH - UI_BATTERY_WIDTH - 8u, &UI_LARGE, page_title(page, tb), title_colour(page), 0);
            title_state = ts;
        }
        uint8_t time_page = page == P_SEQ || page == P_HOME;
        uint32_t ls = (pd_c32 >> 24) ^ (uint32_t)pat.cycle << 8 ^ clk_run << 12 ^ pd_running << 13 ^ (uint32_t)time_page << 14;
        if (ls != drawn_line) { draw_cycle_line(time_page); drawn_line = ls; }
        uint32_t ps = page_state(page, now), rs = row_state(page);
        if (whole || ps != drawn_state || rs != drawn_rows) {
            draw_page(page, whole || rs != drawn_rows);
            drawn_state = ps;
            drawn_rows = rs;
        }
        uint32_t fs = (uint32_t)(uintptr_t)message ^ message_at * 31u ^ (uint32_t)picked << 3 ^ (uint32_t)rec_on << 8 ^
                      (uint32_t)pend_caught[picked] << 9 ^ (uint32_t)scale_root_pc() << 10 ^ (uint32_t)arp.on << 14 ^
                      clk_run << 15 ^ pd_running << 16 ^ (uint32_t)pat.lfo_target << 24 ^
                      (page == P_SEQ ? (uint32_t)(((uint64_t)pd_c32 * cycle_beats[pat.cycle]) >> 32) << 17 : 0u);
        if (fs != drawn_footer) { draw_footer(page); drawn_footer = fs; }
#if !BOARD_SMK37
        uint8_t battery_level = read_battery_level(now);
        if (battery_drawn != battery_level) { draw_battery(battery_level); battery_drawn = battery_level; }
#endif
        idle_ok = 0;
    }
}
