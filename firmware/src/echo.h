/* SPDX-License-Identifier: GPL-3.0-only */
/* PLUMB's SWING ECHO: a ping-pong echo that comes to rest like a pendulum.
 *
 * The sound goes into the left line only. Each line's repeat feeds the other line, so the first echo is on the left,
 * the next on the right, then left again: it swings. On every pass a little of each side bleeds into the other, so
 * the swing gets narrower, and a gentle low-pass makes each repeat darker, until the last ones sit quietly in the
 * middle. Two 12-bit lines at 20 kHz (as LOOPHEADS' delay), 1.5 s each: the same memory as the old mono echo. */
#include "fx12.h"
#define ECHO_LEN 30000u                         /* 1.5 s a side at 20 kHz */
#define ECHO_ENABLED (1u << 31)
#define ECHO_TIME_MASK 0x7fffu
#define ECHO_FADE 256u                          /* a new time: the old one fades out over 256 steps */
#define ECHO_BLEED 4096                         /* of 32768: how much of each side goes to the other per pass */
#define ECHO_DARK 20000                         /* of 32768: the low-pass in the loop (lower = darker repeats) */

#ifdef DINGHY_HOST
static int16_t echo_line[2][ECHO_LEN];
#else
static int16_t echo_line[2][ECHO_LEN] __attribute__((section(".pool")));
#endif

static volatile uint32_t echo_command;
static struct {
    uint32_t head, valid, time, old_time, fade;
    fx12_clock_t clock;
    int32_t lp[2], prev[2], now[2], mix, target_mix, feedback;
} echo;

static uint32_t echo_tempo_samples(uint32_t bpm, uint8_t clocks)   /* 24 clocks a quarter -> steps at 20 kHz */
{
    uint32_t per_minute = bpm * 24u, t = (FX12_RATE * 60u * clocks + per_minute / 2u) / per_minute;
    while (t >= ECHO_LEN) t /= 2u;             /* (longer than the line: half of it, still on the beat) */
    return t ? t : 1u;
}

static void echo_configure(uint8_t on, uint32_t time, uint8_t feedback, uint8_t mix)
{
    echo_command = (on ? ECHO_ENABLED : 0) | (time & ECHO_TIME_MASK) | (uint32_t)feedback << 16 | (uint32_t)mix << 23;
}

static int32_t echo_tap(uint32_t side, uint32_t time)
{
    if (time > echo.valid) return 0;
    uint32_t i = echo.head >= time ? echo.head - time : echo.head + ECHO_LEN - time;
    return echo_line[side][i];
}

static uint8_t echo_begin_block(void)
{
    uint32_t c = echo_command;
    uint8_t on = (c & ECHO_ENABLED) != 0;
    if (!on && !echo.mix) {
        if (echo.time) {                        /* forget the history without clearing the lines in the interrupt */
            echo.time = echo.valid = echo.fade = 0;
            echo.clock = (fx12_clock_t){0};
            echo.lp[0] = echo.lp[1] = echo.prev[0] = echo.prev[1] = echo.now[0] = echo.now[1] = 0;
        }
        return 0;
    }
    uint32_t time = c & ECHO_TIME_MASK;
    if (!echo.time) echo.time = time;
    if (!echo.fade && echo.time != time) { echo.old_time = echo.time; echo.time = time; echo.fade = ECHO_FADE; }
    echo.feedback = (int32_t)((c >> 16) & 127u) * FX12_GAIN_ONE / 100;
    echo.target_mix = on ? (int32_t)((c >> 23) & 127u) * FX12_GAIN_ONE / 100 : 0;
    return 1;
}

static inline void echo_render(int32_t dry, int32_t *left, int32_t *right)
{
    int32_t input;
    if (fx12_input(&echo.clock, dry, &input)) {
        int32_t tap[2];
        for (uint32_t s = 0; s < 2u; s++) {
            tap[s] = echo_tap(s, echo.time);
            if (echo.fade) {
                int32_t old = echo_tap(s, echo.old_time);
                tap[s] += (old - tap[s]) * (int32_t)echo.fade / (int32_t)ECHO_FADE;
            }
            echo.prev[s] = echo.now[s];
            echo.now[s] = tap[s];
            echo.lp[s] += (tap[s] - echo.lp[s]) * ECHO_DARK / 32768;   /* each pass a little darker */
        }
        if (echo.fade) echo.fade--;
        /* crossed: the left repeat goes on from the right, and back; a little of each side stays (the swing narrows) */
        int32_t to_l = (echo.lp[1] * (32768 - ECHO_BLEED) + echo.lp[0] * ECHO_BLEED) / 32768;
        int32_t to_r = (echo.lp[0] * (32768 - ECHO_BLEED) + echo.lp[1] * ECHO_BLEED) / 32768;
        echo_line[0][echo.head] = fx12_quantize(input / 16 + to_l * echo.feedback / FX12_GAIN_ONE);
        echo_line[1][echo.head] = fx12_quantize(to_r * echo.feedback / FX12_GAIN_ONE);
        if (++echo.head == ECHO_LEN) echo.head = 0;
        if (echo.valid < ECHO_LEN) echo.valid++;
    }
    echo.mix = fx12_slew_mix(echo.mix, echo.target_mix);
    if (echo.mix) {
        *left = fx12_output(&echo.clock, echo.prev[0], echo.now[0]);
        *right = fx12_output(&echo.clock, echo.prev[1], echo.now[1]);
    }
}
