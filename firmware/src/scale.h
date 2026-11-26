/* SPDX-License-Identifier: GPL-3.0-only */
/* BUOY's scale stage: the very last thing before the bells.
 * The ARP, the SEQ and the keys only deal in "steps" (0 = the root, 1 = the next note of the scale, ...). Here a
 * step becomes a pitch. A change of scale or key (GLO) moves everything at once: recordings, held drones too.
 * Some scales are not on the piano's grid: their notes carry cents (the bells can tune anywhere; MIDI out sends
 * the nearest key). */
static const struct { const char *name; uint16_t steps; int8_t cents[12]; } scales[] = {
    {"CHROMATIC", 0xfffu, {0}},
    {"MAJOR", 0xab5u, {0}},
    {"MINOR", 0x5adu, {0}},
    {"MAJOR PENT", 0x295u, {0}},
    {"MINOR PENT", 0x4a9u, {0}},
    {"BLUES", 0x4e9u, {0}},
    {"HARMONIC MINOR", 0x9adu, {0}},
    {"MELODIC MINOR", 0xaadu, {0}},
    {"DORIAN", 0x6adu, {0}},
    {"PHRYGIAN", 0x5abu, {0}},
    {"LYDIAN", 0xad5u, {0}},
    {"MIXOLYDIAN", 0x6b5u, {0}},
    {"LOCRIAN", 0x56bu, {0}},
    {"LYDIAN DOMINANT", 0x6d5u, {0}},
    {"ALTERED", 0x55bu, {0}},
    {"WHOLE TONE", 0x555u, {0}},
    {"DIM HALF-WHOLE", 0x6dbu, {0}},
    {"DIM WHOLE-HALF", 0xb6du, {0}},
    {"AUGMENTED", 0x999u, {0}},
    {"HIRAJOSHI", 0x18du, {0}},
    {"IN SEN", 0x4a3u, {0}},
    {"IWATO", 0x463u, {0}},
    {"KUMOI", 0x28du, {0}},
    {"YO", 0x2a5u, {0}},
    {"PELOG", 0x18bu, {0}},
    {"SLENDRO", 0x4a5u, {0, 0, 40, 0, 0, -20, 0, 20, 0, 0, -40, 0}},   /* five nearly equal steps (240 cents) */
    {"EGYPTIAN", 0x4a5u, {0}},
    {"CHINESE GONG", 0x295u, {0}},
    {"HIJAZ", 0x5b3u, {0}},
    {"DOUBLE HARMONIC", 0x9b3u, {0}},
    {"RAGA BHAIRAV", 0x9b3u, {0}},
    {"RAGA BHAIRAVI", 0x5abu, {0}},
    {"RAGA TODI", 0x9cbu, {0}},
    {"RAGA MARWA", 0xad3u, {0}},
    {"HUNGARIAN MINOR", 0x9cdu, {0}},
    {"HUNGARIAN MAJOR", 0x6d9u, {0}},
    {"UKRAINIAN DORIAN", 0x6cdu, {0}},
    {"PERSIAN", 0x973u, {0}},
    {"ORIENTAL", 0x673u, {0}},
    {"NEAPOLITAN MINOR", 0x9abu, {0}},
    {"NEAPOLITAN MAJOR", 0xaabu, {0}},
    {"ENIGMATIC", 0xd53u, {0}},
    {"PROMETHEUS", 0x655u, {0}},
    {"SPANISH 8-TONE", 0x57bu, {0}},
    {"JUST MAJOR", 0xab5u, {0, 0, 4, 0, -14, -2, 0, 2, 0, -16, 0, -12}},   /* pure thirds and fifths */
    {"HARMONIC SERIES", 0xdd5u, {0, 0, 4, 0, -14, 0, -49, 2, 41, 0, -31, -12}},   /* overtones 8 .. 15 */
    {"FIFTHS", 0x081u, {0}},
    {"OCTAVES", 0x001u, {0}},
};
#define SCALE_COUNT (sizeof scales / sizeof scales[0])
#define SCALE_BASE 48                           /* step 0 with no transposition: C3 */

static struct {
    uint8_t scale;                              /* index into scales[] */
    int8_t transpose;                           /* semitones, -31 .. +43: the GLO page's keys (with OCT) */
    int8_t octave;                              /* -2 .. +2: OCT- / OCT+ (the ARP, the keys; not the tracks) */
    uint8_t size;                               /* notes in the scale (1 .. 12) */
    uint8_t offset[12];                         /* semitones of each step above the root */
} scale_now = {1, 0, 0, 7, {0, 2, 4, 5, 7, 9, 11}};

static void scale_select(uint8_t scale)
{
    if (scale >= SCALE_COUNT) scale = 0;
    scale_now.scale = scale;
    uint8_t n = 0;
    for (uint8_t i = 0; i < 12; i++)
        if (scales[scale].steps >> i & 1u) scale_now.offset[n++] = i;
    scale_now.size = n;
}

static uint8_t scale_root_pc(void)              /* the key the scale stands on, 0 = C .. 11 = B */
{
    return (uint8_t)((SCALE_BASE + scale_now.transpose + 120) % 12);
}

static int32_t floor_div(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* a step (any integer, negative too) -> a pitch in 1/256 semitones (with the scale's cents), within MIDI 12..120 */
static int32_t scale_step_pitch(int32_t step)
{
    int32_t n = scale_now.size, octave = floor_div(step, n), i = step - octave * n;
    uint8_t off = scale_now.offset[i];
    int32_t note = SCALE_BASE + scale_now.transpose + 12 * octave + off;
    while (note < 12) note += 12;
    while (note > 120) note -= 12;
    return note * 256 + scales[scale_now.scale].cents[off] * 256 / 100;
}

static uint8_t scale_step_note(int32_t step)    /* the nearest MIDI note (MIDI out) */
{
    return (uint8_t)((scale_step_pitch(step) + 128) >> 8);
}

static int scale_contains(uint8_t pitch)
{
    return scales[scale_now.scale].steps >> ((pitch + 12u - scale_root_pc()) % 12u) & 1u;
}

/* a played note (the keys, MIDI in) -> its step: the nearest note of the scale, the lower one at equal distance */
static int32_t scale_note_step(int32_t note)
{
    int32_t rel = note - (SCALE_BASE + scale_now.transpose), best = 0, best_d = 99;
    int32_t octave = floor_div(rel, 12), pc = rel - 12 * octave;
    for (int32_t o = -1; o <= 1; o++)
        for (int32_t i = 0; i < scale_now.size; i++) {
            int32_t d = (o * 12 + scale_now.offset[i]) - pc, m = d < 0 ? -d + 1 : d;   /* (lower wins a tie) */
            if (m < best_d) { best_d = m; best = (octave + o) * scale_now.size + i; }
        }
    return best;
}

/* SPREAD's partner: how many steps up (or down) is the scale's octave, its fifth, its third */
static int32_t scale_spread_steps(uint8_t spread)
{
    int32_t n = scale_now.size;
    if (spread == 1) return n;                  /* OCTAVE UP */
    if (spread == 4) return -n;                 /* OCTAVE DOWN */
    int32_t want = spread == 2 ? 14 : 7, best = 1, best_d = 999;   /* (twice the semitones: 7, 3.5) */
    for (int32_t i = 1; i < n; i++) {
        int32_t d = 2 * scale_now.offset[i] - want;
        d = d < 0 ? -d : d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return n > 1 ? best : n;
}
