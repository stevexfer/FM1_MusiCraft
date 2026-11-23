/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 MusiCraft: the voxel world. A 256 x 32 x 256 procedural archipelago on
 * an endless sea, streamed in 8 x 32 x 8 chunks inside a 96 x 32 x 96 active
 * window around the player. Drawn by marching one ray a pixel through the grid
 * (the integer DDA of Amanatides & Woo) with row-invariant Y setup, reciprocal
 * lookup table, empty-sky ceiling culling, and fast opaque block termination.
 * Four 4 x 16 step-sequencer stages sit across the world with proximity-scaled
 * audio (full <= 10 blocks, attenuating 10..25 blocks, silent >= 25 blocks). */
#include "voxel.h"
#include "seq.h"

uint8_t vx_world[VX_N];
int32_t vx_win_x0 = -999999, vx_win_z0 = -999999;
uint32_t vx_chunks_loaded;
uint32_t vx_chunks_unloaded;

/* ---- palette: pastel, index 0 = see-through */
enum {
    P_NONE, P_GRASS, P_GRASS_D, P_GRASS_L, P_DIRT, P_DIRT_D, P_DIRT_L, P_STONE, P_STONE_D, P_STONE_L,
    P_SAND, P_SAND_D, P_WATER, P_WATER_L, P_BARK, P_BARK_D, P_RING, P_PINK, P_PINK_D, P_PINK_L,
    P_LEAF, P_LEAF_D, P_RED, P_YEL, P_STEM, P_EYE, P_BED, P_BED_D, P_BED_L, P_OBS, P_OBS_D, P_OBS_L,
    P_PAD, P_PADRIM, P_ON0, P_ON1, P_ON2, P_ON3, P_ONL0, P_ONL1, P_ONL2, P_ONL3, P_LAMP, P_LAMP_D,
    P_LAMP_ON, P_LAMP_HI, P_CLOUD, P_CLOUD_D, P_COUNT
};
static const uint8_t PAL[P_COUNT][3] = {
    {0, 0, 0}, {126, 214, 98}, {100, 192, 84}, {158, 232, 120}, {170, 116, 80}, {140, 94, 64},
    {192, 140, 98}, {152, 152, 168}, {126, 126, 142}, {178, 178, 192}, {244, 226, 164}, {226, 204, 142},
    {96, 172, 242}, {150, 208, 252}, {150, 104, 74}, {116, 78, 56}, {216, 176, 124}, {255, 176, 206},
    {242, 144, 184}, {255, 208, 226}, {96, 186, 96}, {72, 156, 80}, {255, 92, 112}, {255, 222, 84},
    {84, 172, 72}, {255, 244, 160}, {74, 72, 84}, {50, 48, 60}, {104, 102, 114}, {56, 34, 88},
    {34, 20, 58}, {128, 88, 190}, {96, 90, 116}, {64, 60, 80}, {255, 96, 106}, {255, 206, 70},
    {84, 222, 232}, {196, 120, 255}, {255, 186, 186}, {255, 242, 178}, {190, 252, 252}, {232, 196, 255},
    {118, 106, 76}, {84, 76, 54}, {255, 236, 140}, {255, 255, 228}, {255, 255, 255}, {232, 238, 250},
};
static const uint8_t FOG[3] = {205, 228, 255};      /* = the sky at the horizon */
static const uint8_t ZENITH[3] = {120, 178, 255};
static const int16_t LIGHT[4] = {256, 222, 192, 150}; /* top, x faces, z faces, bottom */

#define EYE 98304                       /* eye height 1.5 blocks */
#define MAXT (30 << 16)                 /* the view distance: 30 blocks (full fog) */
#define TINF (1u << 30)
#define KPIX 765                        /* tan(35 deg) / 60, Q16: a 70 degree view over 120 pixels */

static int16_t SIN[1024];                       /* Q14 */
static uint8_t TEX[B_COUNT][3][256];            /* faces: top, side, bottom */
static uint16_t LUT[4][16][P_COUNT];            /* light x fog x palette -> panel RGB565 */
static uint16_t SKY[33];
static uint16_t SEA[16];
static uint32_t RECIP[4096];                    /* RECIP[d] = TINF / d, eliminates per-pixel divides */

#define isin(a) SIN[(a) & 1023]
#define icos(a) SIN[((a) + 256) & 1023]

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

static uint16_t panel565(uint32_t r, uint32_t g, uint32_t b)   /* byte-swapped for the panel */
{
    uint32_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t mix8(uint32_t a, uint32_t b, uint32_t k, uint32_t n)  /* a + (b - a) k / n */
{
    return (a * (n - k) + b * k) / n;
}

static uint8_t pick3(uint32_t h, uint8_t a, uint8_t b, uint8_t c)
{
    h &= 255u;
    return h < 150u ? a : h < 215u ? b : c;
}

static uint8_t texel(int b, int f, int u, int v)
{
    uint32_t h = hash3((uint32_t)(b * 3 + f), (uint32_t)u, (uint32_t)v);
    int rim = u == 0 || u == 15 || v == 0 || v == 15, rim2 = u <= 1 || u >= 14 || v <= 1 || v >= 14;
    int du = 2 * u - 15, dv, d2;
    switch (b) {
    case B_GRASS:
        if (f == 0 || (f == 1 && (v < 3 || (v == 3 && (h & 256u)))))
            return pick3(h, P_GRASS, P_GRASS_D, P_GRASS_L);
        return pick3(h, P_DIRT, P_DIRT_D, P_DIRT_L);
    case B_DIRT:
        return pick3(h, P_DIRT, P_DIRT_D, P_DIRT_L);
    case B_STONE:
        return pick3(h, P_STONE, P_STONE_D, P_STONE_L);
    case B_SAND:
        return pick3(h, P_SAND, P_SAND_D, P_SAND);
    case B_WATER:
        return ((u + 2 * v + (int)(h & 1u)) & 7) == 0 ? P_WATER_L : P_WATER;
    case B_LOG:
        if (f == 1)
            return (u % 4 == 1 || (h & 255u) < 40u) ? P_BARK_D : P_BARK;
        dv = 2 * v - 15;
        d2 = (du * du + dv * dv) / 4;
        return d2 > 49 ? P_BARK : ((d2 / 9) & 1) ? P_BARK_D : P_RING;
    case B_LEAF_PINK:
        return (h & 255u) < 56u ? P_NONE : pick3(h >> 8, P_PINK, P_PINK_D, P_PINK_L);
    case B_LEAF_GREEN:
        return (h & 255u) < 56u ? P_NONE : pick3(h >> 8, P_LEAF, P_LEAF_D, P_GRASS_L);
    case B_FLOWER_RED:
    case B_FLOWER_YEL:
        if (f == 2)
            return P_NONE;
        dv = f == 0 ? 2 * v - 15 : 2 * v - 9;
        d2 = du * du + dv * dv;
        if (d2 <= 8)
            return P_EYE;
        if (d2 <= 44)
            return b == B_FLOWER_RED ? P_RED : P_YEL;
        if (f == 1 && (u == 7 || u == 8) && v >= 8)
            return P_STEM;
        if (f == 1 && v >= 11 && v <= 12 && (u == 5 || u == 6 || u == 9 || u == 10))
            return P_STEM;
        return P_NONE;
    case B_BEDROCK:
        return pick3(h, P_BED, P_BED_D, P_BED_L);
    case B_OBSIDIAN:
        return (h & 255u) < 18u ? P_OBS_L : (h & 255u) < 120u ? P_OBS_D : P_OBS;
    case B_PAD_OFF:
        return f == 0 && !rim2 ? P_PAD : P_PADRIM;
    case B_PAD_ON0:
    case B_PAD_ON1:
    case B_PAD_ON2:
    case B_PAD_ON3:
        if (f != 0)
            return (uint8_t)(P_ON0 + b - B_PAD_ON0);
        if (rim2)
            return P_PADRIM;
        return (uint8_t)((u >= 5 && u <= 10 && v >= 5 && v <= 10 ? P_ONL0 : P_ON0) + b - B_PAD_ON0);
    case B_LAMP_OFF:
        return rim || u % 5 == 0 || v % 5 == 0 ? P_LAMP_D : P_LAMP;
    case B_LAMP_ON:
        return rim || u % 5 == 0 || v % 5 == 0 ? P_LAMP_ON : P_LAMP_HI;
    case B_CLOUD:
        return (h & 255u) < 48u ? P_CLOUD_D : P_CLOUD;
    default:
        return P_NONE;
    }
}

void vx_init(void)
{
    int64_t c = (int64_t)1 << 30, s = 0;
    const int64_t C = 1073721611, S = 6588356;  /* cos, sin of a 1024th of a turn, Q30 */
    int i, b, f, u, v, l, k;
    for (i = 0; i < 1024; i++) {
        int64_t nc = (c * C - s * S) >> 30, ns = (s * C + c * S) >> 30;
        SIN[i] = (int16_t)((s + (1 << 15)) >> 16);
        c = nc;
        s = ns;
    }
    for (b = 0; b < B_COUNT; b++)
        for (f = 0; f < 3; f++)
            for (v = 0; v < 16; v++)
                for (u = 0; u < 16; u++)
                    TEX[b][f][v * 16 + u] = texel(b, f, u, v);
    for (l = 0; l < 4; l++)
        for (f = 0; f < 16; f++)
            for (k = 0; k < P_COUNT; k++) {
                uint32_t rgb[3];
                for (i = 0; i < 3; i++)
                    rgb[i] = mix8((uint32_t)PAL[k][i] * (uint32_t)LIGHT[l] >> 8, FOG[i], (uint32_t)f, 15u);
                LUT[l][f][k] = panel565(rgb[0], rgb[1], rgb[2]);
            }
    for (i = 0; i <= 32; i++)
        SKY[i] = panel565(mix8(FOG[0], ZENITH[0], (uint32_t)i, 32u), mix8(FOG[1], ZENITH[1], (uint32_t)i, 32u),
                          mix8(FOG[2], ZENITH[2], (uint32_t)i, 32u));
    for (f = 0; f < 16; f++)
        SEA[f] = LUT[0][f][P_WATER];
    RECIP[0] = TINF;
    for (i = 1; i < 4096; i++)
        RECIP[i] = TINF / (uint32_t)i;
}

/* ---- fast reciprocal & fixed-point helpers */
static inline uint32_t mulq16(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b) >> 16);
}

static inline uint32_t recip_tinf(uint32_t u)
{
    if (u < 4096u)
        return RECIP[u];
    if (u < 32752u) {
        uint32_t q = u >> 3, a = RECIP[q], b = RECIP[q + 1u];
        return (a - (((a - b) * (u & 7u)) >> 3)) >> 3;
    }
    return TINF / u;
}

static inline int is_glow(uint32_t b)
{
    return (b >= B_PAD_ON0 && b <= B_PAD_ON3) || b == B_LAMP_ON;
}

/* maximum Y of any non-cloud block in the world (terrain <= 16, trees <= 21) */
#define VX_GROUND_MAX_Y 21u
#define VX_CLOUD_MIN_Y  27u
#define VX_CLOUD_TOP_Y  29u

/* ---- the renderer */
void vx_render(const vx_player *p, int32_t target, uint16_t *fb, void (*row_done)(int y))
{
    const int32_t sy = isin(p->yaw), cy = icos(p->yaw), sp = isin(p->pitch), cp = icos(p->pitch);
    const int32_t fx = sy * cp >> 14, fy = sp, fz = cy * cp >> 14;          /* forward */
    const int32_t ux = -(sy * sp >> 14), uy = cp, uz = -(cy * sp >> 14);    /* up */
    /* a ray, Q20: forward + column * h + row * w, centred on the middle of the picture */
    const int32_t hx = cy * KPIX >> 10, hz = -sy * KPIX >> 10;
    const int32_t wx = -ux * KPIX >> 10, wy = -uy * KPIX >> 10, wz = -uz * KPIX >> 10;
    int32_t ex, ey, ez, ix, iy, iz, eidx, skip_water;
    uint32_t ox, oy, oz;
    const int32_t sea_top = (VX_SEA + 1) << 16;
    int32_t row, col;

    vx_sync_chunks(p->x, p->z);
    ex = p->x - (vx_win_x0 << 16);
    ey = p->y + EYE;
    ez = p->z - (vx_win_z0 << 16);
    ix = ex >> 16;
    iy = ey >> 16;
    iz = ez >> 16;
    ox = (uint32_t)ex & 0xFFFFu;
    oy = (uint32_t)ey & 0xFFFFu;
    oz = (uint32_t)ez & 0xFFFFu;
    eidx = VX_IDX(ix, iy, iz);
    skip_water = (unsigned)iy < VX_H && vx_world[eidx] == B_WATER;

    for (row = 0; row < VX_RH; row++) {
        int32_t ax = (fx << 6) + (2 * row - (VX_RH - 1)) * wx / 2 - (VX_RW - 1) * hx / 2;
        const int32_t ay = (fy << 6) + (2 * row - (VX_RH - 1)) * wy / 2;
        int32_t az = (fz << 6) + (2 * row - (VX_RH - 1)) * wz / 2 - (VX_RW - 1) * hz / 2;
        const int32_t dy = ay >> 6;
        uint32_t tdy = TINF, tmy0 = TINF, t_cloud = TINF, vy_limit = VX_CLOUD_TOP_Y;
        int32_t sy_ = 0, sy_stride = 0, face_y = 0, row_skip = 0;
        uint16_t px_miss;
        uint16_t *out = fb + row * VX_RW;

        /* Y ray setup and sky/sea miss color are invariant across the entire row */
        if (dy > 0) {
            tdy = recip_tinf((uint32_t)dy);
            tmy0 = mulq16(0x10000u - oy, tdy);
            sy_ = 1;
            sy_stride = VX_WIN_W * VX_WIN_D;
            face_y = 3;
            if (iy >= (int32_t)VX_CLOUD_TOP_Y) {
                row_skip = 1;
            } else if (iy < (int32_t)VX_CLOUD_MIN_Y) {
                t_cloud = tmy0 + (uint32_t)((int32_t)(VX_CLOUD_MIN_Y - 1u) - iy) * tdy;
                vy_limit = VX_GROUND_MAX_Y + 1u;
                if (t_cloud > MAXT && iy > (int32_t)VX_GROUND_MAX_Y)
                    row_skip = 1;
            }
        } else if (dy < 0) {
            tdy = recip_tinf((uint32_t)-dy);
            tmy0 = mulq16(oy, tdy);
            sy_ = -1;
            sy_stride = -(VX_WIN_W * VX_WIN_D);
            face_y = 0;
        } else {
            if ((iy > (int32_t)VX_GROUND_MAX_Y && iy < (int32_t)VX_CLOUD_MIN_Y) || iy >= (int32_t)VX_CLOUD_TOP_Y)
                row_skip = 1;
        }

        if (dy < 0 && ey > sea_top) {
            uint32_t tq = mulq16((uint32_t)(ey - sea_top), tdy) >> 6;
            px_miss = SEA[tq >= (MAXT >> 6) ? 15 : (tq << 6) >> 17];
        } else {
            int32_t k = dy >> 9;
            px_miss = SKY[k < 0 ? 0 : k > 32 ? 32 : k];
        }

        if (row_skip) {
            for (col = 0; col < VX_RW; col++)
                out[col] = px_miss;
            if (row_done)
                row_done(row);
            continue;
        }

        for (col = 0; col < VX_RW; col++, ax += hx, az += hz) {
            const int32_t dx = ax >> 6, dz = az >> 6;
            uint32_t tdx = TINF, tdz = TINF, tmx = TINF, tmy = tmy0, tmz = TINF, t = 0;
            uint32_t cur_vy_lim = vy_limit;
            int32_t sx = 0, sz_stride = 0, vy = iy, idx = eidx, face = 0;
            uint32_t b, c;
            int32_t tt, u, v, light, fog;
            uint16_t px;

            if (dx > 0) {
                tdx = recip_tinf((uint32_t)dx);
                tmx = mulq16(0x10000u - ox, tdx);
                sx = 1;
            } else if (dx < 0) {
                tdx = recip_tinf((uint32_t)-dx);
                tmx = mulq16(ox, tdx);
                sx = -1;
            }
            if (dz > 0) {
                tdz = recip_tinf((uint32_t)dz);
                tmz = mulq16(0x10000u - oz, tdz);
                sz_stride = VX_WIN_W;
            } else if (dz < 0) {
                tdz = recip_tinf((uint32_t)-dz);
                tmz = mulq16(oz, tdz);
                sz_stride = -VX_WIN_W;
            }

            /* Register-tight DDA: active window keeps >= 44 blocks margin around
             * the player while MAXT = 30 blocks, so X and Z never go out of bounds. */
            for (;;) {
                if (tmx < tmy && tmx < tmz) {
                    t = tmx;
                    if (t > MAXT)
                        goto miss;
                    tmx += tdx;
                    idx += sx;
                    face = 1;
                } else if (tmy < tmz) {
                    t = tmy;
                    if (t > MAXT)
                        goto miss;
                    tmy += tdy;
                    vy += sy_;
                    if ((uint32_t)vy >= cur_vy_lim) {
                        if (sy_ > 0 && t_cloud <= MAXT && (uint32_t)vy < VX_CLOUD_MIN_Y) {
                            /* Fast-forward across the empty sky gap directly to the cloud layer */
                            while (tmx < t_cloud) {
                                tmx += tdx;
                                idx += sx;
                            }
                            while (tmz < t_cloud) {
                                tmz += tdz;
                                idx += sz_stride;
                            }
                            idx += ((int32_t)(VX_CLOUD_MIN_Y + 1u) - vy) * (VX_WIN_W * VX_WIN_D);
                            vy = VX_CLOUD_MIN_Y;
                            t = t_cloud;
                            tmy = t_cloud + tdy;
                            cur_vy_lim = VX_CLOUD_TOP_Y;
                            face = 3;
                        } else {
                            goto miss;
                        }
                    } else {
                        idx += sy_stride;
                        face = face_y;
                    }
                } else {
                    t = tmz;
                    if (t > MAXT)
                        goto miss;
                    tmz += tdz;
                    idx += sz_stride;
                    face = 2;
                }
                b = vx_world[idx];
                if (b) {
                    if ((uint32_t)(b - B_LEAF_PINK) < 4u) {
                        /* Cutout block (leaves / flowers): test alpha inside loop */
                        tt = (int32_t)(t >> 6);
                        if (face == 1) {
                            u = ((ez + (dz * tt >> 8)) >> 12) & 15;
                            v = 15 - (((ey + (dy * tt >> 8)) >> 12) & 15);
                            c = TEX[b][1][v * 16 + u];
                        } else if (face == 2) {
                            u = ((ex + (dx * tt >> 8)) >> 12) & 15;
                            v = 15 - (((ey + (dy * tt >> 8)) >> 12) & 15);
                            c = TEX[b][1][v * 16 + u];
                        } else {
                            u = ((ex + (dx * tt >> 8)) >> 12) & 15;
                            v = ((ez + (dz * tt >> 8)) >> 12) & 15;
                            c = TEX[b][face ? 2 : 0][v * 16 + u];
                        }
                        if (!c)
                            continue;
                        goto shade;
                    }
                    if (skip_water && b == B_WATER)
                        continue;
                    break;
                }
            }

            /* Opaque hit: compute UV and shade outside the inner DDA loop */
            tt = (int32_t)(t >> 6);
            if (face == 1) {
                u = ((ez + (dz * tt >> 8)) >> 12) & 15;
                v = 15 - (((ey + (dy * tt >> 8)) >> 12) & 15);
                c = TEX[b][1][v * 16 + u];
            } else if (face == 2) {
                u = ((ex + (dx * tt >> 8)) >> 12) & 15;
                v = 15 - (((ey + (dy * tt >> 8)) >> 12) & 15);
                c = TEX[b][1][v * 16 + u];
            } else {
                u = ((ex + (dx * tt >> 8)) >> 12) & 15;
                v = ((ez + (dz * tt >> 8)) >> 12) & 15;
                c = TEX[b][face ? 2 : 0][v * 16 + u];
            }
        shade:
            light = face;
            fog = (int32_t)(t >> 17);
            if (is_glow(b)) {
                light = 0;
                fog >>= 1;
            }
            px = LUT[light][fog][c];
            if (idx == target && (u == 0 || u == 15 || v == 0 || v == 15))
                px = 0xFFFFu;
            out[col] = px;
            continue;
        miss:
            out[col] = px_miss;
        }
        if (row_done)
            row_done(row);
    }
}

int32_t vx_pick(const vx_player *p)
{
    const int32_t sy = isin(p->yaw), cy = icos(p->yaw), sp = isin(p->pitch), cp = icos(p->pitch);
    const int32_t d[3] = {sy * cp >> 14, sp, cy * cp >> 14};
    int32_t e[3];
    const int32_t lim[3] = {VX_WIN_W, VX_H, VX_WIN_D}, stride[3] = {1, VX_WIN_W * VX_WIN_D, VX_WIN_W};
    int32_t v[3], st[3], idx, a;
    uint32_t td[3], tm[3];
    vx_sync_chunks(p->x, p->z);
    e[0] = p->x - (vx_win_x0 << 16);
    e[1] = p->y + EYE;
    e[2] = p->z - (vx_win_z0 << 16);
    for (a = 0; a < 3; a++) {
        uint32_t o = (uint32_t)e[a] & 0xFFFFu;
        v[a] = e[a] >> 16;
        st[a] = d[a] > 0 ? 1 : d[a] < 0 ? -1 : 0;
        td[a] = st[a] ? recip_tinf((uint32_t)(d[a] * st[a])) : TINF;
        tm[a] = st[a] > 0 ? mulq16(0x10000u - o, td[a]) : st[a] < 0 ? mulq16(o, td[a]) : TINF;
    }
    if ((uint32_t)v[0] >= VX_WIN_W || (uint32_t)v[1] >= VX_H || (uint32_t)v[2] >= VX_WIN_D)
        return -1;
    idx = VX_IDX(v[0], v[1], v[2]);
    for (;;) {
        uint32_t b;
        a = tm[0] < tm[1] && tm[0] < tm[2] ? 0 : tm[1] < tm[2] ? 1 : 2;
        if (tm[a] > (5u << 16))
            return -1;
        tm[a] += td[a];
        v[a] += st[a];
        if ((uint32_t)v[a] >= (uint32_t)lim[a])
            return -1;
        idx += st[a] * stride[a];
        b = vx_world[idx];
        if (b != B_AIR && b != B_WATER)
            return idx;
    }
}

/* ---- the 4 sequencer stages and proximity scaling */
static const int16_t ST_X0[VX_SEQS] = {120, 172, 120, 68};
static const int16_t ST_Z0[VX_SEQS] = {136, 136, 84,  136};
static int32_t stage_y[VX_SEQS] = {10, 10, 11, 11};
static int32_t lamp_on = -1;
static uint32_t cur_seed = 0x4D5543u;

void vx_stage_pos(int seq, int32_t *x0, int32_t *y0, int32_t *z0)
{
    if ((unsigned)seq >= VX_SEQS)
        seq = 0;
    if (x0) *x0 = ST_X0[seq];
    if (y0) *y0 = stage_y[seq];
    if (z0) *z0 = ST_Z0[seq];
}

static uint32_t isqrt32(uint32_t n)
{
    uint32_t x = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= x + bit) {
            n -= x + bit;
            x = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
        bit >>= 2;
    }
    return x;
}

/* Distance in Q8 blocks (256 = 1.0 block) from the player to the nearest part
 * of sequencer stage `seq` (x0..x0+18, stage_y..stage_y+3, z0..z0+6). */
uint32_t vx_seq_dist_q8(const vx_player *p, int seq)
{
    int32_t px, py, pz, x0, x1, y0, y1, z0, z1, dx, dy, dz;
    if ((unsigned)seq >= VX_SEQS)
        return 64000u;
    px = p->x >> 8;
    py = (p->y + (EYE >> 1)) >> 8;
    pz = p->z >> 8;
    x0 = (int32_t)ST_X0[seq] << 8;
    x1 = ((int32_t)ST_X0[seq] + 18) << 8;
    y0 = stage_y[seq] << 8;
    y1 = (stage_y[seq] + 3) << 8;
    z0 = (int32_t)ST_Z0[seq] << 8;
    z1 = ((int32_t)ST_Z0[seq] + 6) << 8;
    dx = px < x0 ? x0 - px : px > x1 ? px - x1 : 0;
    dy = py < y0 ? y0 - py : py > y1 ? py - y1 : 0;
    dz = pz < z0 ? z0 - pz : pz > z1 ? pz - z1 : 0;
    if (dx > 32767 || dy > 32767 || dz > 32767)
        return 32767u;
    return isqrt32((uint32_t)(dx * dx + dy * dy + dz * dz));
}

/* Full volume (256) within 10 blocks of any part of a sequencer, attenuating
 * smoothly from 10 to 25 blocks, silent (0) at >= 25 blocks. */
void vx_update_proximity(const vx_player *p)
{
    int s;
    for (s = 0; s < VX_SEQS; s++) {
        uint32_t d = vx_seq_dist_q8(p, s);
        if (d <= (10u << 8)) {
            seq_gain[s] = 256u;
        } else if (d >= (25u << 8)) {
            seq_gain[s] = 0u;
        } else {
            uint32_t rem = (25u << 8) - d;      /* 1..3839 over the 15-block fade zone */
            uint32_t g = (rem * rem + 57599u) / 57600u;
            seq_gain[s] = (uint16_t)(g > 255u ? 255u : g);
        }
    }
}

/* ---- procedural world & 8x8 chunk streaming */
static uint32_t vnoise(int32_t x, int32_t z, int32_t cell, uint32_t seed)   /* smooth value noise 0..255 */
{
    int32_t gx = x / cell, gz = z / cell;
    uint32_t fx = (uint32_t)(x % cell) * 256u / (uint32_t)cell, fz = (uint32_t)(z % cell) * 256u / (uint32_t)cell;
    uint32_t a = hash3((uint32_t)gx, (uint32_t)gz, seed) & 255u, b = hash3((uint32_t)gx + 1u, (uint32_t)gz, seed) & 255u;
    uint32_t c = hash3((uint32_t)gx, (uint32_t)gz + 1u, seed) & 255u, d = hash3((uint32_t)gx + 1u, (uint32_t)gz + 1u, seed) & 255u;
    uint32_t top, bot;
    fx = fx * fx * (768u - 2u * fx) >> 16;     /* smoothstep, Q8 */
    fz = fz * fz * (768u - 2u * fz) >> 16;
    top = (a * (256u - fx) + b * fx) >> 8;
    bot = (c * (256u - fx) + d * fx) >> 8;
    return (top * (256u - fz) + bot * fz) >> 8;
}

static void put_world(int32_t wx, int32_t y, int32_t wz, uint8_t b)
{
    int32_t lx = wx - vx_win_x0, lz = wz - vx_win_z0;
    if ((uint32_t)lx < VX_WIN_W && (uint32_t)y < VX_H && (uint32_t)lz < VX_WIN_D)
        vx_world[VX_IDX(lx, y, lz)] = b;
}

static int stage_at_margin(int32_t wx, int32_t wz)
{
    int s;
    for (s = 0; s < VX_SEQS; s++)
        if (wx >= ST_X0[s] - 3 && wx <= ST_X0[s] + 20 && wz >= ST_Z0[s] - 7 && wz <= ST_Z0[s] + 8)
            return s;
    return -1;
}

static int on_avenue(int32_t wx, int32_t wz)
{
    return (wz >= 134 && wz <= 141 && wx >= 64 && wx <= 194) ||
           (wx >= 125 && wx <= 132 && wz >= 80 && wz <= 144);
}

static int32_t raw_terrain_h(int32_t wx, int32_t wz, uint32_t seed)
{
    int32_t nx = wx + 256, nz = wz + 256;
    int32_t h = 5 + (int32_t)(vnoise(nx, nz, 32, seed) * 10u >> 8) +
                (int32_t)(vnoise(nx, nz, 12, seed ^ 0x5A5Au) * 5u >> 8) +
                (int32_t)(vnoise(nx, nz, 6, seed ^ 0xA5A5u) * 2u >> 8);
    /* Central island chain + peninsulas keeping all 4 stages & bridges above sea level */
    int32_t dx = wx - 128, dz = wz - 120;
    int32_t r2 = (dx * dx) / 2 + (dz * dz) * 2 / 3;
    if (r2 < 2400)
        h += (2400 - r2) / 800;
    else if (r2 > 5200)
        h -= (r2 - 5200) / 450;
    /* Outer edge drop-off to the endless sea */
    if (wx < 20) h -= (20 - wx) / 2;
    if (wx > VX_W - 21) h -= (wx - (VX_W - 21)) / 2;
    if (wz < 20) h -= (20 - wz) / 2;
    if (wz > VX_D - 21) h -= (wz - (VX_D - 21)) / 2;
    return h < 1 ? 1 : h > 16 ? 16 : h;
}

static int32_t terrain_h(int32_t wx, int32_t wz, uint32_t seed)
{
    int32_t h = raw_terrain_h(wx, wz, seed);
    int s;
    for (s = 0; s < VX_SEQS; s++) {
        int32_t x0 = ST_X0[s] - 2, x1 = ST_X0[s] + 19;
        int32_t z0 = ST_Z0[s] - 6, z1 = ST_Z0[s] + 7;
        int32_t dx = wx < x0 ? x0 - wx : wx > x1 ? wx - x1 : 0;
        int32_t dz = wz < z0 ? z0 - wz : wz > z1 ? wz - z1 : 0;
        int32_t d = dx > dz ? dx : dz;
        if (d == 0)
            return stage_y[s];
        if (d <= 6) {
            int32_t lim = (d + 1) >> 1;
            if (h < stage_y[s] - lim) h = stage_y[s] - lim;
            if (h > stage_y[s] + lim) h = stage_y[s] + lim;
        }
    }
    return h;
}

/* Generate one 8 x 32 x 8 chunk at world chunk (cx, cz) into window slot (sx, sz) */
static void gen_chunk(int32_t cx, int32_t cz, int32_t sx, int32_t sz)
{
    const int32_t wx0 = cx * VX_CW, wz0 = cz * VX_CD;
    const int32_t lx0 = sx * VX_CW, lz0 = sz * VX_CD;
    uint8_t ch_h[VX_CD][VX_CW];
    int32_t x, y, z, s, i;

    vx_chunks_loaded++;

    /* Outside the 256 x 256 world: endless sea */
    if ((uint32_t)cx >= (VX_W / VX_CW) || (uint32_t)cz >= (VX_D / VX_CD)) {
        for (y = 0; y < VX_H; y++) {
            uint8_t b = y == 0 ? B_BEDROCK : y <= VX_SEA ? B_WATER : B_AIR;
            for (z = 0; z < VX_CD; z++) {
                uint8_t *dst = &vx_world[VX_IDX(lx0, y, lz0 + z)];
                for (x = 0; x < VX_CW; x++)
                    dst[x] = b;
            }
        }
        return;
    }

    /* 1. Terrain columns */
    for (z = 0; z < VX_CD; z++) {
        int32_t wz = wz0 + z;
        for (x = 0; x < VX_CW; x++) {
            int32_t wx = wx0 + x;
            int32_t h = terrain_h(wx, wz, cur_seed);
            ch_h[z][x] = (uint8_t)h;
            for (y = 0; y < VX_H; y++) {
                uint8_t b = B_AIR;
                if (y == 0)
                    b = B_BEDROCK;
                else if (y < h - 3)
                    b = B_STONE;
                else if (y < h)
                    b = B_DIRT;
                else if (y == h)
                    b = h <= VX_SEA + 1 ? B_SAND : B_GRASS;
                else if (y <= VX_SEA)
                    b = B_WATER;
                vx_world[VX_IDX(lx0 + x, y, lz0 + z)] = b;
            }
        }
    }

    /* 2. Trees: up to 2 per 8x8 chunk with trunk in [2..5]x[2..5] so canopy [-2..2] stays inside chunk */
    for (i = 0; i < 2; i++) {
        uint32_t thash = hash3((uint32_t)(cx * 2 + i), (uint32_t)cz, cur_seed ^ 0x72EEu);
        int32_t tx, tz, wx, wz, h, th, pink, a, b, c;
        if ((thash & 255u) > 155u)
            continue;
        tx = 2 + i * 2 + (int32_t)((thash >> 8) & 1u);
        tz = 2 + (int32_t)((thash >> 10) & 3u);
        wx = wx0 + tx;
        wz = wz0 + tz;
        h = ch_h[tz][tx];
        if (h <= VX_SEA + 1 || h > 14 || stage_at_margin(wx, wz) >= 0 || on_avenue(wx, wz))
            continue;
        th = 3 + (int32_t)((thash >> 12) & 1u);
        pink = ((thash >> 16) & 255u) < 150u;
        for (y = 1; y <= th; y++)
            vx_world[VX_IDX(lx0 + tx, h + y, lz0 + tz)] = B_LOG;
        for (b = -2; b <= 2; b++)
            for (c = -2; c <= 2; c++)
                for (a = -2; a <= 2; a++) {
                    int32_t r = a * a + c * c + b * b * 2;
                    uint32_t lh = hash3(thash, (uint32_t)(a + 4), (uint32_t)((b + 4) * 8 + c + 4));
                    if (r > 6 || (r > 3 && (lh & 3u) == 0u))
                        continue;
                    if (vx_world[VX_IDX(lx0 + tx + a, h + th + 1 + b, lz0 + tz + c)] == B_AIR)
                        vx_world[VX_IDX(lx0 + tx + a, h + th + 1 + b, lz0 + tz + c)] =
                            pink ? B_LEAF_PINK : B_LEAF_GREEN;
                }
    }

    /* 3. Flowers & clouds */
    for (z = 0; z < VX_CD; z++) {
        int32_t wz = wz0 + z;
        for (x = 0; x < VX_CW; x++) {
            int32_t wx = wx0 + x, h = ch_h[z][x];
            uint32_t fh = hash3((uint32_t)wx, (uint32_t)wz, cur_seed ^ 0xF10Eu);
            uint32_t cn = vnoise(wx + 200, wz * 2 + 300, 7, cur_seed ^ 0xC10Du);
            if (h > VX_SEA + 1 && stage_at_margin(wx, wz) < 0 && (fh & 255u) < 12u &&
                vx_world[VX_IDX(lx0 + x, h + 1, lz0 + z)] == B_AIR)
                vx_world[VX_IDX(lx0 + x, h + 1, lz0 + z)] = (fh & 256u) ? B_FLOWER_RED : B_FLOWER_YEL;
            if (cn > 168u)
                vx_world[VX_IDX(lx0 + x, 27, lz0 + z)] = B_CLOUD;
            if (cn > 196u)
                vx_world[VX_IDX(lx0 + x, 28, lz0 + z)] = B_CLOUD;
        }
    }

    /* 4. Sequencer stages overlapping this chunk */
    for (s = 0; s < VX_SEQS; s++) {
        int32_t sx0 = ST_X0[s], sz0 = ST_Z0[s], sh = stage_y[s];
        if (wx0 + VX_CW - 1 < sx0 || wx0 > sx0 + 17 || wz0 + VX_CD - 1 < sz0 || wz0 > sz0 + 5)
            continue;
        for (z = 0; z < VX_CD; z++) {
            int32_t wz = wz0 + z;
            if (wz < sz0 || wz > sz0 + 5)
                continue;
            for (x = 0; x < VX_CW; x++) {
                int32_t wx = wx0 + x;
                int border;
                if (wx < sx0 || wx > sx0 + 17)
                    continue;
                border = wx == sx0 || wx == sx0 + 17 || wz == sz0;
                for (y = sh + 1; y < sh + 5; y++)
                    vx_world[VX_IDX(lx0 + x, y, lz0 + z)] = B_AIR;
                if (wz == sz0 + 5) {
                    int step = wx - sx0 - 1;
                    vx_world[VX_IDX(lx0 + x, sh, lz0 + z)] = B_BEDROCK;
                    vx_world[VX_IDX(lx0 + x, sh + 1, lz0 + z)] =
                        border ? B_OBSIDIAN : (step == lamp_on ? B_LAMP_ON : B_LAMP_OFF);
                    vx_world[VX_IDX(lx0 + x, sh + 2, lz0 + z)] = B_OBSIDIAN;
                } else {
                    if (border) {
                        vx_world[VX_IDX(lx0 + x, sh, lz0 + z)] = B_BEDROCK;
                        if (wz == sz0 && (wx == sx0 || wx == sx0 + 17))
                            vx_world[VX_IDX(lx0 + x, sh + 1, lz0 + z)] = B_OBSIDIAN;
                    } else {
                        int tr = wz - sz0 - 1, st = wx - sx0 - 1;
                        vx_world[VX_IDX(lx0 + x, sh, lz0 + z)] =
                            seq_pat_all[s][tr][st] ? (uint8_t)(B_PAD_ON0 + tr) : B_PAD_OFF;
                    }
                }
            }
        }
    }
}

/* Keep the 12x12 chunk window (96x96 blocks) centered on the player's Q16 position,
 * streaming chunks in and out when the player crosses an 8-block chunk boundary. */
void vx_sync_chunks(int32_t px, int32_t pz)
{
    int32_t bx = px >> 16, bz = pz >> 16;
    int32_t qx = (bx - 4) >> 3, qz = (bz - 4) >> 3;
    int32_t nx0 = (qx - 5) * VX_CW, nz0 = (qz - 5) * VX_CD;
    int32_t dxc, dzc, cx0, cz0, sx, sz, y, z, x;

    if (nx0 == vx_win_x0 && nz0 == vx_win_z0)
        return;

    dxc = (nx0 - vx_win_x0) / VX_CW;
    dzc = (nz0 - vx_win_z0) / VX_CD;
    cx0 = nx0 / VX_CW;
    cz0 = nz0 / VX_CD;

    /* Full reload on initial spawn or long teleport */
    if (vx_win_x0 < -100000 || dxc <= -VX_WIN_CW || dxc >= VX_WIN_CW || dzc <= -VX_WIN_CD || dzc >= VX_WIN_CD) {
        if (vx_win_x0 > -100000)
            vx_chunks_unloaded += VX_WIN_CW * VX_WIN_CD;
        vx_win_x0 = nx0;
        vx_win_z0 = nz0;
        for (sz = 0; sz < VX_WIN_CD; sz++)
            for (sx = 0; sx < VX_WIN_CW; sx++)
                gen_chunk(cx0 + sx, cz0 + sz, sx, sz);
        return;
    }

    /* Shift surviving blocks in-place and count unloaded chunks */
    {
        int32_t keep_cw = VX_WIN_CW - (dxc < 0 ? -dxc : dxc);
        int32_t keep_cd = VX_WIN_CD - (dzc < 0 ? -dzc : dzc);
        int32_t sh_x = dxc * VX_CW, sh_z = dzc * VX_CD;
        vx_chunks_unloaded += (uint32_t)(VX_WIN_CW * VX_WIN_CD - keep_cw * keep_cd);

        for (y = 0; y < VX_H; y++) {
            uint8_t *plane = &vx_world[y * (VX_WIN_W * VX_WIN_D)];
            int32_t z_start = sh_z >= 0 ? 0 : VX_WIN_D - 1;
            int32_t z_end   = sh_z >= 0 ? VX_WIN_D : -1;
            int32_t z_step  = sh_z >= 0 ? 1 : -1;
            for (z = z_start; z != z_end; z += z_step) {
                int32_t src_z = z + sh_z;
                if ((uint32_t)src_z >= VX_WIN_D)
                    continue;
                {
                    uint8_t *dst_row = plane + z * VX_WIN_W;
                    const uint8_t *src_row = plane + src_z * VX_WIN_W;
                    if (sh_x >= 0) {
                        for (x = 0; x < VX_WIN_W - sh_x; x++)
                            dst_row[x] = src_row[x + sh_x];
                    } else {
                        for (x = VX_WIN_W - 1; x >= -sh_x; x--)
                            dst_row[x] = src_row[x + sh_x];
                    }
                }
            }
        }
    }

    vx_win_x0 = nx0;
    vx_win_z0 = nz0;

    /* Generate only the newly entered 8x8 chunks */
    for (sz = 0; sz < VX_WIN_CD; sz++) {
        int32_t old_sz = sz + dzc;
        for (sx = 0; sx < VX_WIN_CW; sx++) {
            int32_t old_sx = sx + dxc;
            if ((uint32_t)old_sx >= VX_WIN_CW || (uint32_t)old_sz >= VX_WIN_CD)
                gen_chunk(cx0 + sx, cz0 + sz, sx, sz);
        }
    }
}

/* ---- the player */
#define PR 19661                        /* half width 0.3 */
#define PH 114688                       /* height 1.75 */
#define GRAV 1573                       /* 24 blocks/s^2, per ms */
#define JUMP 498074                     /* 7.6 blocks/s: 1.2 blocks up */
#define VTERM 1310720                   /* 20 blocks/s */
static int32_t spawn_x, spawn_y, spawn_z;

static int solid_at(int32_t wx, int32_t y, int32_t wz)
{
    int32_t lx = wx - vx_win_x0, lz = wz - vx_win_z0;
    uint32_t b;
    if ((uint32_t)wx >= VX_W || (uint32_t)wz >= VX_D || y < 0)
        return 1;
    if (y >= VX_H || (uint32_t)lx >= VX_WIN_W || (uint32_t)lz >= VX_WIN_D)
        return 0;
    b = vx_world[VX_IDX(lx, y, lz)];
    return b != B_AIR && b != B_WATER && b != B_FLOWER_RED && b != B_FLOWER_YEL;
}

static int box_hits(int32_t x, int32_t y, int32_t z)
{
    int32_t x0 = (x - PR) >> 16, x1 = (x + PR - 1) >> 16, y0 = y >> 16, y1 = (y + PH - 1) >> 16;
    int32_t z0 = (z - PR) >> 16, z1 = (z + PR - 1) >> 16, a, b, c;
    for (b = y0; b <= y1; b++)
        for (c = z0; c <= z1; c++)
            for (a = x0; a <= x1; a++)
                if (solid_at(a, b, c))
                    return 1;
    return 0;
}

static void move_h(vx_player *p, int32_t dx, int32_t dz)
{
    int32_t nx = p->x + dx, nz = p->z + dz, up;
    if (!dx && !dz)
        return;
    if (!box_hits(nx, p->y, nz)) {
        p->x = nx;
        p->z = nz;
        return;
    }
    up = ((p->y >> 16) + 1) << 16;
    if (p->on_ground && !box_hits(p->x, up, p->z) && !box_hits(nx, up, nz)) {
        p->y = up;
        p->x = nx;
        p->z = nz;
    }
}

static int water_at(int32_t x, int32_t y, int32_t z)
{
    int32_t lx = (x >> 16) - vx_win_x0, wy = y >> 16, lz = (z >> 16) - vx_win_z0;
    return (uint32_t)lx < VX_WIN_W && (uint32_t)wy < VX_H && (uint32_t)lz < VX_WIN_D &&
           vx_world[VX_IDX(lx, wy, lz)] == B_WATER;
}

static void step(vx_player *p, const vx_input *in, int32_t dt)
{
    const int32_t s = isin(p->yaw), c = icos(p->yaw);
    const int32_t mf = (int32_t)in->fwd - in->back, mr = (int32_t)in->right - in->left;
    int32_t wx = s * mf + c * mr, wz = c * mf - s * mr, speed = p->in_water ? 170 : 282, dy, ny;
    if (mf && mr) {
        wx = wx * 181 >> 8;
        wz = wz * 181 >> 8;
    }
    move_h(p, speed * dt * wx >> 14, 0);
    move_h(p, 0, speed * dt * wz >> 14);
    vx_sync_chunks(p->x, p->z);
    if (p->in_water) {
        p->vy -= 393 * dt;
        if (p->vy < -163840)
            p->vy = -163840;
        if (in->jump)
            p->vy = 229376;
    } else {
        p->vy -= GRAV * dt;
        if (p->vy < -VTERM)
            p->vy = -VTERM;
        if (in->jump && p->on_ground)
            p->vy = JUMP;
    }
    dy = p->vy * dt / 1000;
    ny = p->y + dy;
    if (box_hits(p->x, ny, p->z)) {
        if (dy < 0)
            p->y = ((ny >> 16) + 1) << 16;
        p->vy = 0;
    } else {
        p->y = ny;
    }
    if (p->y > (VX_H - 3) << 16)
        p->y = (VX_H - 3) << 16;
    p->on_ground = (uint8_t)box_hits(p->x, p->y - 64, p->z);
    p->in_water = (uint8_t)water_at(p->x, p->y + 26214, p->z);
    p->eye_in_water = (uint8_t)water_at(p->x, p->y + EYE, p->z);
    if (p->y < 0) {
        p->x = spawn_x;
        p->y = spawn_y;
        p->z = spawn_z;
        p->vy = 0;
        vx_sync_chunks(p->x, p->z);
    }
}

void vx_update(vx_player *p, const vx_input *in, uint32_t dt_ms)
{
    p->yaw = (p->yaw + in->turn) & 1023;
    p->pitch += in->look;
    if (p->pitch > 240)
        p->pitch = 240;
    if (p->pitch < -240)
        p->pitch = -240;
    if (dt_ms > 100u)
        dt_ms = 100u;
    while (dt_ms) {
        uint32_t dt = dt_ms > 16u ? 16u : dt_ms;
        dt_ms -= dt;
        step(p, in, (int32_t)dt);
    }
    vx_update_proximity(p);
}

void vx_generate(uint32_t seed, vx_player *p)
{
    int s;
    cur_seed = seed;
    for (s = 0; s < VX_SEQS; s++) {
        int32_t sh = raw_terrain_h(ST_X0[s] + 8, ST_Z0[s] + 2, seed);
        if (sh < VX_SEA + 2)
            sh = VX_SEA + 2;
        if (sh > 14)
            sh = 14;
        stage_y[s] = sh;
    }
    lamp_on = -1;
    vx_win_x0 = -999999;
    vx_win_z0 = -999999;
    vx_chunks_loaded = 0;
    vx_chunks_unloaded = 0;
    spawn_x = ((ST_X0[0] + 8) << 16) + 32768;
    spawn_z = ((ST_Z0[0] - 5) << 16) + 32768;
    spawn_y = (stage_y[0] + 1) << 16;
    p->x = spawn_x;
    p->y = spawn_y;
    p->z = spawn_z;
    p->vy = 0;
    p->yaw = 0;
    p->pitch = -60;
    p->on_ground = 1;
    p->in_water = p->eye_in_water = 0;
    vx_sync_chunks(p->x, p->z);
    vx_update_proximity(p);
}

void vx_stage_pad_ex(int seq, int track, int step, int on)
{
    if ((unsigned)seq < VX_SEQS && (unsigned)track < VX_TRACKS && (unsigned)step < VX_STEPS)
        put_world(ST_X0[seq] + 1 + step, stage_y[seq], ST_Z0[seq] + 1 + track,
                  on ? (uint8_t)(B_PAD_ON0 + track) : B_PAD_OFF);
}

void vx_stage_pad(int track, int step, int on)
{
    vx_stage_pad_ex(0, track, step, on);
}

void vx_stage_lamp(int step)
{
    int s;
    if (step == lamp_on)
        return;
    for (s = 0; s < VX_SEQS; s++) {
        if (lamp_on >= 0)
            put_world(ST_X0[s] + 1 + lamp_on, stage_y[s] + 1, ST_Z0[s] + 5, B_LAMP_OFF);
        if (step >= 0 && step < VX_STEPS)
            put_world(ST_X0[s] + 1 + step, stage_y[s] + 1, ST_Z0[s] + 5, B_LAMP_ON);
    }
    lamp_on = step;
}

int vx_stage_hit_ex(int32_t idx, int *seq, int *track, int *step)
{
    int32_t plane = VX_WIN_W * VX_WIN_D;
    int32_t y, rem, lz, lx, wx, wz, s;
    if (idx < 0 || idx >= VX_N)
        return 0;
    y = idx / plane;
    rem = idx % plane;
    lz = rem / VX_WIN_W;
    lx = rem % VX_WIN_W;
    wx = vx_win_x0 + lx;
    wz = vx_win_z0 + lz;
    for (s = 0; s < VX_SEQS; s++) {
        if (y == stage_y[s] && wx >= ST_X0[s] + 1 && wx <= ST_X0[s] + VX_STEPS &&
            wz >= ST_Z0[s] + 1 && wz <= ST_Z0[s] + VX_TRACKS) {
            if (seq) *seq = s;
            if (track) *track = wz - ST_Z0[s] - 1;
            if (step) *step = wx - ST_X0[s] - 1;
            return 1;
        }
    }
    return 0;
}

int vx_stage_hit(int32_t idx, int *track, int *step)
{
    return vx_stage_hit_ex(idx, 0, track, step);
}
