/* SPDX-License-Identifier: GPL-3.0-only */
/* PLUMB's two effects, in PARALLEL: each hears the dry sound on its own, none feeds the other.
 *   out = dry + SWING ECHO (echo.h: a ping-pong that comes to rest) + REVERB (hall.h; its tail BLOOMs as the
 *   pendulums meet: dinghy.c moves its decay with the cycle) */
#include "echo.h"
#include "hall.h"

static void effects_process(int32_t *out, uint32_t frames)
{
    uint8_t echo_active = echo_begin_block();
    uint8_t hall_active = hall_begin_block();
    if (!echo_active && !hall_active) return;

    for (uint32_t frame = 0; frame < frames; frame++) {
        int32_t *pair = &out[2u * frame];
        int32_t dry = (pair[0] + pair[1]) / 2;
        int32_t el = 0, er = 0, hl = 0, hr = 0;
        if (echo_active) echo_render(dry, &el, &er);
        if (hall_active) hall_render(dry, &hl, &hr);
        int32_t em = echo.mix, hm = hall.mix;
        pair[0] += el * em / FX12_GAIN_ONE + hl * hm / FX12_GAIN_ONE;
        pair[1] += er * em / FX12_GAIN_ONE + hr * hm / FX12_GAIN_ONE;
    }
}
