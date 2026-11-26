/* SPDX-License-Identifier: GPL-3.0-only */
/* PLUMB's sound and its pendulums. Everything here runs in the audio interrupt (or is set up for it).
 *
 *   THE PENDULUMS   12 of them, released together. Pendulum k swings (SWINGS + k) times in one CYCLE, so they leave
 *                   in a line, snake, split into rows, scatter, and all meet again at the end of the cycle. Each time a
 *                   bob passes its STRIKE point it plays its note. The rhythm is the physics: polyrhythms that always
 *                   come back together. The clock is Dinghy's (kit_clock.h): a cycle starts on the transport's
 *                   downbeat, so a DAW's START releases them in time.
 *   THE BOBS        what each strike plays, one of three oscillator engines (no resonators, no plucked strings):
 *                   FOLD    a West Coast voice: a sine through a wavefolder, into a low-pass gate (the vactrol "ping")
 *                   PHASE   phase distortion, as Casio's CZ: a cosine whose phase is bent into saw, square, pulse
 *                   PULSAR  pulsar synthesis (Curtis Roads): one short grain of a sine per cycle; its own pitch is a
 *                           formant, its length the duty
 *                   SPARK is how much the strike itself brightens the bob (the envelope opens the timbre).
 *   ENV, LFO        an ADSR per strike; one LFO for PITCH, TIMBRE, LEVEL or GRAVITY (which warps the pendulums' time).
 *
 * Every number is an integer (no FPU). */
#include "plumb_tables.h"

#define NVOICE 8u
#define PENDULUMS 12u
#define Q15 32768
#define SUB 16u                                 /* the control rate: 16 frames (as kit_clock.h's CLK_CTL) */

enum { ENG_FOLD, ENG_PHASE, ENG_PULSAR, ENGINES };
enum { SHAPE_SAW, SHAPE_SQUARE, SHAPE_PULSE, SHAPE_DOUBLE, SHAPES };
enum { STRIKE_MIDDLE, STRIKE_ONE_WAY, STRIKE_LEFT, STRIKE_WALLS, STRIKES };
enum { LFO_PITCH, LFO_TIMBRE, LFO_LEVEL, LFO_GRAVITY, LFO_TARGETS };
enum { LW_SINE, LW_TRIANGLE, LW_RANDOM, LW_DRIFT, LFO_WAVES };
enum { ROW_RISE, ROW_FALL, ROW_ARCH, ROW_VALLEY, ROW_ZIGZAG, ROW_CHORD, ROW_FIFTHS, ROW_SCATTER, ROWS };
enum { PAT_UP, PAT_DOWN, PAT_BOUNCE, PAT_RANDOM, PAT_SPREAD, PATTERNS };

static const char *const engine_names[ENGINES] = {"FOLD", "PHASE", "PULSAR"};
static const char *const shape_names[SHAPES] = {"SAW", "SQUARE", "PULSE", "DOUBLE"};
static const char *const strike_names[STRIKES] = {"MIDDLE", "ONE WAY", "LEFT WALL", "BOTH WALLS"};
static const char *const lfo_target_names[LFO_TARGETS] = {"PITCH", "TIMBRE", "LEVEL", "GRAVITY"};
static const char *const lfo_wave_names[LFO_WAVES] = {"SINE", "TRIANGLE", "RANDOM", "DRIFT"};
static const char *const row_names[ROWS] = {"RISE", "FALL", "ARCH", "VALLEY", "ZIGZAG", "CHORD", "FIFTHS", "SCATTER"};
static const char *const pattern_names[PATTERNS] = {"UP", "DOWN", "BOUNCE", "RANDOM", "SPREAD"};
static const uint8_t cycle_beats[] = {4, 6, 8, 12, 16, 24, 32, 48, 64};
#define CYCLES (sizeof cycle_beats)
static const uint8_t grid_ticks[] = {0, 3, 4, 6, 8, 12, 24};     /* 24 a quarter */
static const char *const grid_names[] = {"FREE", "1/32", "1/16 T", "1/16", "1/8 T", "1/8", "1/4"};
#define GRIDS (sizeof grid_ticks)
static const uint8_t echo_clocks[] = {3, 6, 9, 12, 18, 24, 36};  /* 24 a quarter */
static const char *const echo_names[] = {"1/32", "1/16", "1/16 D", "1/8", "1/8 D", "1/4", "1/4 D"};
#define ECHOS (sizeof echo_clocks)

/* a patch: everything PRESETS loads and SAVE keeps */
typedef struct {
    uint8_t engine, k1, k2, spark, width;       /* EDIT (k1, k2: the engine's own two) */
    uint8_t attack, decay, sustain, release;    /* ENV: indexes into env_ms[]; sustain % */
    uint8_t lfo_target, lfo_wave, lfo_rate, lfo_depth;
    uint8_t reverb, echo, echo_time, chorus;    /* FX */
    uint8_t cycle, swings, strike, grid;        /* SEQ: the pendulums' rhythm */
    uint8_t count, damp, row, scale;            /* HOME: how many swing, how fast they tire, the row; the scale */
    int8_t notes[PENDULUMS];                    /* the row: a scale step each */
} patch_t;

static patch_t pat;

/* ------------------------------------------------------------------------------------------------ helpers */
static inline int32_t sinq(uint32_t ph)         /* 2^32 a cycle -> -32767..32767 */
{
    uint32_t i = ph >> 22, f = (ph >> 6) & 0xFFFFu;
    int32_t a = sine1k[i], b = sine1k[i + 1u];
    return a + (((b - a) * (int32_t)f) >> 16);
}

static uint32_t inc_of(int32_t p)               /* 1/256 semitones -> phase step a frame */
{
    if (p < 12 * 256) p = 12 * 256;
    if (p > 119 * 256) p = 119 * 256;
    uint32_t n = (uint32_t)p >> 8, f = (uint32_t)p & 255u;
    uint32_t a = inc_c9[n % 12u] >> (10u - n / 12u);
    uint32_t m = n + 1u, b = inc_c9[m % 12u] >> (10u - m / 12u);
    return a + (uint32_t)(((uint64_t)(b - a) * f) >> 8);
}

static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int32_t soft(int32_t x)           /* a soft ceiling near 31000 */
{
    int32_t m = x < 0 ? -x : x;
    if (m <= 22000) return x;
    m = 22000 + 9000 * (m - 22000) / (9000 + (m - 22000));
    return x < 0 ? -m : m;
}

static uint32_t env_step(uint8_t index)         /* the change a frame (Q24) for a full sweep in env_ms[index] */
{
    if (index >= ENV_STEPS) index = ENV_STEPS - 1u;
    uint32_t frames = (uint32_t)env_ms[index] * (FS / 100u) / 10u;
    return frames ? (1u << 24) / frames + 1u : 1u << 24;
}

/* ------------------------------------------------------------------------------------------------- voices */
enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
typedef struct {
    int32_t pitch;                              /* 1/256 semitones */
    int32_t env;                                /* 0 .. 1 << 24 */
    int32_t vel;                                /* Q15 */
    int32_t gl, gr;                             /* the pan, Q15 */
    int32_t lp;                                 /* FOLD's low-pass gate */
    uint32_t ph, gate, wait, age;               /* gate: frames left (0xFFFFFFFF: a held key) */
    uint8_t stage, tag, pend;                   /* tag: the key holding it (0: none); pend: the pendulum + 1 */
} voice_t;
static voice_t vo[NVOICE];
static uint32_t vo_age;
static volatile uint16_t pend_level[PENDULUMS];  /* for the screen: how loud each pendulum's last strike still is */

static void voice_start(uint32_t v, int32_t pitch, int32_t vel, int32_t pan, uint32_t gate, uint32_t wait,
                        uint8_t tag, uint8_t pend)
{
    voice_t *x = &vo[v];
    x->pitch = pitch;
    x->vel = vel;
    /* pan -32767 (left) .. 32767 (right): an equal-power law from the sine table */
    uint32_t a = (uint32_t)(pan + 32768) << 14;  /* 0 .. 2^30: a quarter cycle */
    x->gl = sinq(a + 0x40000000u);
    x->gr = sinq(a);
    x->gate = gate;
    x->wait = wait;
    x->tag = tag;
    x->pend = pend;
    x->age = ++vo_age;
    x->stage = ST_ATTACK;
    x->env = 0;
    x->ph = 0;
    x->lp = 0;
}

static uint32_t voice_cap = NVOICE;             /* fewer when the sound runs short of time (app_audio) */
static uint32_t voice_pick(void)                /* a silent voice, else the quietest of the oldest */
{
    uint32_t best = 0, score_best = 0xFFFFFFFFu;
    for (uint32_t v = 0; v < voice_cap; v++) {
        uint32_t score;
        if (vo[v].stage == ST_IDLE) return v;
        score = (vo[v].stage == ST_RELEASE ? 0u : 0x40000000u) + (uint32_t)(vo[v].env >> 10) + (vo_age - vo[v].age > 1000u ? 0u : 0x10000000u);
        if (vo[v].tag) score += 0x80000000u;    /* a held key is the last to go */
        if (score < score_best) { score_best = score; best = v; }
    }
    return best;
}

/* the commands of the main loop (the keys): a small queue, one writer each side */
typedef struct { uint8_t on, tag; int16_t vel; int32_t pitch; } cmd_t;
static cmd_t cmdq[16];
static volatile uint32_t cmd_w, cmd_r;
static void cmd_put(uint8_t on, uint8_t tag, int32_t pitch, int32_t vel)
{
    if (cmd_w - cmd_r >= 16u) return;
    cmd_t *c = &cmdq[cmd_w % 16u];
    c->on = on; c->tag = tag; c->pitch = pitch; c->vel = (int16_t)vel;
    cmd_w++;
}

/* --------------------------------------------------------------------------------------------------- LFO */
static uint32_t lfo_ph, lfo_rnd[2] = {0x68e31da4u, 0x1b873593u};
static volatile int32_t lfo_now;                /* -32767..32767, for the screen and GRAVITY */
static int32_t lfo_value(void)
{
    int32_t x = (int32_t)(lfo_ph >> 16) - 32768, m = x < 0 ? -x : x;
    switch (pat.lfo_wave) {
    case LW_SINE: return clampi((int32_t)((int64_t)x * (32768 - m) >> 13), -32767, 32767);
    case LW_TRIANGLE: return clampi(32767 - 2 * m, -32767, 32767);
    case LW_RANDOM: return (int32_t)(lfo_rnd[1] >> 16) - 32768;
    default: {
        int32_t a = (int32_t)(lfo_rnd[0] >> 16) - 32768, b = (int32_t)(lfo_rnd[1] >> 16) - 32768;
        uint32_t t = lfo_ph >> 17, s = (t * t >> 15) * (3u * 32768u - 2u * t) >> 15;
        return a + (int32_t)(((int64_t)(b - a) * (int32_t)s) >> 15);
    }
    }
}

/* ----------------------------------------------------------------------------------------- the pendulums */
static volatile int32_t pend_pitch[PENDULUMS]; /* each one's note now (the row through the scale), from the main loop */
static volatile uint8_t pend_caught[PENDULUMS];/* SEL: caught, hangs still */
static volatile int32_t arp_list[32];          /* the ARP: the held notes as pitches (main loop), arp_n of them */
static volatile uint32_t arp_n, arp_on, arp_pattern, arp_chance = 100;
static struct {
    uint32_t c32;                               /* where in the cycle: 2^32 a cycle */
    uint32_t tick_c, frac;                      /* the clock's tick in the cycle, and the way to the next (Q16) */
    uint32_t amp;                               /* the swing, Q16 (DAMP takes it down) */
    uint32_t prev[PENDULUMS];
    uint8_t pending[PENDULUMS], side[PENDULUMS];
    uint8_t running, seen_tick, damp_seg;
    int32_t grav;                               /* GRAVITY's warp, in 2^32 a cycle */
} pd;
static volatile uint32_t pd_c32, pd_amp, pd_running;   /* for the screen */
static uint32_t arp_idx, arp_rnd = 0x2545f491u;
static int32_t arp_dir = 1;

/* what a strike did, for the main loop (MIDI out, the key lights, the screen) */
typedef struct { uint8_t pend, note, vel; } strike_t;
static strike_t strq[32];
static volatile uint32_t str_w, str_r;

/* the clock's ticks in this block (kit_clock.h calls app_tick from clock_audio) */
static uint32_t tk_n, tk_tick[8], tk_ofs[8];
static void app_tick(uint32_t tick, uint32_t ofs)
{
    if (tk_n < 8u) { tk_tick[tk_n] = tick; tk_ofs[tk_n] = ofs; tk_n++; }
}

static uint32_t rnd_next(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 16; }

static int32_t arp_pick(uint32_t k)             /* the ARP's next note (or -1: rest) */
{
    uint32_t n = arp_n;
    if (!n) return -1;
    if (arp_chance < 100u && rnd_next(&arp_rnd) % 100u >= arp_chance) return -1;
    switch (arp_pattern) {
    case PAT_UP: arp_idx = (arp_idx + 1u) % n; break;
    case PAT_DOWN: arp_idx = arp_idx == 0u || arp_idx >= n ? n - 1u : arp_idx - 1u; break;
    case PAT_BOUNCE: {
        int32_t i = (int32_t)arp_idx + arp_dir;
        if (n < 2u) { arp_idx = 0; break; }
        if (i < 0 || i >= (int32_t)n) { arp_dir = -arp_dir; i = (int32_t)arp_idx + arp_dir; }
        arp_idx = (uint32_t)clampi(i, 0, (int32_t)n - 1);
        break;
    }
    case PAT_RANDOM: arp_idx = rnd_next(&arp_rnd) % n; break;
    default: arp_idx = k % n; break;           /* SPREAD: pendulum k plays note k */
    }
    if (arp_idx >= n) arp_idx = 0;
    return arp_list[arp_idx];
}

static uint32_t frames_per_tick(void)
{
    uint32_t q = clk_q ? clk_q : clock_quarter();
    return q / CLK_PPQN;
}

static void pendulum_strike(uint32_t k, uint32_t wait)
{
    uint32_t n = pat.swings + k, s = pat.strike == STRIKE_MIDDLE || pat.strike == STRIKE_WALLS ? 2u : 1u;
    int32_t pitch = pend_pitch[k];
    if (arp_on && arp_n) {
        int32_t a = arp_pick(k);
        if (a < 0) return;
        pitch = a;
    }
    /* the pan: across the row (low pendulum left); BOTH WALLS: the wall it touched */
    int32_t pan = pat.strike == STRIKE_WALLS ? (pd.side[k] ? 32767 : -32767)
                : ((int32_t)k * 2 - (int32_t)(PENDULUMS - 1u)) * 32767 / (int32_t)(PENDULUMS - 1u);
    pan = pan * pat.width / 100;
    /* the gate: half the time to this bob's next strike */
    uint32_t T = (uint32_t)cycle_beats[pat.cycle % CYCLES] * CLK_PPQN * frames_per_tick();
    uint32_t gate = T / (n * s) / 2u;
    int32_t vel = (int32_t)(pd.amp >> 1);       /* Q16 -> Q15 */
    if (vel < 200) return;
    voice_start(voice_pick(), pitch, vel, pan, gate ? gate : 1u, wait, 0, (uint8_t)(k + 1u));
    pend_level[k] = (uint16_t)vel;
    if (str_w - str_r < 32u) {
        strike_t *e = &strq[str_w % 32u];
        e->pend = (uint8_t)k;
        e->note = (uint8_t)clampi((pitch + 128) >> 8, 0, 127);
        e->vel = (uint8_t)clampi(vel >> 8, 1, 127);
        str_w++;
    }
}

/* where pendulum k is: its phase, 2^32 a swing there and back (0 = at the left wall, where they are released) */
static inline uint32_t pend_phase(uint32_t c32, int32_t grav, uint32_t k)
{
    uint32_t n = pat.swings + k;
    return n * (c32 + (uint32_t)grav);
}

static void pendulums_block(uint32_t frames)
{
    uint32_t T24 = (uint32_t)cycle_beats[pat.cycle % CYCLES] * CLK_PPQN;
    uint32_t unit = 0xFFFFFFFFu / T24;
    uint32_t fpt = frames_per_tick();
    uint32_t df = fpt ? (SUB << 16) / fpt : 0;
    uint32_t grid = grid_ticks[pat.grid % GRIDS];
    uint32_t count = pat.count > PENDULUMS ? PENDULUMS : pat.count;
    static const uint32_t ths[STRIKES][2] = {{0x40000000u, 0xC0000000u}, {0x40000000u, 0}, {0, 0}, {0, 0x80000000u}};
    uint32_t nth = pat.strike == STRIKE_MIDDLE || pat.strike == STRIKE_WALLS ? 2u : 1u;
    uint32_t t = 0;
    if (!clk_run) {                             /* stopped: caught where they are */
        pd.running = 0;
        pd_running = 0;
        tk_n = 0;
        return;
    }
    for (uint32_t f = 0; f < frames; f += SUB) {
        uint8_t grid_now = 0, ticked = 0;
        /* the clock's ticks in these 16 frames put the cycle exactly where the beat is */
        while (t < tk_n && tk_ofs[t] < f + SUB) {
            uint32_t tick = tk_tick[t++];
            ticked = 1;
            if (tick == 0u || !pd.running) {    /* the downbeat (PLAY, a DAW's START): released from the line */
                pd.running = 1;
                pd.amp = 65535u;
                pd.damp_seg = 0;
                for (uint32_t k = 0; k < PENDULUMS; k++) { pd.prev[k] = 0xFFFFFFFFu; pd.pending[k] = 0; }
                arp_idx = 0xFFFFFFFFu;
            }
            pd.tick_c = tick % T24;
            pd.frac = 0;
            if (grid && tick % grid == 0u) grid_now = 1;
            /* DAMP: 64 steps a cycle */
            uint8_t seg = (uint8_t)(pd.tick_c * 64u / T24);
            if (seg != pd.damp_seg) {
                pd.damp_seg = seg;
                pd.amp = (uint32_t)(((uint64_t)pd.amp * damp_q16[pat.damp % DAMPS]) >> 16);
            }
        }
        if (!pd.running) continue;
        if (!ticked) {   /* no tick here: on toward the next one (never past it) */
            pd.frac += df;
            if (pd.frac > 65535u) pd.frac = 65535u;
        }
        pd.c32 = pd.tick_c * unit + (uint32_t)(((uint64_t)pd.frac * unit) >> 16);
        if (pd.amp < 655u) continue;            /* (1 %: they have stopped) */
        for (uint32_t k = 0; k < count; k++) {
            uint32_t ph = pend_phase(pd.c32, pd.grav, k), was = pd.prev[k];
            pd.prev[k] = ph;
            for (uint32_t i = 0; i < nth; i++) {
                uint32_t th = ths[pat.strike % STRIKES][i];
                if (was - th > ph - th && was - ph > 0x80000000u) {   /* passed it, going forward */
                    pd.pending[k] = 1;
                    pd.side[k] = (uint8_t)i;
                }
            }
            if (pd.pending[k] && (!grid || grid_now)) {
                pd.pending[k] = 0;
                if (!pend_caught[k]) pendulum_strike(k, f);
            }
        }
    }
    tk_n = 0;
    pd_c32 = pd.c32;
    pd_amp = pd.amp;
    pd_running = pd.running && pd.amp >= 655u;
}

/* ------------------------------------------------------------------------------------------------ one voice */
static int32_t vbuf_l[HALF_FRAMES], vbuf_r[HALF_FRAMES];

static void voice_render(voice_t *x, uint32_t frames, int32_t pitch_mod, int32_t timbre_mod, int32_t level)
{
    uint32_t up = env_step(pat.attack), down = env_step(pat.decay), rel = env_step(pat.release);
    int32_t sustain = (int32_t)((uint32_t)pat.sustain * (1u << 24) / 100u);
    uint32_t inc = inc_of(x->pitch + pitch_mod);
    uint8_t eng = pat.engine;
    int32_t peak = 0, vl = ((x->vel * level) >> 15) >> 1;   /* the strike x the LFO's tremolo, Q14 */
    for (uint32_t f0 = 0; f0 < frames; f0 += SUB) {
        uint32_t n = frames - f0 < SUB ? frames - f0 : SUB;
        uint32_t start = 0;
        if (x->wait) {                          /* a strike later in this block */
            if (x->wait >= n) { x->wait -= n; continue; }
            start = x->wait;
            x->wait = 0;
        }
        /* the timbre, once every 16 frames: the knob, plus SPARK x the envelope, plus the LFO */
        int32_t e15 = x->env >> 9;              /* Q15 */
        int32_t tim = (int32_t)pat.k1 * 327 + mul15(e15, (int32_t)pat.spark * 327) + timbre_mod;
        tim = clampi(tim, 0, 32767);            /* 0 .. 1 (Q15) */
        /* the engine's coefficients */
        int32_t gain_q8 = 256, sym = 0, kq = Q15;
        uint32_t knee = 32768, slope1 = 65536, slope2 = 65536, rd = 0, fq8 = 256;
        if (eng == ENG_FOLD) {
            gain_q8 = 256 + tim * 9 / 128;      /* up to ~9.9x: many folds */
            sym = ((int32_t)pat.k2 - 50) * 400;
            kq = 1200 + mul15(e15, 31000 - 1200);    /* the gate: the envelope opens the low-pass too */
        } else if (eng == ENG_PHASE) {
            int32_t d = mul15(tim, 31500);      /* the distortion 0 .. 0.96 */
            uint32_t shape = pat.k2 % SHAPES;
            if (shape == SHAPE_SQUARE) knee = (uint32_t)(32768 - d);              /* of a half cycle */
            else if (shape == SHAPE_PULSE) knee = (uint32_t)(65536 - 2 * d);       /* where it stops */
            else knee = (uint32_t)(32768 - d);                                     /* SAW, DOUBLE */
            if (knee < 600u) knee = 600u;
            if (shape == SHAPE_SQUARE) slope1 = (32768u << 8) / knee;              /* Q8: 32-bit products */
            else if (shape == SHAPE_PULSE) slope1 = (65535u << 8) / knee;
            else { slope1 = (32768u << 16) / knee; slope2 = (32768u << 16) / (65536u - knee); }
        } else {
            uint32_t du = 65536u - (uint32_t)pat.k2 * 620u;   /* DUTY 0..100: the grain from all of the cycle to 6 % */
            if (du < 2000u) du = 2000u;
            rd = (65536u << 8) / du;            /* Q8 */
            fq8 = 256u + (uint32_t)tim * 11u / 128u;   /* the formant: 1x .. 12x the note */
        }
        for (uint32_t i = start; i < n; i++) {
            /* the envelope */
            if (x->gate != 0xFFFFFFFFu && x->gate) { if (--x->gate == 0u && x->stage != ST_IDLE) x->stage = ST_RELEASE; }
            switch (x->stage) {
            case ST_ATTACK: x->env += (int32_t)up; if (x->env >= 1 << 24) { x->env = 1 << 24; x->stage = ST_DECAY; } break;
            case ST_DECAY: x->env -= (int32_t)down; if (x->env <= sustain) { x->env = sustain; x->stage = ST_SUSTAIN; } break;
            case ST_RELEASE: x->env -= (int32_t)rel; if (x->env <= 0) { x->env = 0; x->stage = ST_IDLE; } break;
            default: break;
            }
            uint32_t ph = x->ph;
            x->ph = ph + inc;
            int32_t s;
            if (eng == ENG_FOLD) {
                int32_t v = ((sinq(ph) * gain_q8) >> 8) + sym;
                uint32_t u = (uint32_t)(v + 32768) & 0x1FFFFu;          /* a triangle fold, 131072 a period */
                if (u > 65536u) u = 131072u - u;
                s = (int32_t)u - 32768;
                x->lp += ((s - x->lp) * (kq >> 1)) >> 14;
                s = x->lp;
            } else if (eng == ENG_PHASE) {
                uint32_t p16 = ph >> 16, w;
                uint32_t shape = pat.k2 % SHAPES;
                if (shape == SHAPE_DOUBLE) p16 = (p16 << 1) & 0xFFFFu;
                if (shape == SHAPE_SQUARE) {
                    /* each half: held at its peak, then across to the other one (CZ's square) */
                    uint32_t h = p16 & 0x7FFFu, hold = 32768u - knee;
                    uint32_t q = h < hold ? 0u : ((h - hold) * slope1) >> 8;
                    w = (p16 & 0x8000u) + (q > 32767u ? 32767u : q);
                } else if (shape == SHAPE_PULSE) {
                    w = p16 < knee ? (p16 * slope1) >> 8 : 65535u;
                } else {
                    w = p16 < knee ? (p16 * slope1) >> 16 : 32768u + (((p16 - knee) * slope2) >> 16);
                }
                s = sinq((w << 16) + 0x40000000u);      /* a cosine, bent */
                if (shape == SHAPE_DOUBLE && (ph >> 31)) s = s / 2;   /* (the CZ's double sine: the second half softer) */
            } else {
                uint32_t p16 = ph >> 16;
                uint32_t local = (p16 * rd) >> 8;
                if (local >= 65536u) s = 0;
                else {
                    int32_t win = sinq(local << 15);            /* half a sine over the grain */
                    s = (sinq((local * fq8) << 8) * win) >> 15;
                }
            }
            /* the level: the envelope, the strike, the LFO's tremolo */
            int32_t a = ((x->env >> 9) * vl) >> 14;   /* (32-bit products only: the chip is 32-bit) */
            s = (s * a) >> 15;
            int32_t m = s < 0 ? -s : s;
            if (m > peak) peak = m;
            vbuf_l[f0 + i] += (s * x->gl) >> 15;
            vbuf_r[f0 + i] += (s * x->gr) >> 15;
        }
        if (x->stage == ST_IDLE) break;
    }
    if (x->pend && x->pend <= PENDULUMS) {
        uint16_t lv = (uint16_t)(peak > 32767 ? 32767 : peak);
        if (lv > pend_level[x->pend - 1u] || x->stage == ST_IDLE) pend_level[x->pend - 1u] = lv;
    }
}

/* ---------------------------------------------------------------------------------------- the whole sound */
static void plumb_render(int32_t *out, uint32_t frames)
{
    /* the keys */
    while (cmd_r != cmd_w) {
        cmd_t c = cmdq[cmd_r % 16u];
        cmd_r++;
        if (c.on) voice_start(voice_pick(), c.pitch, c.vel, 0, 0xFFFFFFFFu, 0, c.tag, 0);
        else for (uint32_t v = 0; v < NVOICE; v++)
            if (vo[v].tag == c.tag && vo[v].stage != ST_IDLE) { vo[v].tag = 0; vo[v].gate = 0; vo[v].stage = ST_RELEASE; }
    }
    /* the LFO, once a block */
    uint32_t before = lfo_ph;
    lfo_ph += lfo_inc16[pat.lfo_rate % LFO_RATES] * (frames / SUB);
    if (lfo_ph < before) { lfo_rnd[0] = lfo_rnd[1]; lfo_rnd[1] = lfo_rnd[1] * 1664525u + 1013904223u; }
    int32_t l = lfo_value(), mod = l * pat.lfo_depth / 100;
    lfo_now = l;
    int32_t pitch_mod = pat.lfo_target == LFO_PITCH ? mod >> 7 : 0;   /* +- 1 semitone */
    int32_t timbre_mod = pat.lfo_target == LFO_TIMBRE ? mod / 2 : 0;
    int32_t level = pat.lfo_target == LFO_LEVEL ? Q15 - (32767 - l) * pat.lfo_depth / 200 : Q15;
    /* GRAVITY: the pendulums' time, pulled ahead and held back (up to 1/16 of a cycle) */
    pd.grav = pat.lfo_target == LFO_GRAVITY ? mod * 8192 : 0;
    pendulums_block(frames);

    for (uint32_t i = 0; i < frames; i++) vbuf_l[i] = vbuf_r[i] = 0;
    for (uint32_t v = 0; v < NVOICE; v++)
        if (vo[v].stage != ST_IDLE) voice_render(&vo[v], frames, pitch_mod, timbre_mod, level);
    for (uint32_t i = 0; i < frames; i++) {
        out[2u * i] += vbuf_l[i] * 3 / 8;       /* (headroom: eight voices at once) */
        out[2u * i + 1u] += vbuf_r[i] * 3 / 8;
    }
    for (uint32_t k = 0; k < PENDULUMS; k++) {  /* the screen's rings fade */
        uint32_t lv = pend_level[k];
        int used = 0;
        for (uint32_t v = 0; v < NVOICE; v++) used |= vo[v].pend == k + 1u && vo[v].stage != ST_IDLE;
        if (!used) pend_level[k] = (uint16_t)(lv - (lv >> 3));
    }
}

static int plumb_quiet(void)
{
    for (uint32_t v = 0; v < NVOICE; v++) if (vo[v].stage != ST_IDLE) return 0;
    return 1;
}
