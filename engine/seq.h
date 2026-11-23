/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 MusiCraft: 4 spatial 16-step sequencers with distinct synth/drum kits
 * and proximity-based level scaling (full level within 10 blocks of any part of
 * a sequencer, attenuating 10..25 blocks, silent at >= 25 blocks).
 * seq_render runs in the audio interrupt; integers only. */
#pragma once
#include <stdint.h>

#define SEQ_COUNT 4
#define SEQ_TRACKS 4
#define SEQ_STEPS 16
#define SEQ_RATE 44100u

extern volatile uint8_t seq_pat_all[SEQ_COUNT][SEQ_TRACKS][SEQ_STEPS];
#define seq_pat (seq_pat_all[0])
extern volatile uint16_t seq_gain[SEQ_COUNT];   /* 0..256 proximity level per sequencer */
extern volatile uint8_t seq_step;               /* the step playing */
extern volatile uint8_t seq_playing;
extern volatile uint16_t seq_bpm;

void seq_init(void);
void seq_render(int32_t *out, unsigned n);      /* n stereo frames, 24-bit in int32 */
