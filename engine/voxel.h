/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 MusiCraft: the voxel world, its renderer and the player. Portable C,
 * integers only (the firmware builds without the FPU): positions are Q16
 * blocks, directions Q14, angles 1024 to the turn.
 *
 * The procedural world is 256 x 32 x 256 blocks (16x larger area than 64x64),
 * streamed in and out in 8 x 32 x 8 chunks inside a 96 x 32 x 96 active window
 * (12 x 12 = 144 chunks) centered on the player. Four 4 x 16 sequencer stages
 * are placed across the world with proximity-scaled audio (full <= 10 blocks,
 * attenuating 10..25 blocks, silent >= 25 blocks). */
#pragma once
#include <stdint.h>

#define VX_W 256                        /* world x (blocks) */
#define VX_H 32                         /* world y, up (blocks) */
#define VX_D 256                        /* world z (blocks) */

#define VX_CW 8                         /* chunk x size (blocks) */
#define VX_CD 8                         /* chunk z size (blocks) */
#define VX_WIN_CW 12                    /* active window width in chunks */
#define VX_WIN_CD 12                    /* active window depth in chunks */
#define VX_WIN_W (VX_WIN_CW * VX_CW)    /* active window x: 96 blocks */
#define VX_WIN_D (VX_WIN_CD * VX_CD)    /* active window z: 96 blocks */
#define VX_N (VX_WIN_W * VX_H * VX_WIN_D)
#define VX_IDX(lx, y, lz) ((y) * (VX_WIN_W * VX_WIN_D) + (lz) * VX_WIN_W + (lx))

#define VX_SEA 7                        /* the top water layer; the sea goes on past the edges */
#define VX_RW 120                       /* the picture: 120 x 120, shown 2x on the 240 x 240 LCD */
#define VX_RH 120

enum {
    B_AIR, B_GRASS, B_DIRT, B_STONE, B_SAND, B_WATER, B_LOG, B_LEAF_PINK, B_LEAF_GREEN,
    B_FLOWER_RED, B_FLOWER_YEL, B_BEDROCK, B_OBSIDIAN, B_PAD_OFF, B_PAD_ON0, B_PAD_ON1, B_PAD_ON2,
    B_PAD_ON3, B_LAMP_OFF, B_LAMP_ON, B_CLOUD, B_COUNT
};

extern uint8_t vx_world[VX_N];
extern int32_t vx_win_x0, vx_win_z0;            /* world block origin of the active 96x96 window */
extern uint32_t vx_chunks_loaded;               /* total 8x8 chunks loaded */
extern uint32_t vx_chunks_unloaded;             /* total 8x8 chunks unloaded */

typedef struct {
    int32_t x, y, z;                    /* the feet, Q16 */
    int32_t vy;                         /* Q16 blocks a second */
    int32_t yaw, pitch;                 /* 0..1023; -240..240 (+ = up) */
    uint8_t on_ground, in_water, eye_in_water;
} vx_player;

typedef struct {
    uint8_t fwd, back, left, right, jump;
    int32_t turn, look;                 /* angle units this frame */
} vx_input;

/* the sequencer stages: 4 stages across the map, each 4 rows (tracks) of 16 pads + lamp wall */
#define VX_SEQS 4
#define VX_TRACKS 4
#define VX_STEPS 16

void vx_init(void);                                         /* tables and textures, once */
void vx_generate(uint32_t seed, vx_player *p);              /* new world, 4 stages, p at stage 0 spawn */
void vx_update(vx_player *p, const vx_input *in, uint32_t dt_ms);
void vx_sync_chunks(int32_t px, int32_t pz);                /* stream 8x8 chunks around Q16 (px, pz) */
void vx_update_proximity(const vx_player *p);               /* update seq_gain[0..3] from distance */
uint32_t vx_seq_dist_q8(const vx_player *p, int seq);       /* Q8 block distance to any part of stage seq */
void vx_stage_pos(int seq, int32_t *x0, int32_t *y0, int32_t *z0);

int32_t vx_pick(const vx_player *p);                        /* the block in the crosshair (reach 5), or -1 */
void vx_render(const vx_player *p, int32_t target, uint16_t *fb, void (*row_done)(int y));

void vx_stage_pad_ex(int seq, int track, int step, int on);
void vx_stage_pad(int track, int step, int on);
void vx_stage_lamp(int step);                               /* -1: all off */
int vx_stage_hit_ex(int32_t idx, int *seq, int *track, int *step);
int vx_stage_hit(int32_t idx, int *track, int *step);       /* idx is a pad: 1 */
