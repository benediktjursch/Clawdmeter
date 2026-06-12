#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// 20×20 grid. CELL sized so the canvas fits the smaller display dimension —
// the canvas is square and centered, so on portrait or letterboxed panels
// it leaves vertical margin rather than cropping.
#define GRID         20
static int  cell      = 24;        // recomputed in splash_init()
static int  canvas_w  = GRID * 24;
static int  canvas_h  = GRID * 24;
// Auf schmalen Hochformat-Displays das gesamte Display als Canvas verwenden
// und den sichtbaren Inhalt der Animation darin zentrieren.
static bool center_visible_sprite = false;

// Background fallback when palette is missing
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;

// While splash is showing, auto-cycle to the next animation in the current
// rate-driven group every this many ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Usage-rate animation groups: 4 groups × up to 4 animations each.
// Filled at init by matching literal names from splash_anims[].
#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    // Group 0 — idle / sleepy
    { "expression sleep", "idle breathe", "idle blink", "expression wink" },
    // Group 1 — normal pace
    { "idle look around", "work think", "work coding", NULL },
    // Group 2 — active
    { "dance sway", "expression surprise", "dance bounce", NULL },
    // Group 3 — heavy
    { "dance bounce dj", "dance sway dj", "dance djmix", NULL },
};

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

static uint16_t *row_buf = NULL;   // scratch row, sized to canvas_w

static inline uint16_t get_splash_color(
    uint8_t code,
    const uint16_t* palette
) {
    if (palette && code < SPLASH_PALETTE_SIZE) {
        return palette[code];
    }

    return COL_EMPTY;
}

static void render_frame(
    const uint8_t* cells,
    const uint16_t* palette
) {
    if (!canvas_buf || !cells) {
        return;
    }

    // Normales Verhalten für die bestehenden quadratischen Displays.
    if (!center_visible_sprite) {
        if (!row_buf) {
            return;
        }

        for (int gy = 0; gy < GRID; gy++) {
            for (int gx = 0; gx < GRID; gx++) {
                const uint8_t code = cells[gy * GRID + gx];
                const uint16_t color =
                    get_splash_color(code, palette);

                uint16_t* p = &row_buf[gx * cell];

                for (int i = 0; i < cell; i++) {
                    p[i] = color;
                }
            }

            for (int dy = 0; dy < cell; dy++) {
                memcpy(
                    &canvas_buf[
                        (gy * cell + dy) * canvas_w
                    ],
                    row_buf,
                    canvas_w * sizeof(uint16_t)
                );
            }
        }

        if (canvas) {
            lv_obj_invalidate(canvas);
        }

        return;
    }

    // Gesamtes Hochformat-Canvas zuerst schwarz löschen.
    const size_t canvas_pixels =
        static_cast<size_t>(canvas_w) * canvas_h;

    for (size_t i = 0; i < canvas_pixels; i++) {
        canvas_buf[i] = COL_EMPTY;
    }

    // Sichtbaren Bereich über ALLE Frames der aktuellen Animation suchen.
    // Dadurch bleibt die Position während der Animation stabil.
    int min_x = GRID;
    int min_y = GRID;
    int max_x = -1;
    int max_y = -1;

    if (cur_anim < SPLASH_ANIM_COUNT) {
        const splash_anim_def_t& animation =
            splash_anims[cur_anim];

        for (uint16_t frame = 0;
             frame < animation.frame_count;
             frame++) {

            const uint8_t* frame_cells =
                animation.frames[frame];

            if (!frame_cells) {
                continue;
            }

            for (int gy = 0; gy < GRID; gy++) {
                for (int gx = 0; gx < GRID; gx++) {
                    const uint8_t code =
                        frame_cells[gy * GRID + gx];

                    const uint16_t color =
                        get_splash_color(
                            code,
                            animation.palette
                        );

                    if (color == COL_EMPTY) {
                        continue;
                    }

                    if (gx < min_x) min_x = gx;
                    if (gx > max_x) max_x = gx;
                    if (gy < min_y) min_y = gy;
                    if (gy > max_y) max_y = gy;
                }
            }
        }
    }

    // Leerer Frame.
    if (max_x < min_x || max_y < min_y) {
        if (canvas) {
            lv_obj_invalidate(canvas);
        }

        return;
    }

    const int visible_cells_w = max_x - min_x + 1;
    const int visible_cells_h = max_y - min_y + 1;

    const int visible_px_w = visible_cells_w * cell;
    const int visible_px_h = visible_cells_h * cell;

    // Sichtbaren Teil exakt im kompletten Display zentrieren.
    const int origin_x = (canvas_w - visible_px_w) / 2;
    const int origin_y = (canvas_h - visible_px_h) / 2;

    for (int gy = min_y; gy <= max_y; gy++) {
        for (int gx = min_x; gx <= max_x; gx++) {
            const uint8_t code =
                cells[gy * GRID + gx];

            const uint16_t color =
                get_splash_color(code, palette);

            const int pixel_x =
                origin_x + (gx - min_x) * cell;

            const int pixel_y =
                origin_y + (gy - min_y) * cell;

            for (int dy = 0; dy < cell; dy++) {
                uint16_t* destination =
                    &canvas_buf[
                        (pixel_y + dy) * canvas_w +
                        pixel_x
                    ];

                for (int dx = 0; dx < cell; dx++) {
                    destination[dx] = color;
                }
            }
        }
    }

    if (canvas) {
        lv_obj_invalidate(canvas);
    }
}

// ---- Mini creature: a small animated creature for embedding in other screens
//      (e.g. the idle "sleeping" indicator). Self-contained — its own canvas and
//      buffer, independent of the full-screen splash above. ----
static lv_obj_t  *mini_canvas = NULL;
static uint16_t  *mini_buf = NULL;
static int        mini_cell = 0;
static int        mini_w = 0;
static const splash_anim_def_t *mini_anim = NULL;
static uint16_t   mini_frame = 0;
static uint32_t   mini_started = 0;

static void mini_render(void) {
    if (!mini_buf || !mini_anim) return;
    const uint8_t *cells = mini_anim->frames[mini_frame];
    const uint16_t *pal = mini_anim->palette;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (pal && code < SPLASH_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < mini_cell; dy++) {
                uint16_t *dst = &mini_buf[(gy * mini_cell + dy) * mini_w + gx * mini_cell];
                for (int dx = 0; dx < mini_cell; dx++) dst[dx] = color;
            }
        }
    }
    if (mini_canvas) lv_obj_invalidate(mini_canvas);
}

lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px) {
    mini_anim = NULL;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, anim_name) == 0) { mini_anim = &splash_anims[i]; break; }
    }
    if (!mini_anim) return NULL;
    mini_cell = px / GRID;
    if (mini_cell < 1) mini_cell = 1;
    mini_w = GRID * mini_cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    mini_buf = (uint16_t*)heap_caps_malloc(mini_w * mini_w * 2, caps);
    if (!mini_buf) return NULL;
    mini_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(mini_canvas, mini_buf, mini_w, mini_w, LV_COLOR_FORMAT_RGB565);
    mini_frame = 0;
    mini_started = millis();
    mini_render();
    return mini_canvas;
}

void splash_mini_tick(void) {
    if (!mini_buf || !mini_anim || mini_anim->frame_count == 0) return;
    if (millis() - mini_started < mini_anim->holds[mini_frame]) return;
    mini_started = millis();
    mini_frame = (mini_frame + 1) % mini_anim->frame_count;
    mini_render();
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    if (canvas_buf) {
        for (int i = 0; i < canvas_w * canvas_h; i++) canvas_buf[i] = COL_EMPTY;
    }
    if (canvas) lv_obj_invalidate(canvas);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();
    int min_dim = (c.width < c.height) ? c.width : c.height;
    cell     = min_dim / GRID;       // fits within the smaller display dimension
    if (cell < 4) cell = 4;

#ifdef BOARD_HAS_PSRAM
    const uint32_t canvas_caps = MALLOC_CAP_SPIRAM;
#else
    // Without PSRAM the full 480×480 RGB565 canvas (460 KB) won't fit. Cap
    // the canvas so the buffer stays under ~80 KB, leaving the rest of
    // internal SRAM free for LVGL, NimBLE, and the audio/PMU stacks. The
    // canvas is centered, so the cost is extra black border around the
    // pixel art — not cropping.
    const uint32_t canvas_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const int MAX_CELL_NO_PSRAM = 10;  // 10*20=200; 200*200*2=78 KB
    if (cell > MAX_CELL_NO_PSRAM) cell = MAX_CELL_NO_PSRAM;
#endif

    #ifdef BOARD_HAS_PSRAM
        // Das 280x456-Board hat genug PSRAM für ein echtes Vollbild-Canvas.
        center_visible_sprite =
            c.width <= 300 &&
            c.height > c.width;
    #else
        center_visible_sprite = false;
    #endif

        if (center_visible_sprite) {
            canvas_w = c.width;
            canvas_h = c.height;
        } else {
            canvas_w = GRID * cell;
            canvas_h = GRID * cell;
        }

    canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, canvas_caps);
    row_buf    = (uint16_t*)heap_caps_malloc(canvas_w * 2,             canvas_caps);
    if (!canvas_buf || !row_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, c.width, c.height);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/scrape_claudepix.js\n"
        "then tools/convert_to_c.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;

    // Auto-rotate to the next animation in the current group.
    if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        splash_pick_for_current_rate();
    }

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void splash_next(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}
