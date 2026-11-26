/* SPDX-License-Identifier: GPL-3.0-only */
/* PLUMB's two effects, in PARALLEL: each hears the dry sound on its own, none feeds the other.
 *   out = dry + ECHO (delay.h, LOOPHEADS' 12-bit delay) + REVERB (hall.h; its tail BLOOMs as the pendulums meet:
 *   dinghy.c moves its decay, brightness and level with the cycle) */
#include "delay.h"
#include "hall.h"

static void effects_process(int32_t *out, uint32_t frames)
{
    uint8_t delay_active = delay_begin_block();
    uint8_t hall_active = hall_begin_block();
    if (!delay_active && !hall_active) return;

    for (uint32_t frame = 0; frame < frames; frame++) {
        int32_t *pair = &out[2u * frame];
        int32_t dry = (pair[0] + pair[1]) / 2;
        int32_t echo = delay_active ? delay_render(dry) : 0;
        int32_t hl = 0, hr = 0;
        if (hall_active) hall_render(dry / 2, &hl, &hr);   /* half as hot into the room (its 12 bits never clip,
                                                 * even blooming), twice as loud out of it */
        int32_t e = echo * delay_state.mix / FX12_GAIN_ONE, hm = hall.mix;
        pair[0] += e + 2 * (hl * hm / FX12_GAIN_ONE);
        pair[1] += e + 2 * (hr * hm / FX12_GAIN_ONE);
    }
}
