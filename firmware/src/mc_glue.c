/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 MusiCraft: the boot flow, the controls, and the 120 x 120 picture shown
 * 2x on the 240 x 240 LCD. */
#include <stdint.h>
#include "fm1_sys.h"
#include "fm1_input.h"
#include "fm1_audio.h"

/* The controls are note names, any octave, on the FM-1's keys and on MIDI (USB or TRS), laid out for
 * Ableton's computer keyboard: W A S D = C# C D E (move), F G (turn), E T = D# F# (look), H = A (jump),
 * J = B (toggle the pad you look at), U = A# (play / stop). */
enum { FM1_BTN_PLAY = 12, FM1_BTN_HOME = 8 };
enum { PC_C, PC_CS, PC_D, PC_DS, PC_E, PC_F, PC_FS, PC_G, PC_GS, PC_A, PC_AS, PC_B };
enum { ENC_SELECT = 0, ENC_ALGO = 1, ENC_K1 = 2 };

static vx_player me;
static uint32_t world_seed = 0x4D5543u;
volatile uint32_t mc_frames;                    /* frames drawn (read by the emulator: --dump) */

/* FM-1 key mask (bit n = key n, key 0 = F3) -> pitch-class mask (bit 0 = C) */
static uint32_t keys_pc(uint32_t n)
{
    uint32_t pc = 0, c = 5u;
    for (; n; n >>= 1) {
        if (n & 1u)
            pc |= 1u << c;
        if (++c == 12u)
            c = 0;
    }
    return pc;
}

#define HELD(pc, k) (((pc) >> (k)) & 1u)

static void read_controls(vx_input *in, uint32_t *press, uint32_t dt)
{
    uint32_t pc = keys_pc(fm1_in.notes) | midi_pc, rel, btn;
    int32_t turn = (int32_t)(HELD(pc, PC_G) - HELD(pc, PC_F)), look = (int32_t)(HELD(pc, PC_DS) - HELD(pc, PC_FS));
    in->fwd = (uint8_t)HELD(pc, PC_CS);
    in->back = (uint8_t)HELD(pc, PC_D);
    in->left = (uint8_t)HELD(pc, PC_C);
    in->right = (uint8_t)HELD(pc, PC_E);
    in->jump = (uint8_t)HELD(pc, PC_A);
    in->turn = turn * (int32_t)(400u * dt / 1000u) + fm1_enc_take(ENC_SELECT) * 16;
    in->look = look * (int32_t)(260u * dt / 1000u) + fm1_enc_take(ENC_ALGO) * 12;
    *press = keys_pc(fm1_input_note_edges()) | midi_note_edges();
    btn = fm1_input_edges(&rel);
    if (btn & (1u << FM1_BTN_PLAY))
        *press |= 1u << PC_AS;
    if (btn & (1u << FM1_BTN_HOME))
        *press |= 1u << 31;
}

/* ---- the picture: rendered into one of two 120 x 120 frames while the other
 * goes out to the LCD by DMA, two rows (four LCD rows) at a time, pumped after
 * every rendered row */
static uint16_t fb[2][VX_RW * VX_RH] __attribute__((aligned(4)));
static uint32_t ls_buf[120u * 4u];
static struct {
    const uint16_t *src;
    uint32_t y;
    uint8_t active;
} ls;
static int8_t fb_pending = -1;                  /* a rendered frame waiting for the LCD */
static uint8_t fb_sent;                         /* the frame streaming (or last streamed) */
static uint8_t lcd_div = 1;                     /* SPI1 = 60 MHz / (lcd_div + 1) */

static void ls_begin(void)
{
    fb_sent = (uint8_t)fb_pending;
    fb_pending = -1;
    ls.src = fb[fb_sent];
    ls.y = 0;
    ls.active = 1;
    lcd_sync();
    fm1_lcd_baud(lcd_div);
    lcd_window(0, 0, 239, 239);
}

static void lcd_pump(void)
{
    uint32_t i, r;
    if (!ls.active) {
        if (fb_pending < 0)
            return;
        ls_begin();
    }
    if (!lcd_idle())
        return;
    if (ls.y >= VX_RH) {
        lcd_sync();
        ls.active = 0;
        if (fb_pending < 0)
            return;
        ls_begin();
    }
    for (r = 0; r < 2u; r++) {
        const uint16_t *s = ls.src + (ls.y + r) * VX_RW;
        uint32_t *d0 = ls_buf + r * 240u, *d1 = d0 + 120u;
        for (i = 0; i < VX_RW; i++) {
            uint32_t p = s[i];
            p |= p << 16;
            d0[i] = p;
            d1[i] = p;
        }
    }
    lcd_data(ls_buf, sizeof ls_buf);
    ls.y += 2u;
}

static void row_hook(int y)
{
    (void)y;
    lcd_pump();
}

static void hud(uint16_t *f)
{
    uint32_t i;
    if (me.eye_in_water)                        /* under water: everything half sea blue */
        for (i = 0; i < VX_RW * VX_RH; i++) {
            uint32_t c = (uint32_t)((f[i] >> 8) | (f[i] << 8)) & 0xFFFFu;
            c = ((c & 0xF7DEu) >> 1) + (0x3B3Eu >> 1);
            f[i] = (uint16_t)((c >> 8) | (c << 8));
        }
    for (i = 0; i < 5u; i++) {                  /* the crosshair */
        f[(VX_RH / 2u) * VX_RW + VX_RW / 2u - 2u + i] = 0xFFFFu;
        f[(VX_RH / 2u - 2u + i) * VX_RW + VX_RW / 2u] = 0xFFFFu;
    }
}

static void stage_from_pattern(void)
{
    int q, t, s;
    for (q = 0; q < VX_SEQS; q++)
        for (t = 0; t < VX_TRACKS; t++)
            for (s = 0; s < VX_STEPS; s++)
                vx_stage_pad_ex(q, t, s, seq_pat_all[q][t][s]);
}

static void new_world(void)
{
    vx_generate(world_seed, &me);
    stage_from_pattern();
}

static void fail_screen(const char *msg)
{
    lcd_fill(0, 0, 240, 240, RGB(80, 0, 0));
    draw_text_box(0, 100, 240, &FONT_S, msg, C_WHITE, 1);
    for (;;)
        fm1_service();
}

static void hex8(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[40];
    uint32_t t0;
    lcd_sync();
    lcd_fill(0, 0, 240, 240, RGB(120, 0, 0));
    draw_text_box(0, 20, 240, &FONT_S, "CRASH", C_WHITE, 1);
    hex8(b, c->vec);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    hex8(b, c->pc);
    draw_text_box(10, 80, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 15000u * 1000u * FM1_TICKS_PER_US)
        fm1_wdt_feed();
    fm1_reboot();
}

static void wait_ms(uint32_t ms)
{
    uint32_t t0 = fm1_ms;
    while ((uint32_t)(fm1_ms - t0) < ms)
        fm1_service();
}

static void boot_splash(void)
{
    fm1_audio_stop();                           /* silent until the island is up (seq_init) */
    lcd_fill(0, 0, 240, 240, RGB(150, 200, 255));
    lcd_fill(0, 150, 240, 90, RGB(126, 214, 98));
    draw_text_box(0, 92, 240, &FONT_S, "FM-1 MUSICRAFT", C_WHITE, 1);
    wait_ms(700u);
    bootguard_clear(&bootguard);
}

static void controls_screen(void)
{
    static const char *const L[][2] = {
        {"C# / D", "FORWARD / BACK"}, {"C / E", "LEFT / RIGHT"}, {"F / G", "TURN"},
        {"D# / F#", "LOOK UP / DOWN"}, {"A", "JUMP"}, {"B", "TOGGLE PAD"},
        {"A# / PLAY", "PLAY / STOP"}, {"SELECT KNOB", "TURN"}, {"ALGO KNOB", "LOOK"},
        {"KNOB 1", "TEMPO"}, {"HOME", "NEW ISLAND"},
    };
    uint32_t i, t0, rel;
    lcd_fill(0, 0, 240, 240, RGB(24, 32, 56));
    draw_text_box(0, 3, 240, &FONT_S, "FM-1 MUSICRAFT", RGB(255, 176, 206), 1);
    for (i = 0; i < sizeof L / sizeof L[0]; i++) {
        draw_text_box(4, 23 + i * 18, 100, &FONT_S, L[i][0], RGB(255, 222, 84), 0);
        draw_text_box(106, 23 + i * 18, 132, &FONT_S, L[i][1], C_WHITE, 0);
    }
    draw_text_box(0, 224, 240, &FONT_S, "PRESS ANY KEY", RGB(150, 160, 190), 1);
    fm1_input_edges(&rel);
    fm1_input_note_edges();
    midi_note_edges();
    while (fm1_in.notes || fm1_in.buttons || midi_pc)
        fm1_service();
    t0 = fm1_ms;
    for (;;) {
        fm1_service();
        if (fm1_input_note_edges() || fm1_input_edges(&rel) || midi_note_edges())
            break;
        if ((uint32_t)(fm1_ms - t0) > 120000u)
            break;
    }
    while (fm1_in.notes || fm1_in.buttons || midi_pc)
        fm1_service();
}

static void musicraft_main(void)
{
    uint32_t last;
    boot_splash();
    controls_screen();
    lcd_fill(0, 0, 240, 240, RGB(150, 200, 255));
    draw_text_box(0, 112, 240, &FONT_S, "GROWING AN ISLAND...", C_WHITE, 1);
    vx_init();
    seq_init();
    new_world();
    fm1_audio_start();
    last = fm1_ms;
    for (;;) {
        vx_input in;
        uint32_t press, now, dt;
        int32_t target;
        int seq, track, step, draw;
        while (fb_pending >= 0) {               /* both frames busy: let the LCD catch up */
            lcd_pump();
            fm1_service();
        }
        draw = 1 - fb_sent;
        now = fm1_ms;
        dt = now - last;
        last = now;
        read_controls(&in, &press, dt);
        vx_update(&me, &in, dt);
        target = vx_pick(&me);
        if ((press & (1u << PC_B)) && vx_stage_hit_ex(target, &seq, &track, &step)) {
            seq_pat_all[seq][track][step] ^= 1u;
            vx_stage_pad_ex(seq, track, step, seq_pat_all[seq][track][step]);
        }
        if (press & (1u << PC_AS))
            seq_playing ^= 1u;
        if (press & (1u << 31)) {
            world_seed = world_seed * 1103515245u + fm1_ticks();
            new_world();
        }
        {
            int32_t bpm = (int32_t)seq_bpm + fm1_enc_take(ENC_K1) * 2;
            seq_bpm = (uint16_t)(bpm < 60 ? 60 : bpm > 200 ? 200 : bpm);
        }
        vx_stage_lamp(seq_playing ? seq_step : -1);
        vx_render(&me, target, fb[draw], row_hook);
        hud(fb[draw]);
        fb_pending = (int8_t)draw;
        lcd_pump();
        fm1_service();
        mc_frames++;
    }
}
