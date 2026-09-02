/*
 * Flappy-style game for the Waveshare ESP32-C6-Touch-LCD-1.47.
 *
 * Portrait 172x320 JD9853 panel, AXS5106L capacitive touch. Tap anywhere on
 * the glass to flap. Three states: READY (bird bobbing, "TAP TO FLAP"),
 * PLAY (pipes scroll, score counts), DEAD (bird drops, game-over card,
 * tap to try again). Best score is kept in NVS.
 *
 * Rendering keeps NO framebuffer. The scene is described by a handful of
 * state variables; compose() paints it, and every frame we re-flush only the
 * vertical bands that actually change:
 *
 *   - one band per pipe   (pipe width + a few px of scroll slack)
 *   - the bird's fixed column
 *   - the ground strip     (so it scrolls)
 *   - the score row        (only when the number changes)
 *
 * A full-screen flush is ~22 ms on this bus (~45 fps hard ceiling), but the
 * per-frame dirty area here is ~60k px ≈ 3 ms, which leaves plenty of head
 * room for a steady 60 fps. The loop is paced with esp_timer; the sub-tick
 * remainder is spun off (needs CONFIG_FREERTOS_HZ=1000, set in
 * sdkconfig.defaults).
 *
 * The bird and pipes are drawn from integer primitives (no FPU on this core),
 * so the art is original pixel work rather than lifted sprite sheets.
 *
 * ⚠︎ Built clean on ESP-IDF 6.1.0, but NOT verified on hardware in this
 * session. The display and touch bring-up is the skill template's, which is
 * hardware-proven; the game logic and dirty-band flushing on top are not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "axs5106l.h"
#include "board_pins.h"
#include "esp_lcd_jd9853.h"
#include "font5x7.h"

static const char *TAG = "flappy";

/* The panel takes RGB565 big-endian, the CPU is little-endian: keep every
 * colour pre-swapped so the blitters can stay dumb. */
#define RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR(r, g, b)   ((uint16_t)((RGB565(r, g, b) << 8) | (RGB565(r, g, b) >> 8)))

/* --- palette --------------------------------------------------------- */
#define C_SKY        COLOR(0x4E, 0xC0, 0xE4)
#define C_SKY_LO     COLOR(0x8F, 0xDA, 0xEC)
#define C_HILL       COLOR(0x7A, 0xC7, 0x8E)

#define C_PIPE       COLOR(0x5E, 0xC4, 0x3C)
#define C_PIPE_HI    COLOR(0x9B, 0xE8, 0x66)
#define C_PIPE_SHD   COLOR(0x36, 0x82, 0x22)
#define C_PIPE_DARK  COLOR(0x1F, 0x50, 0x18)

#define C_GRASS      COLOR(0x8A, 0xD6, 0x4E)
#define C_GRASS_D    COLOR(0x53, 0x9B, 0x2E)
#define C_DIRT       COLOR(0xDF, 0xB8, 0x73)
#define C_DIRT_D     COLOR(0xC2, 0x99, 0x54)

#define C_BIRD_OUT   COLOR(0x3A, 0x2A, 0x12)
#define C_BIRD       COLOR(0xF7, 0xD0, 0x3C)
#define C_BIRD_HI    COLOR(0xFF, 0xE8, 0x8A)
#define C_BIRD_BELLY COLOR(0xFC, 0xF2, 0xC6)
#define C_WING       COLOR(0xE7, 0xA9, 0x2B)
#define C_WING_OUT   COLOR(0x9A, 0x6A, 0x16)
#define C_WHITE      COLOR(0xFF, 0xFF, 0xFF)
#define C_BLACK      COLOR(0x22, 0x22, 0x22)
#define C_BEAK       COLOR(0xF4, 0x8A, 0x1E)
#define C_BEAK_D     COLOR(0xC9, 0x5E, 0x12)

#define C_TEXT       COLOR(0xFF, 0xFF, 0xFF)
#define C_TEXT_SHD   COLOR(0x20, 0x38, 0x40)
#define C_CARD       COLOR(0xEA, 0xD8, 0xA6)
#define C_CARD_EDGE  COLOR(0x8A, 0x6A, 0x3A)
#define C_GOLD       COLOR(0xFF, 0xC8, 0x3C)

/* --- geometry ------------------------------------------------------- */
#define W            BSP_LCD_H_RES        /* 172 */
#define H            BSP_LCD_V_RES        /* 320 */

#define GROUND_Y     284                  /* top of the ground strip */

#define BIRD_X       40                   /* bird left edge (fixed) */
#define BIRD_W       24
#define BIRD_H       15

#define PIPE_W       52
#define PIPE_GAP     96                   /* vertical opening */
#define PIPE_LIP_H   13
#define PIPE_LIP_OV  4                    /* lip overhang each side */
#define PIPE_COUNT   3
#define PIPE_SPACING 104                  /* px between consecutive pipes */
#define PIPE_EASE_HOLD  12                /* first N pipes at double spacing   */
#define PIPE_EASE_RAMP  10               /* pipes over which it tightens back */
#define PIPE_STEP_MAX 2                   /* max px a pipe moves per frame */

#define TILE_ROWS    32                   /* staging buffer height */

/* --- fixed-point physics (no FPU) ---------------------------------- */
#define FP           8                    /* 1/256 px sub-pixel */
#define GRAVITY_FP   80                   /* +vy per frame        (~0.31 px/f^2) */
#define FLAP_VY_FP   (-1400)              /* vy on a flap         (~-5.5 px/f)   */
#define MAX_VY_FP    1500                 /* terminal fall speed  (~5.9 px/f)    */
#define PIPE_SPEED_FP 416                 /* scroll speed         (~1.63 px/f ≈ 98 px/s) */

#define FRAME_US     16667LL              /* 60 fps */

#define SCORE_Y      10
#define SCORE_H      30

#define NVS_NS       "flappy"
#define NVS_BEST     "best"

/* ------------------------------------------------------------------ */
/*  Game state                                                         */
/* ------------------------------------------------------------------ */

typedef enum { ST_READY, ST_PLAY, ST_DEAD } state_t;

typedef struct {
    bool active;
    bool scored;
    int  x_fp;      /* left edge, 1/256 px */
    int  gap;       /* gap centre, px */
} pipe_t;

typedef struct {
    state_t state;
    uint32_t tick;          /* frames since boot */
    uint32_t death_tick;
    uint32_t flap_tick;     /* last frame a flap was applied */

    int  bird_y_fp;         /* bird top, 1/256 px */
    int  bird_vy_fp;

    pipe_t pipe[PIPE_COUNT];
    int  pipes_made;        /* how many pipes have been placed this round */
    int  score;
    int  best;
} game_t;

static game_t g;

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_tile;                  /* DMA staging buffer */
static SemaphoreHandle_t s_flush_done;

static int s_shown_score = -1;            /* what the score row currently shows */

/* ------------------------------------------------------------------ */
/*  Clipped 2D blitter over the staging buffer                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int x0, y0;      /* band origin in screen coordinates */
    int w, h;        /* band size */
    uint16_t *buf;
} canvas_t;

static inline void put_pixel(canvas_t *c, int x, int y, uint16_t color)
{
    int lx = x - c->x0;
    int ly = y - c->y0;
    if (lx < 0 || ly < 0 || lx >= c->w || ly >= c->h) {
        return;
    }
    c->buf[ly * c->w + lx] = color;
}

static void fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    int lx0 = x - c->x0;
    int ly0 = y - c->y0;
    int lx1 = lx0 + w;
    int ly1 = ly0 + h;

    if (lx0 < 0) { lx0 = 0; }
    if (ly0 < 0) { ly0 = 0; }
    if (lx1 > c->w) { lx1 = c->w; }
    if (ly1 > c->h) { ly1 = c->h; }

    for (int ly = ly0; ly < ly1; ly++) {
        uint16_t *row = &c->buf[ly * c->w];
        for (int lx = lx0; lx < lx1; lx++) {
            row[lx] = color;
        }
    }
}

/* Solid ellipse. The dx/dy sweep is clamped to the band up front, so calling
 * it for a big shape that only grazes a narrow band costs almost nothing.
 * Integer test: (dx/rx)^2 + (dy/ry)^2 <= 1. */
static void fill_ellipse(canvas_t *c, int cx, int cy, int rx, int ry, uint16_t color)
{
    if (rx <= 0 || ry <= 0) {
        return;
    }
    int dy0 = c->y0 - cy;           if (dy0 < -ry) { dy0 = -ry; }
    int dy1 = c->y0 + c->h - 1 - cy; if (dy1 >  ry) { dy1 =  ry; }
    int dx0 = c->x0 - cx;           if (dx0 < -rx) { dx0 = -rx; }
    int dx1 = c->x0 + c->w - 1 - cx; if (dx1 >  rx) { dx1 =  rx; }
    if (dy0 > dy1 || dx0 > dx1) {
        return;
    }

    int rx2 = rx * rx;
    int ry2 = ry * ry;
    int rr  = rx2 * ry2;
    for (int dy = dy0; dy <= dy1; dy++) {
        int t = dy * dy * rx2;
        if (t > rr) {
            continue;
        }
        uint16_t *row = &c->buf[(cy + dy - c->y0) * c->w];
        for (int dx = dx0; dx <= dx1; dx++) {
            if (dx * dx * ry2 + t <= rr) {
                row[cx + dx - c->x0] = color;
            }
        }
    }
}

static void draw_char(canvas_t *c, int x, int y, char ch, int scale, uint16_t color)
{
    if (x + FONT5X7_WIDTH * scale <= c->x0 || x >= c->x0 + c->w ||
        y + FONT5X7_HEIGHT * scale <= c->y0 || y >= c->y0 + c->h) {
        return;
    }
    if (ch < FONT5X7_FIRST_CHAR || ch > FONT5X7_LAST_CHAR) {
        ch = '?';
    }
    const uint8_t *glyph = &font5x7[(ch - FONT5X7_FIRST_CHAR) * FONT5X7_WIDTH];

    for (int col = 0; col < FONT5X7_WIDTH; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT5X7_HEIGHT; row++) {
            if (!(bits & (1 << row))) {
                continue;
            }
            if (scale == 1) {
                put_pixel(c, x + col, y + row, color);
            } else {
                fill_rect(c, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? (n * (FONT5X7_WIDTH + 1) - 1) * scale : 0;
}

static void draw_text(canvas_t *c, int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++) {
        draw_char(c, x, y, *s, scale, color);
        x += (FONT5X7_WIDTH + 1) * scale;
    }
}

/* Text with a 1-cell drop shadow, centred horizontally. */
static void draw_text_center_shd(canvas_t *c, int y, const char *s, int scale,
                                 uint16_t color)
{
    int x = (W - text_width(s, scale)) / 2;
    draw_text(c, x + scale, y + scale, s, scale, C_TEXT_SHD);
    draw_text(c, x, y, s, scale, color);
}

/* ------------------------------------------------------------------ */
/*  Scene pieces                                                       */
/* ------------------------------------------------------------------ */

static void draw_background(canvas_t *c)
{
    fill_rect(c, 0, 0, W, GROUND_Y, C_SKY);
    fill_rect(c, 0, GROUND_Y - 60, W, 40, C_SKY_LO);   /* soft haze near horizon */
    fill_ellipse(c, 30, GROUND_Y + 6, 60, 34, C_HILL); /* rolling hills */
    fill_ellipse(c, 120, GROUND_Y + 10, 74, 40, C_HILL);
}

/* One vertical pipe segment with a bevel: dark border, body, left highlight,
 * right shadow. */
static void pipe_seg(canvas_t *c, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fill_rect(c, x, y, w, h, C_PIPE_DARK);
    fill_rect(c, x + 1, y + 1, w - 2, h - 2, C_PIPE);
    fill_rect(c, x + 3, y + 1, 3, h - 2, C_PIPE_HI);
    fill_rect(c, x + w - 5, y + 1, 3, h - 2, C_PIPE_SHD);
}

static void draw_pipe(canvas_t *c, int px, int gap)
{
    int gt = gap - PIPE_GAP / 2;        /* bottom edge of the top pipe */
    int gb = gap + PIPE_GAP / 2;        /* top edge of the bottom pipe */

    /* top pipe: body runs off the top of the screen, then the lip */
    pipe_seg(c, px, -4, PIPE_W, gt - PIPE_LIP_H + 4);
    pipe_seg(c, px - PIPE_LIP_OV, gt - PIPE_LIP_H, PIPE_W + 2 * PIPE_LIP_OV, PIPE_LIP_H);

    /* bottom pipe: lip, then body down to the ground */
    pipe_seg(c, px - PIPE_LIP_OV, gb, PIPE_W + 2 * PIPE_LIP_OV, PIPE_LIP_H);
    pipe_seg(c, px, gb + PIPE_LIP_H, PIPE_W, GROUND_Y - (gb + PIPE_LIP_H));
}

static void draw_ground(canvas_t *c, uint32_t scroll)
{
    fill_rect(c, 0, GROUND_Y, W, 4, C_GRASS_D);
    fill_rect(c, 0, GROUND_Y, W, 3, C_GRASS);
    fill_rect(c, 0, GROUND_Y + 4, W, H - GROUND_Y - 4, C_DIRT);

    int off = (int)(scroll % 16);
    for (int x = -off; x < W; x += 16) {
        fill_rect(c, x, GROUND_Y + 9, 9, 5, C_DIRT_D);
        fill_rect(c, x + 4, GROUND_Y + 20, 6, 4, C_DIRT_D);
    }
}

/* Bird, drawn from primitives. wf = wing frame 0..2, tilt shifts the head. */
static void draw_bird(canvas_t *c, int ox, int oy, int wf, int tilt)
{
    int cx = ox + 9;
    int cy = oy + 7;

    /* body */
    fill_ellipse(c, cx, cy, 9, 8, C_BIRD_OUT);
    fill_ellipse(c, cx, cy, 8, 7, C_BIRD);
    fill_ellipse(c, cx - 1, cy - 2, 5, 4, C_BIRD_HI);
    fill_ellipse(c, cx + 1, cy + 3, 6, 4, C_BIRD_BELLY);

    /* wing */
    static const int8_t wing_dy[3] = { 3, 0, -3 };
    static const int8_t wing_ry[3] = { 2, 4, 5 };
    int wy = oy + 8 + wing_dy[wf];
    fill_ellipse(c, ox + 6, wy, 5, wing_ry[wf] + 1, C_WING_OUT);
    fill_ellipse(c, ox + 6, wy, 4, wing_ry[wf], C_WING);

    /* eye */
    int ey = oy + 4 + tilt;
    fill_ellipse(c, ox + 15, ey, 3, 3, C_WHITE);
    fill_ellipse(c, ox + 16, ey, 1, 2, C_BLACK);

    /* beak, protruding to the right */
    int by = oy + 6 + tilt;
    fill_rect(c, ox + 18, by,     6, 3, C_BEAK);
    fill_rect(c, ox + 18, by + 3, 5, 2, C_BEAK_D);
    fill_rect(c, ox + 17, by + 1, 1, 3, C_BEAK);
}

/* ------------------------------------------------------------------ */
/*  compose(): the whole scene, clipped to one band                    */
/* ------------------------------------------------------------------ */

static void compose(canvas_t *c)
{
    draw_background(c);

    if (g.state != ST_READY) {
        for (int i = 0; i < PIPE_COUNT; i++) {
            if (g.pipe[i].active) {
                draw_pipe(c, g.pipe[i].x_fp >> FP, g.pipe[i].gap);
            }
        }
    }

    /* the ground scrolls while playing, sits still otherwise */
    uint32_t gscroll = (g.state == ST_PLAY) ? g.tick * 2 : 0;
    draw_ground(c, gscroll);

    /* bird */
    int by = g.bird_y_fp >> FP;
    int wf;
    int tilt = 0;
    if (g.state == ST_DEAD) {
        wf = 1;
        tilt = 2;
    } else if (g.tick - g.flap_tick < 6) {
        wf = 2;                                   /* just flapped: wings up */
    } else {
        wf = (int)((g.tick >> 2) % 3);
        if (g.state == ST_PLAY && g.bird_vy_fp > 700) {
            tilt = 2;                             /* diving */
        }
    }
    draw_bird(c, BIRD_X, by, wf, tilt);

    /* HUD / overlays, always on top */
    if (g.state == ST_PLAY) {
        char s[8];
        snprintf(s, sizeof(s), "%d", g.score);
        draw_text_center_shd(c, SCORE_Y, s, 3, C_TEXT);
    } else if (g.state == ST_READY) {
        draw_text_center_shd(c, 66, "FLAPPY", 3, C_GOLD);
        draw_text_center_shd(c, 104, "TAP TO FLAP", 1, C_TEXT);
        if (g.best > 0) {
            char s[16];
            snprintf(s, sizeof(s), "BEST %d", g.best);
            draw_text_center_shd(c, 122, s, 1, C_TEXT);
        }
    } else { /* ST_DEAD */
        int cardw = 140, cardh = 92;
        int cx0 = (W - cardw) / 2;
        int cy0 = 96;
        fill_rect(c, cx0 - 2, cy0 - 2, cardw + 4, cardh + 4, C_CARD_EDGE);
        fill_rect(c, cx0, cy0, cardw, cardh, C_CARD);
        draw_text_center_shd(c, cy0 + 10, "GAME OVER", 2, C_TEXT_SHD);

        char s[16];
        snprintf(s, sizeof(s), "SCORE  %d", g.score);
        draw_text(c, cx0 + 14, cy0 + 38, s, 1, C_TEXT_SHD);
        snprintf(s, sizeof(s), "BEST   %d", g.best);
        draw_text(c, cx0 + 14, cy0 + 52, s, 1, C_TEXT_SHD);

        if ((g.tick - g.death_tick) > 40 && ((g.tick >> 3) & 1)) {
            draw_text_center_shd(c, cy0 + 70, "TAP TO RETRY", 1, C_TEXT_SHD);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Flushing                                                           */
/* ------------------------------------------------------------------ */

static void flush_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) { w = W - x; }
    if (y + h > H) { h = H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    int max_rows = (W * TILE_ROWS) / w;
    if (max_rows < 1) {
        max_rows = 1;
    }

    for (int by = y; by < y + h; by += max_rows) {
        int bh = (by + max_rows > y + h) ? (y + h - by) : max_rows;
        canvas_t c = { .x0 = x, .y0 = by, .w = w, .h = bh, .buf = s_tile };
        compose(&c);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, x, by, x + w, by + bh, s_tile));
        /* draw_bitmap only queues the DMA; the next band must not repaint
         * s_tile until the panel has consumed this one. */
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}

static void flush_all(void)
{
    flush_rect(0, 0, W, H);
    s_shown_score = (g.state == ST_PLAY) ? g.score : -1;
}

/* The bands that change every frame. */
static void flush_dynamic(void)
{
    /* pipes */
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (!g.pipe[i].active) {
            continue;
        }
        int px = g.pipe[i].x_fp >> FP;
        flush_rect(px - PIPE_LIP_OV - 1, 0,
                   PIPE_W + 2 * PIPE_LIP_OV + PIPE_STEP_MAX + 2, GROUND_Y);
    }

    /* bird column (fixed x, moves only vertically) */
    flush_rect(BIRD_X - 3, 0, BIRD_W + 6, GROUND_Y);

    /* scrolling ground */
    if (g.state == ST_PLAY || g.state == ST_READY) {
        flush_rect(0, GROUND_Y, W, H - GROUND_Y);
    }

    /* score digits, only when the number changes */
    if (g.state == ST_PLAY && g.score != s_shown_score) {
        flush_rect(0, 0, W, SCORE_Y + SCORE_H);
        s_shown_score = g.score;
    }
}

/* ------------------------------------------------------------------ */
/*  Game logic                                                         */
/* ------------------------------------------------------------------ */

/* Horizontal distance from the previous pipe to pipe number `n` (0-based).
 * The first PIPE_EASE_HOLD pipes sit at twice the normal spacing; over the
 * next PIPE_EASE_RAMP pipes the extra distance is linearly removed, then it
 * settles at the regular PIPE_SPACING. */
static int pipe_spacing(int n)
{
    if (n < PIPE_EASE_HOLD) {
        return PIPE_SPACING * 2;
    }
    int r = n - PIPE_EASE_HOLD;
    if (r >= PIPE_EASE_RAMP) {
        return PIPE_SPACING;
    }
    return PIPE_SPACING + PIPE_SPACING * (PIPE_EASE_RAMP - r) / PIPE_EASE_RAMP;
}

static int rand_gap(void)
{
    int lo = PIPE_GAP / 2 + 20;
    int hi = GROUND_Y - PIPE_GAP / 2 - 20;
    return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

static void pipes_spawn(void)
{
    int x = W + 40;
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (i > 0) {
            x += pipe_spacing(i);
        }
        g.pipe[i].active = true;
        g.pipe[i].scored = false;
        g.pipe[i].x_fp = x << FP;
        g.pipe[i].gap = rand_gap();
    }
    g.pipes_made = PIPE_COUNT;
}

static void game_reset(void)
{
    g.state = ST_READY;
    g.bird_y_fp = 132 << FP;
    g.bird_vy_fp = 0;
    g.score = 0;
    for (int i = 0; i < PIPE_COUNT; i++) {
        g.pipe[i].active = false;
    }
}

static void best_load(void)
{
    nvs_handle_t h;
    g.best = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_BEST, &v) == ESP_OK) {
            g.best = v;
        }
        nvs_close(h);
    }
}

static void best_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_BEST, g.best);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void bird_flap(void)
{
    g.bird_vy_fp = FLAP_VY_FP;
    g.flap_tick = g.tick;
}

static void die(void)
{
    g.state = ST_DEAD;
    g.death_tick = g.tick;
    if (g.score > g.best) {
        g.best = g.score;
        best_save();
    }
    flush_all();
}

static void on_tap(void)
{
    switch (g.state) {
    case ST_READY:
        g.state = ST_PLAY;
        pipes_spawn();
        bird_flap();
        flush_all();
        break;
    case ST_PLAY:
        bird_flap();
        break;
    case ST_DEAD:
        if (g.tick - g.death_tick > 40) {   /* ~0.66 s lock-out */
            game_reset();
            flush_all();
        }
        break;
    }
}

static void game_update(void)
{
    g.tick++;

    if (g.state == ST_READY) {
        /* gentle bob */
        static const int8_t bob[16] = { 0,-1,-2,-3,-3,-3,-2,-1, 0, 1, 2, 3, 3, 3, 2, 1 };
        g.bird_y_fp = (132 + bob[(g.tick >> 2) & 15]) << FP;
        return;
    }

    /* gravity (PLAY and DEAD) */
    g.bird_vy_fp += GRAVITY_FP;
    if (g.bird_vy_fp > MAX_VY_FP) {
        g.bird_vy_fp = MAX_VY_FP;
    }
    g.bird_y_fp += g.bird_vy_fp;

    if (g.bird_y_fp < 0) {
        g.bird_y_fp = 0;
        g.bird_vy_fp = 0;
    }
    if ((g.bird_y_fp >> FP) + BIRD_H >= GROUND_Y) {
        g.bird_y_fp = (GROUND_Y - BIRD_H) << FP;
        g.bird_vy_fp = 0;
        if (g.state == ST_PLAY) {
            die();
        }
        return;
    }

    if (g.state != ST_PLAY) {
        return;
    }

    /* scroll pipes, recycle, score */
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (!g.pipe[i].active) {
            continue;
        }
        g.pipe[i].x_fp -= PIPE_SPEED_FP;
        int px = g.pipe[i].x_fp >> FP;

        if (!g.pipe[i].scored && px + PIPE_W < BIRD_X) {
            g.pipe[i].scored = true;
            g.score++;
        }

        if (px + PIPE_W + PIPE_LIP_OV < 0) {
            int rightmost = 0;
            for (int k = 0; k < PIPE_COUNT; k++) {
                if (g.pipe[k].active && (g.pipe[k].x_fp >> FP) > rightmost) {
                    rightmost = g.pipe[k].x_fp >> FP;
                }
            }
            g.pipe[i].x_fp = (rightmost + pipe_spacing(g.pipes_made)) << FP;
            g.pipe[i].gap = rand_gap();
            g.pipe[i].scored = false;
            g.pipes_made++;
        }
    }

    /* collision — slightly forgiving hitbox */
    int bx0 = BIRD_X + 3;
    int bx1 = BIRD_X + BIRD_W - 6;
    int by0 = (g.bird_y_fp >> FP) + 3;
    int by1 = (g.bird_y_fp >> FP) + BIRD_H - 3;

    for (int i = 0; i < PIPE_COUNT; i++) {
        if (!g.pipe[i].active) {
            continue;
        }
        int px = g.pipe[i].x_fp >> FP;
        if (bx1 < px || bx0 > px + PIPE_W) {
            continue;
        }
        int gt = g.pipe[i].gap - PIPE_GAP / 2;
        int gb = g.pipe[i].gap + PIPE_GAP / 2;
        if (by0 < gt || by1 > gb) {
            die();
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Hardware bring-up (from the skill template)                        */
/* ------------------------------------------------------------------ */

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static void backlight_set(uint8_t percent)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 255 / 100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

static void display_init(void)
{
    /* TF card shares SCLK/MOSI with the panel — park its CS high. */
    gpio_config_t sd_cs = {
        .pin_bit_mask = 1ULL << BSP_SD_CS,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_cs));
    gpio_set_level(BSP_SD_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = BSP_LCD_SCLK,
        .mosi_io_num = BSP_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W * TILE_ROWS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    s_flush_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_flush_done ? ESP_OK : ESP_ERR_NO_MEM);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BSP_LCD_PIXEL_CLK,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                             &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, JD9853_LCD_X_GAP, JD9853_LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_tile = heap_caps_malloc(W * TILE_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(s_tile ? ESP_OK : ESP_ERR_NO_MEM);
}

static i2c_master_bus_handle_t i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* ------------------------------------------------------------------ */
/*  Application                                                        */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Flappy on ESP32-C6-Touch-LCD-1.47");

    backlight_init();          /* 0 % duty first — see the template */
    nvs_init();
    display_init();

    memset(&g, 0, sizeof(g));
    best_load();
    game_reset();

    flush_all();
    backlight_set(90);

    i2c_master_bus_handle_t i2c_bus = i2c_init();

    axs5106l_config_t tp_cfg = {
        .i2c_bus = i2c_bus,
        .rst_gpio = BSP_TP_RST,
        .int_gpio = BSP_TP_INT,
        .width = W,
        .height = H,
    };
    axs5106l_handle_t tp = NULL;
    esp_err_t err = axs5106l_new(&tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        vTaskDelay(portMAX_DELAY);
    }
    ESP_LOGI(TAG, "ready — tap to flap");

    bool was_pressed = false;
    int64_t next_us = esp_timer_get_time();

    for (;;) {
        /* ---- input: rising edge of a touch = one flap ---- */
        axs5106l_data_t t;
        bool pressed = (axs5106l_read(tp, &t) == ESP_OK) && t.pressed;
        if (pressed && !was_pressed) {
            on_tap();
        }
        was_pressed = pressed;

        /* ---- simulate + render ---- */
        game_update();
        if (g.state != ST_DEAD || (g.tick - g.death_tick) <= 60) {
            flush_dynamic();
        } else {
            /* bird has landed and the card is up — nothing moves, idle cheap */
            flush_rect(BIRD_X - 3, 0, BIRD_W + 6, GROUND_Y);
        }

        /* ---- pace to 60 fps ---- */
        next_us += FRAME_US;
        int64_t now = esp_timer_get_time();
        int64_t slack = next_us - now;
        if (slack < -FRAME_US * 3) {
            next_us = now;                 /* fell far behind, resync */
        } else if (slack > 0) {
            int ms = (int)(slack / 1000);
            if (ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(ms));
            }
            while (esp_timer_get_time() < next_us) {
                /* spin the sub-millisecond remainder */
            }
        }
    }
}
