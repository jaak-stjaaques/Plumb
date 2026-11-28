# Changelog

## 0.5 – 2026-11-27

- **LATCH really keeps the chord.** When the keys of a chord come up, one of them can bounce (seen "pressed" again
  for an instant, on the FM-1's key scan and in the emulator). 0.4 took that for a new chord and kept only that one
  note. Now a bounce of one of the chord's own notes within 150 ms of the latch is ignored; a new chord still
  replaces the latched one.

## 0.4 – 2026-11-27

(An earlier 0.4, 0.5 and 0.6 tried a blooming reverb and a swing echo; they were taken back. This 0.4 is 0.3 plus:)

- **LATCH keeps the whole chord.** With the ARP on and LATCH ON, every note held keeps playing when the keys are let
  go, even one by one (before, only the last key let go stayed). Once every key is up, the next key starts a new
  chord. MIDI notes count as keys.

## 0.3 – 2026-11-25

- **CYCLE goes to 96 and 128 beats**: at 96 BPM, 128 beats is 80 seconds before they meet again. Saved presets keep
  their cycles.

## 0.2 – 2026-11-25

- **No more crackle.** A pendulum that strikes while all eight voices sound takes one over: that voice now fades out
  over 1.5 ms before the new strike, where it used to be cut off (and silenced until its strike). The envelope never
  jumps from silence in less than 1.5 ms. PULSAR's grain and PHASE's bend change only as a cycle starts; FOLD's fold
  glides; the LFO's LEVEL moves smoothly; FOLD's low-pass gate never opens fully (hard folds alias). On the host:
  from 451 clicks to 23 over the eight presets, the rest FOLD's own corners.
- **LFO TARGET: FOLDS and SPARK.** TIMBRE is now called after what it moves (FOLDS, DCW or FORMANT), and SPARK is new.
  The order on the knob: PITCH, FOLDS, SPARK, LEVEL, GRAVITY. Saved presets keep their targets.
- **Silent at power-on**: the pendulums hang still until PLAY.

## 0.1 – 2026-11-25

The first PLUMB: a pendulum wave on the FM-1.

- Twelve pendulums, released together by PLAY (or a MIDI START), meeting again at the end of each CYCLE.
- SEQ: CYCLE, SWINGS, STRIKE (middle, one way, left wall, both walls), GRID (free, or 1/32 .. 1/4).
- HOME: the row (a note per pendulum, eight ROW shapes, REC into the row), PENDULUMS, DAMP; SEL catches a pendulum.
- Three oscillator engines: FOLD (wavefolder and low-pass gate), PHASE (CZ phase distortion), PULSAR (pulsar
  synthesis); SPARK, WIDTH.
- ARP: the pendulums play the keys held (UP, DOWN, BOUNCE, RANDOM, SPREAD), RANGE, CHANCE, LATCH.
- ENV, LFO (PITCH, TIMBRE, LEVEL, GRAVITY), FX (reverb, echo, chorus in parallel), 48 scales, 8 presets and 8 USER.
- On Dinghy 0.6: MIDI clock sync, MIDI in and out, USB audio, FM-1 Mirror, the backup page.
- Tested on the host only (screens, sound, the clock sync test); not yet on an FM-1.
