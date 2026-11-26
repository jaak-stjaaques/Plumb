# Licensing

PLUMB and its Dinghy foundation are free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## PLUMB

- The app (`firmware/src/dinghy.c`), the pendulums and the three engines (`firmware/src/plumb.h`), `tools/gen_tables.py`,
  `tools/gen_boot_logo.py`, the docs and the screens: jaak-stjaaques, written with Claude (Anthropic).
- PLUMB's own: the bloom of the reverb (in `dinghy.c`).
- From the same author's **LOOPHEADS**, **BEACHCOMBER** and **BUOY**: the delay and the hall (`delay.h`, `hall.h`,
  `fx12.h`), the scales (`scale.h`), the IBM Plex Mono screen font (`ui_font.h`,
  `tools/gen_ui_font.py`), the text drawing over a colour in `gfx.c`, and the look of the pages.
- The ideas it plays with belong to everyone: the pendulum wave (a physics classroom demonstration), the wavefolder and
  low-pass gate of West Coast synthesis, Casio's phase distortion, Curtis Roads' pulsar synthesis.
- The boot screen is [twelve plumb bobs caught mid-swing](assets/plumb-boot.png) (`assets/logo.png`), drawn by
  `tools/gen_boot_logo.py`; it replaces Dinghy's logo.

## Who made what (Dinghy)

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The SMK-37 board layer: `firmware/hal/smk_input.h`, `fm1_flash_map.h`, the SMK-37 parts of `fm1_cc.h`, `fm1_flash.h`, `fm1_lcd_hw.h`, `lcd.c`, `ota.c`, `bootguard.h`, `firmware/loader/`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/build.py`; `firmware/src/smk_wheel.h`; `tests/ota_test.c`, `ldr_test.c`, `install_test.py`, `smk_wheel_test.c`, `smk_pkg_test.py` | Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` (the reference port of the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit) |
| The TRS MIDI IN `firmware/src/midi_uart.c` | Felucca's, as sloopDX carries it, with the ring read by content from Felucca [Salt] 0.9-salt14 by ChanceTheMaker (found by keremimo) |
| The update loader's screen: `firmware/loader/ldr_screen.c`, `tests/ldr_screen_test.c` | Copyright (C) 2026 zvenson (from OberVave, the Firmware Hub's look); the idea, a loader that draws, its 5 x 7 letters and the order of things from [Skiff](https://github.com/illuminus5/skiff) by illuminus5, GPL-3.0 |
| From OberVave: the storage (blobs, slots), `LCD_IDLE`, the SMK-37 wiring of `platform.c`, `store.h`, the pad event queue in `smk_input.h`, `tests/smk_flash_test.c`, `tests/run_tests.sh`, the CI workflow | Copyright (C) 2026 zvenson |
| From OberVave (0.6): `firmware/src/kit_clock.h` (the MIDI clock lock), `kit_midi.h`, `kit_backup.h`, `kit_link.h`, `kit_mirror.h` and the mirror hooks in `lcd.c`, `MIDI_RT_HOOK` in `usb.c` and `midi_uart.c`; `tests/clock_sync_test.py`, `tests/backup_test.c` | Copyright (C) 2026 zvenson; the backup asked for by dmochen (zvenson/obervave#15) |
| The wiring `platform.c`, the host mode `tests/`, Dinghy's own docs (`docs/API.md`, `RULES.md`, `SMK37.md`, `MAKE-IT-YOURS.md`); Dinghy's sine app, which PLUMB's `dinghy.c` replaced | zvenson |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| The screen mirror: `firmware/src/screen_mirror/` (`screen_mirror.c`, `.h`), Copyright (c) 2026 Patrick F. Kellogg, for his [FM-1 Mirror](https://github.com/pfkellogg/fm1-mirror) app | MIT | `firmware/src/screen_mirror/LICENSE` |
| IBM Plex Mono Medium, rendered into `firmware/gen/ui_font.h` | SIL OFL 1.1 | `assets/fonts/IBMPlexMono-OFL.txt` |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE", "FM-1", "SMK-37" and "MKE-P37"
belong to their owners. PLUMB and Dinghy are not made or endorsed by them.
