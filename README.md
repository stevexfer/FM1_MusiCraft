# FM-1 MusiCraft

A cute, colourful voxel world you walk around in first person on the M-VAVE FM-1, with
four 16-step sequencers built into the landscape: bedrock-and-obsidian stages of 4 rows x 16 pads
(each with its own drum/synth kit: pentatonic FM bell, acid bass, music-box arp, and FM lead).
Look at any pad and toggle it; a lamp in the back wall follows the playhead, and each sequencer
scales its volume by proximity (full level within 10 blocks, attenuating from 10 to 25 blocks,
and silent at 25+ blocks). The 256 x 32 x 256 procedural map is streamed in and out in 8 x 8
chunks around the player.

Firmware on the Felucca / sloopDX FM-1 hardware layer (`firmware/hal`), with the same USB
update path as FM-1 Mario. Package identity `FM-1_971`.

## Controls

Note names, any octave, on the FM-1's keys and on MIDI (USB or TRS, any channel). Laid out
for Ableton Live's computer keyboard, so W A S D move you:

| Note (Live key) | Action |
| --- | --- |
| C# / D (W / S) | Forward / back |
| C / E (A / D) | Step left / right |
| F / G (F / G) | Turn left / right |
| D# / F# (E / T) | Look up / down |
| A (H) | Jump (swim up in water) |
| B (J) | Toggle the pad in the crosshair |
| A# (U) or PLAY | Play / stop |
| SELECT knob | Turn |
| ALGORITHM knob | Look up / down |
| KNOB 1 | Tempo (60-200 BPM) |
| HOME | A new island (the pattern stays) |

## Build

```bash
./build.sh                 # -> build/musicraft.fwsc (JieLi toolchain + AC79 SDK, as FM1_Mario)
tools/host_test.sh         # the engine on the Mac: build/host/*.ppm views, build/host/seq.wav
```

Flash `build/musicraft.fwsc` with the same WebUSB installer as FM-1 Mario.

## How it works

- `engine/voxel.c`: a 256 x 32 x 256 procedural world streamed in 8 x 32 x 8 chunks inside a
  96 x 32 x 96 active window, drawn at 120 x 120 (shown 2x) by marching one ray a pixel through
  the grid in integer fixed point with reciprocal lookup, row-invariant Y setup, and empty-sky
  culling; 16 x 16 textures generated at boot; fog into a pastel sky; the player (gravity,
  collision, one-block step-up, swimming).
- `engine/seq.c`: four 4 x 16 step sequencers with distinct kits and proximity-based level
  attenuation (10..25 blocks), rendered in the audio interrupt.
- `firmware/src/mc_glue.c`: the controls, the LCD stream (one frame renders while the
  other goes out by DMA), the boot and controls screens.

## Layout

| Path | Role |
| --- | --- |
| `engine/` | Portable C: world, renderer, player, sequencer |
| `firmware/hal/` | FM-1 hardware layer (Felucca / sloopDX) |
| `firmware/src/` | Boot, USB (MIDI, update), LCD, glue |
| `firmware/loader/` | The USB update loader |
| `tools/` | `build.py` (package), `host_test.sh` (engine on the host) |

GPL-3.0-only (see `COPYING`).
