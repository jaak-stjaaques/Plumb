/* SPDX-License-Identifier: GPL-3.0-only */
/* The three effects in PARALLEL: each hears the dry sound on its own, none feeds another.
 *   out = dry + delay x DELAY + hall x HALL + chorus x CHORUS */
#include "delay.h"
#include "hall.h"
#include "chorus.h"

static void effects_process(int32_t *out, uint32_t frames)
{
    uint8_t delay_active = delay_begin_block();
    uint8_t hall_active = hall_begin_block();
    uint8_t chorus_active = chorus_begin_block();
    if (!delay_active && !hall_active && !chorus_active) return;

    for (uint32_t frame = 0; frame < frames; frame++) {
        int32_t *pair = &out[2u * frame];
        int32_t dry = (pair[0] + pair[1]) / 2;
        int32_t echo = delay_active ? delay_render(dry) : 0;
        int32_t hl = 0, hr = 0, cl = 0, cr = 0;
        if (hall_active) hall_render(dry, &hl, &hr);
        if (chorus_active) chorus_render(dry, &cl, &cr);
        int32_t dm = delay_state.mix, hm = hall.mix, cm = chorus.mix;
        int32_t e = echo * dm / FX12_GAIN_ONE;
        pair[0] += e + hl * hm / FX12_GAIN_ONE + cl * cm / FX12_GAIN_ONE;
        pair[1] += e + hr * hm / FX12_GAIN_ONE + cr * cm / FX12_GAIN_ONE;
    }
}
