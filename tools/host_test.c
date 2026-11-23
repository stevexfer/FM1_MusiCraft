/* SPDX-License-Identifier: GPL-3.0-only */
/* Host check of the engine (tools/host_test.sh): renders views to PPM, walks
 * the player across chunks between sequencers, verifies proximity attenuation
 * (<= 10 blocks full, 10..25 attenuating, >= 25 silent), toggles pads, and
 * records the spatial sequencers to WAV. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "voxel.h"
#include "seq.h"

static uint16_t fb[VX_RW * VX_RH];

static void ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f)
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (y = 0; y < 240; y++)
        for (x = 0; x < 240; x++) {
            uint16_t s = fb[(y / 2) * VX_RW + x / 2];
            uint16_t c = (uint16_t)((s >> 8) | (s << 8));
            unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                                    (unsigned char)((c & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void shot(vx_player *p, const char *name)
{
    char path[128];
    clock_t t0 = clock();
    int i;
    for (i = 0; i < 50; i++)
        vx_render(p, vx_pick(p), fb, NULL);
    snprintf(path, sizeof path, "build/host/%s.ppm", name);
    ppm(path);
    printf("%-10s pos %6.2f %5.2f %6.2f yaw %3d pitch %4d  %.3f ms/frame (host)\n", name, p->x / 65536.0,
           p->y / 65536.0, p->z / 65536.0, (int)p->yaw, (int)p->pitch,
           (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 50);
}

int main(int argc, char **argv)
{
    vx_player p;
    vx_input in;
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 0x4D5543u;
    int i, q, tr, st;
    vx_init();
    seq_init();
    vx_generate(seed, &p);
    for (q = 0; q < VX_SEQS; q++)
        for (tr = 0; tr < VX_TRACKS; tr++)
            for (st = 0; st < VX_STEPS; st++)
                vx_stage_pad_ex(q, tr, st, seq_pat_all[q][tr][st]);
    vx_stage_lamp(4);
    shot(&p, "spawn");
    printf("spawn proximity gains: seq0=%u seq1=%u seq2=%u seq3=%u (chunks loaded=%u unloaded=%u)\n",
           seq_gain[0], seq_gain[1], seq_gain[2], seq_gain[3], vx_chunks_loaded, vx_chunks_unloaded);

    memset(&in, 0, sizeof in);
    in.turn = 512;                              /* look back at the island */
    vx_update(&p, &in, 16);
    in.turn = 0;
    p.pitch = 0;
    shot(&p, "island");
    in.turn = 512;
    vx_update(&p, &in, 16);
    in.turn = 0;
    in.fwd = 1;                                 /* walk onto stage 0 */
    for (i = 0; i < 100; i++)
        vx_update(&p, &in, 16);
    in.fwd = 0;
    p.pitch = -200;
    {
        int32_t t = vx_pick(&p);
        printf("on stage 0: pick %d stage hit %d", (int)t, vx_stage_hit_ex(t, &q, &tr, &st));
        if (vx_stage_hit_ex(t, &q, &tr, &st)) {
            printf(" seq %d track %d step %d -> toggled", q, tr, st);
            seq_pat_all[q][tr][st] ^= 1u;
            vx_stage_pad_ex(q, tr, st, seq_pat_all[q][tr][st]);
        }
        printf("\n");
    }
    shot(&p, "stage");
    in.jump = 1;
    vx_update(&p, &in, 16);
    in.jump = 0;
    for (i = 0; i < 20; i++)
        vx_update(&p, &in, 16);
    printf("jump: feet at %.2f after 0.34 s\n", p.y / 65536.0);
    p.pitch = 40;
    p.yaw = 300;
    shot(&p, "side");

    /* Walk east from Stage 0 (x=120..137) to Stage 1 (x=172..189), streaming chunks
     * and checking proximity attenuation at 10..25 blocks and silence at >= 25 blocks */
    p.yaw = 256;                                /* face +X (East) */
    p.pitch = -30;
    in.fwd = 1;
    for (i = 0; i < 720; i++) {
        vx_update(&p, &in, 16);
        if (i % 120 == 0 || i == 719) {
            double d0 = vx_seq_dist_q8(&p, 0) / 256.0;
            double d1 = vx_seq_dist_q8(&p, 1) / 256.0;
            printf("walk east step %3d: x=%6.2f  dist0=%5.1f (gain=%3u)  dist1=%5.1f (gain=%3u)  chunks +%u/-%u\n",
                   i, p.x / 65536.0, d0, seq_gain[0], d1, seq_gain[1], vx_chunks_loaded, vx_chunks_unloaded);
        }
    }
    in.fwd = 0;
    p.yaw = 0;
    p.pitch = -60;
    shot(&p, "stage1");

    {
        static int32_t pcm[44100 * 2 * 6];
        FILE *w = fopen("build/host/seq.wav", "wb");
        int n = 44100 * 6;
        uint32_t data = (uint32_t)n * 2, rate = 44100, brate = rate * 2, riff = 36 + data, fmtlen = 16;
        uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
        seq_render(pcm, (unsigned)n);
        if (w) {
            fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
            fwrite(&fmtlen, 4, 1, w); fwrite(&fmt, 2, 1, w); fwrite(&ch, 2, 1, w);
            fwrite(&rate, 4, 1, w); fwrite(&brate, 4, 1, w); fwrite(&align, 2, 1, w); fwrite(&bits, 2, 1, w);
            fwrite("data", 1, 4, w); fwrite(&data, 4, 1, w);
            for (i = 0; i < n; i++) {
                int16_t s = (int16_t)(pcm[2 * i] >> 7);
                fwrite(&s, 2, 1, w);
            }
            fclose(w);
        }
    }
    return 0;
}
