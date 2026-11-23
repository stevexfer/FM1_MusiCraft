/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 MusiCraft: four 16-step, 4-track sequencers placed across the world.
 * Each sequencer has its own pattern, voice character, and proximity gain
 * (seq_gain[0..3] in 0..256: 256 at <= 10 blocks, fading to 0 at >= 25 blocks). */
#include "seq.h"

volatile uint8_t seq_pat_all[SEQ_COUNT][SEQ_TRACKS][SEQ_STEPS] = {
    { /* 0: Meadow Stage - classic kick, snare, hat, pentatonic FM bell */
        {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
        {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
        {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1},
        {1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0},
    },
    { /* 1: East Lagoon Stage - deep sub kick, rim/clap, shaker, FM acid bass */
        {1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0},
        {0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0},
        {1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1},
        {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0},
    },
    { /* 2: North Cherry Grove - pulse tom, woodblock, crisp tick, chiptune arp */
        {1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0},
        {0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1},
        {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
        {1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0},
    },
    { /* 3: West Highlands - electro kick, synth snare, ride, FM synth lead */
        {1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0},
        {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1},
        {1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1},
        {1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0},
    },
};

volatile uint16_t seq_gain[SEQ_COUNT] = {256, 0, 0, 0};
volatile uint8_t seq_step = SEQ_STEPS - 1;
volatile uint8_t seq_playing = 1;
volatile uint16_t seq_bpm = 112;

static int16_t SINE[1024];                      /* Q15 */
static uint32_t NOTE_INC[128];                  /* phase step a sample, 2^32 = one cycle */
static const uint8_t MELODY_NOTE[SEQ_COUNT][SEQ_STEPS] = {
    {72, 76, 79, 81, 84, 81, 79, 76, 74, 76, 79, 84, 86, 84, 79, 76},   /* 0: C5 pentatonic bell */
    {36, 36, 48, 39, 41, 43, 48, 46, 36, 39, 43, 46, 48, 46, 43, 39},   /* 1: C2 acid bassline */
    {84, 88, 91, 88, 86, 84, 81, 79, 84, 86, 88, 91, 93, 91, 88, 84},   /* 2: C6 music-box arp */
    {60, 64, 67, 71, 72, 71, 67, 64, 62, 65, 69, 72, 74, 72, 69, 67},   /* 3: C4 FM synth lead */
};

typedef struct {
    uint32_t kph, kinc, sph, bph, mph, binc;
    int32_t kenv, nenv, tenv, henv, benv, menv, hprev;
} seq_voice_t;

static struct {
    uint32_t left;                              /* samples to the next step, Q8 */
    uint32_t noise;
    seq_voice_t sv[SEQ_COUNT];
} v;

void seq_init(void)
{
    int64_t c = (int64_t)1 << 30, s = 0, inc = 42852281;   /* A4: 440 Hz */
    const int64_t C = 1073721611, S = 6588356, SEMI = 1137589835;
    int i;
    for (i = 0; i < 1024; i++) {
        int64_t nc = (c * C - s * S) >> 30, ns = (s * C + c * S) >> 30;
        int64_t q = (s + (1 << 14)) >> 15;
        SINE[i] = (int16_t)(q > 32767 ? 32767 : q);
        c = nc;
        s = ns;
    }
    for (i = 69; i < 128; i++, inc = inc * SEMI >> 30)
        NOTE_INC[i] = (uint32_t)inc;
    for (i = 68, inc = 42852281; i >= 0; i--) {
        inc = (inc << 30) / SEMI;
        NOTE_INC[i] = (uint32_t)inc;
    }
    v.noise = 0x1234567u;
    v.left = 0;
}

static const uint32_t KICK_START[SEQ_COUNT] = {15585000u, 12660000u, 21400000u, 23370000u};
static const uint32_t KICK_FLOOR[SEQ_COUNT] = {4400000u,  3500000u,  6300000u,  4800000u};
static const uint32_t SNARE_INC[SEQ_COUNT]  = {18505000u, 30200000u, 74000000u, 26000000u};

static void trigger(uint32_t step)
{
    uint32_t s;
    for (s = 0; s < SEQ_COUNT; s++) {
        seq_voice_t * u = &v.sv[s];
        if (seq_pat_all[s][0][step]) {
            u->kenv = 32767;
            u->kinc = KICK_START[s];
            u->kph = 0;
        }
        if (seq_pat_all[s][1][step]) {
            u->nenv = s == 2 ? 12000 : 26000;
            u->tenv = s == 2 ? 28000 : 20000;
            u->sph = 0;
        }
        if (seq_pat_all[s][2][step])
            u->henv = s == 1 ? 18000 : 22000;
        if (seq_pat_all[s][3][step]) {
            u->benv = 30000;
            u->menv = 32767;
            u->binc = NOTE_INC[MELODY_NOTE[s][step]];
            u->bph = u->mph = 0;
        }
    }
}

void seq_render(int32_t *out, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int32_t total = 0, nz;
        uint32_t s;
        if (seq_playing) {
            if (v.left < 256u) {
                uint32_t st = (seq_step + 1u) % SEQ_STEPS;
                seq_step = (uint8_t)st;
                v.left += SEQ_RATE * 60u * 256u / (4u * (seq_bpm ? seq_bpm : 1u));
                trigger(st);
            }
            v.left -= 256u;
        }
        v.noise ^= v.noise << 13;
        v.noise ^= v.noise >> 17;
        v.noise ^= v.noise << 5;
        nz = (int32_t)(v.noise >> 16) - 32768;
        for (s = 0; s < SEQ_COUNT; s++) {
            uint32_t g = seq_gain[s];
            seq_voice_t *u = &v.sv[s];
            int32_t mix = 0, hp;
            if (!g) {
                /* advance envelopes coarsely to zero when out of earshot */
                u->kenv = u->nenv = u->tenv = u->henv = u->benv = u->menv = 0;
                continue;
            }
            if (u->kenv) {                      /* kick / tom */
                mix += SINE[u->kph >> 22] * u->kenv >> 15;
                u->kph += u->kinc;
                if (u->kinc > KICK_FLOOR[s])
                    u->kinc -= u->kinc >> (s == 3 ? 9 : 10);
                u->kenv -= (u->kenv >> 12) + 1;
                if (u->kenv < 0)
                    u->kenv = 0;
            }
            if (u->nenv || u->tenv) {           /* snare / clap / woodblock */
                mix += (nz * u->nenv >> 15) * 9 / 16 + (SINE[u->sph >> 22] * u->tenv >> 15) / 2;
                u->sph += SNARE_INC[s];
                u->nenv -= (u->nenv >> 11) + 1;
                u->tenv -= (u->tenv >> (s == 2 ? 9 : 10)) + 1;
                if (u->nenv < 0)
                    u->nenv = 0;
                if (u->tenv < 0)
                    u->tenv = 0;
            }
            hp = nz - u->hprev;                 /* hat / shaker / tick */
            u->hprev = nz;
            if (u->henv) {
                mix += (hp >> 1) * u->henv >> 15 >> 2;
                u->henv -= (u->henv >> (s == 1 ? 10 : s == 2 ? 8 : 9)) + 1;
                if (u->henv < 0)
                    u->henv = 0;
            }
            if (u->benv) {                      /* FM voice: bell (3.5:1), acid bass (1:1), arp/lead (2:1) */
                int32_t m = SINE[u->mph >> 22] * u->menv >> 15;
                uint32_t fm_shift = s == 1 ? 16u : 15u;
                mix += (SINE[(u->bph + ((uint32_t)m << fm_shift)) >> 22] * u->benv >> 15) * 7 / 16;
                u->bph += u->binc;
                if (s == 0)
                    u->mph += u->binc * 3u + (u->binc >> 1);
                else if (s == 1)
                    u->mph += u->binc;
                else
                    u->mph += u->binc << 1;
                u->benv -= (u->benv >> (s == 1 ? 12 : 13)) + 1;
                u->menv -= (u->menv >> (s == 1 ? 10 : 11)) + 1;
                if (u->benv < 0)
                    u->benv = 0;
                if (u->menv < 0)
                    u->menv = 0;
            }
            total += (mix * (int32_t)g) >> 8;
        }
        total = total * 5 / 8;                  /* headroom for kick + snare + bell together */
        if (total > 32767)
            total = 32767;
        else if (total < -32768)
            total = -32768;
        out[2u * i] = total << 7;               /* 16 -> 24 bits, -6 dB */
        out[2u * i + 1u] = total << 7;
    }
}
