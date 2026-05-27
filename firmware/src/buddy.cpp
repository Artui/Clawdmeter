#include "buddy.h"
#include "buddy_art.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);

// Block glyphs for stat bars (present in the regenerated Mono font).
#define BAR_FULL  "█"  // █
#define BAR_EMPTY "░"  // ░
#define STAR      "★"  // ★
#define STAT_BARS 10        // bar width in cells

// Rarity colors, ported from upstream claude-buddy (server/theme.ts dark
// theme) — common gray brightened from 0x999999 so it isn't muddy on a black
// AMOLED. Indexed by BuddyState.rarity (0=common .. 4=legendary).
static const uint32_t RARITY_COLOR[5] = {
    0xBBBBBB,  // common    (brightened gray)
    0x4EBA65,  // uncommon  (green)
    0xB1B9F9,  // rare      (blue)
    0xAF87FF,  // epic      (purple)
    0xFFC107,  // legendary (gold)
};
// Which elements the rarity color tints (BuddyState.tint bitmask).
#define BUDDY_TINT_STARS    0x01
#define BUDDY_TINT_NAME     0x02
#define BUDDY_TINT_CREATURE 0x04

// ---- Layout (from board geometry; no #ifdef BOARD_*) ----
static struct {
    const lv_font_t* creature_font;
    const lv_font_t* meta_font;
    const lv_font_t* stats_font;
    const lv_font_t* remark_font;
    int16_t creature_y;
    int16_t meta_y;
    int16_t stars_y;
    int16_t stats_y;
} BL;

static lv_obj_t* root        = nullptr;
static lv_obj_t* lbl_meta    = nullptr;  // name + level (styrene)
static lv_obj_t* lbl_stars   = nullptr;  // rarity ★ row (mono — styrene lacks ★)
static lv_obj_t* lbl_art     = nullptr;  // the creature
static lv_obj_t* lbl_stats   = nullptr;  // 5 stat rows
static lv_obj_t* bubble      = nullptr;  // transient speech bubble (the buddy "talks")
static lv_obj_t* lbl_remark  = nullptr;  // bubble text

static BuddyState state = {};

// Sub-view cycled by "cycle within" (PWR by default) while on the buddy screen.
enum BuddyView { BUDDY_VIEW_FULL, BUDDY_VIEW_CREATURE, BUDDY_VIEW_STATS, BUDDY_VIEW_COUNT };
static BuddyView view = BUDDY_VIEW_FULL;

// ---- Animation ----
static uint8_t  cur_frame = 0;
static uint32_t frame_started_ms = 0;
static bool     blinking = false;
static uint32_t blink_started_ms = 0;
#define FRAME_HOLD_MS 700
#define BLINK_EVERY_MS 4200
#define BLINK_LEN_MS   140

// ---- Speech bubble (the buddy's remarks) ----
#define REMARK_SHOW_DEFAULT_MS 7000   // bubble lifetime until the daemon sets one
static uint32_t remark_show_ms   = REMARK_SHOW_DEFAULT_MS;
static char     shown_remark[96] = "";
static uint32_t remark_until_ms  = 0;
static bool     remark_active    = false;

// Device-local phrases for the LEFT+RIGHT "poke" combo — picked without a
// daemon round-trip, so they're generic (not event-aware) but instant. ASCII
// only, buddy voice. See buddy_say_random().
static const char* BUDDY_PHRASES[] = {
    "poke poke", "yes? what is it", "hi there", "boop", "you rang?",
    "i'm working, i'm working", "ready when you are", "looking sharp today",
    "let's build something", "i believe in you", "coffee break?",
    "stay hydrated, dev", "ship it", "no bugs today, promise",
    "that's a nice variable name", "* waves *",
};
#define BUDDY_PHRASE_COUNT (sizeof(BUDDY_PHRASES) / sizeof(BUDDY_PHRASES[0]))

// Append `src` to `dst` (bounded), returning the new end pointer.
static char* append(char* dst, const char* end, const char* src) {
    while (*src && dst < end - 1) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

// Substitute every "{E}" in `line` with the eye glyph into the buffer.
static char* append_line_with_eye(char* dst, const char* end,
                                   const char* line, const char* eye) {
    for (const char* p = line; *p && dst < end - 1; ) {
        if (p[0] == '{' && p[1] == 'E' && p[2] == '}') {
            dst = append(dst, end, eye);
            p += 3;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    return dst;
}

static const char* current_eye(void) {
    if (blinking) return BUDDY_BLINK_EYE;
    uint8_t m = state.mood < BUDDY_MOOD_COUNT ? state.mood : 0;
    return BUDDY_MOOD_EYE[m];
}

static void render_art(void) {
    if (!lbl_art || !state.valid) return;
    uint8_t sp = state.species < BUDDY_SPECIES_COUNT ? state.species : 0;
    const BuddySpecies& spec = BUDDY_SPECIES_ART[sp];
    const BuddyFrame& f = spec.frames[cur_frame % BUDDY_FRAMES];
    const char* eye = current_eye();

    // Optional hat replaces the (blank) top line, or tucks between wyvern horns.
    uint8_t hat = state.hat < 8 ? state.hat : 0;
    bool is_wyvern = (strcmp(spec.name, "wyvern") == 0);
    const char* hat_line = nullptr;
    if (hat != 0) {
        if (is_wyvern) {
            if (BUDDY_WYVERN_HAT[hat][0]) hat_line = BUDDY_WYVERN_HAT[hat];
        } else if (f.lines[0] && f.lines[0][0] && f.lines[0][strspn(f.lines[0], " ")] == '\0') {
            hat_line = BUDDY_HAT_ART[hat];  // line 0 is all spaces → safe to hat
        }
    }

    static char buf[640];
    char* dst = buf;
    const char* end = buf + sizeof(buf);
    for (int i = 0; f.lines[i] != NULL; i++) {
        const char* line = (i == 0 && hat_line) ? hat_line : f.lines[i];
        dst = append_line_with_eye(dst, end, line, eye);
        if (f.lines[i + 1] != NULL) dst = append(dst, end, "\n");
    }
    lv_label_set_text(lbl_art, buf);
}

static void render_meta(void) {
    if (!state.valid) return;
    if (lbl_meta) {
        static char buf[48];
        snprintf(buf, sizeof(buf), "%s  Lv%u",
                 state.name[0] ? state.name : "Buddy", state.level);
        lv_label_set_text(lbl_meta, buf);
    }
    if (lbl_stars) {
        char stars[32] = "";  // ★ only exists in the Mono font, not Styrene
        uint8_t n = (state.rarity < 5 ? state.rarity : 4) + 1;
        for (uint8_t i = 0; i < n; i++) strcat(stars, STAR);
        lv_label_set_text(lbl_stars, stars);
    }
}

static void render_stats(void) {
    if (!lbl_stats || !state.valid) return;
    bool full = (view == BUDDY_VIEW_STATS);  // dedicated view → full stat names
    int lblw = full ? 9 : 3;
    static char buf[320];
    char* dst = buf;
    const char* end = buf + sizeof(buf);
    for (int i = 0; i < BUDDY_STAT_COUNT; i++) {
        int val = state.stats[i] > 100 ? 100 : state.stats[i];
        int filled = (val * STAT_BARS + 50) / 100;   // round to nearest cell
        char row[96];
        char* r = row;
        r += snprintf(r, sizeof(row), "%-*s ", lblw,
                      full ? BUDDY_STAT_FULL[i] : BUDDY_STAT_LABELS[i]);
        for (int b = 0; b < STAT_BARS; b++)
            r = append(r, row + sizeof(row), b < filled ? BAR_FULL : BAR_EMPTY);
        snprintf(r, sizeof(row) - (r - row), " %3d", val);
        dst = append(dst, end, row);
        if (i + 1 < BUDDY_STAT_COUNT) dst = append(dst, end, "\n");
    }
    lv_label_set_text(lbl_stats, buf);
}

static void position_creature(bool talking);  // defined below; used by apply_view

// Show/hide + reposition the labels for the current sub-view. The header
// (name/level + stars) is shown in every view; the creature and the stat bars
// toggle. STATS view renders the bars centered with full stat names.
static void apply_view(void) {
    if (!root) return;
    bool show_art   = (view != BUDDY_VIEW_STATS);
    bool show_stats = (view != BUDDY_VIEW_CREATURE);

    if (lbl_art) {
        if (show_art) lv_obj_clear_flag(lbl_art, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(lbl_art, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_stats) {
        if (show_stats) lv_obj_clear_flag(lbl_stats, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(lbl_stats, LV_OBJ_FLAG_HIDDEN);
    }

    switch (view) {
    case BUDDY_VIEW_FULL:
        lv_obj_align(lbl_art, LV_ALIGN_TOP_MID, 0, BL.creature_y);
        lv_obj_align(lbl_stats, LV_ALIGN_BOTTOM_MID, 0, BL.stats_y);
        break;
    case BUDDY_VIEW_CREATURE:
        lv_obj_align(lbl_art, LV_ALIGN_CENTER, 0, 10);     // big, centered
        break;
    case BUDDY_VIEW_STATS:
        lv_obj_align(lbl_stats, LV_ALIGN_CENTER, 0, 20);   // block under header
        break;
    default: break;
    }
    render_stats();  // label set (short vs full) depends on the view
    if (remark_active) position_creature(true);  // keep it tucked while talking
}

// While the buddy is talking the opaque bubble would clip the creature's head,
// so we tuck the creature just below the bubble; when it stops talking we put
// it back where the current sub-view wants it. (No-op in STATS view — no art.)
static void position_creature(bool talking) {
    if (!lbl_art || view == BUDDY_VIEW_STATS) return;
    if (talking && bubble && !lv_obj_has_flag(bubble, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_update_layout(root);  // realise bubble height before measuring
        lv_coord_t below = lv_obj_get_y(bubble) + lv_obj_get_height(bubble) + 8;
        lv_obj_align(lbl_art, LV_ALIGN_TOP_MID, 0, below);
    } else if (view == BUDDY_VIEW_FULL) {
        lv_obj_align(lbl_art, LV_ALIGN_TOP_MID, 0, BL.creature_y);
    } else {  // BUDDY_VIEW_CREATURE
        lv_obj_align(lbl_art, LV_ALIGN_CENTER, 0, 10);
    }
}

static void hide_bubble(void) {
    remark_active = false;
    if (bubble) lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);
    position_creature(false);  // restore the creature to its resting spot
}

// Pop the speech bubble with `text` for REMARK_SHOW_MS. Empty text just hides.
static void show_bubble(const char* text) {
    if (!bubble || !lbl_remark) return;
    if (!text || !text[0]) { hide_bubble(); return; }
    lv_label_set_text(lbl_remark, text);
    lv_obj_move_foreground(bubble);  // above creature/stats
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_HIDDEN);
    remark_active = true;
    remark_until_ms = millis() + remark_show_ms;
    position_creature(true);  // tuck the creature under the bubble
}

// Recolor the creature / name / stars per the rarity-tint bitmask. Untinted
// elements keep their theme default (white creature/name, terra-cotta stars).
static void apply_rarity_colors(void) {
    lv_color_t rc = lv_color_hex(RARITY_COLOR[state.rarity < 5 ? state.rarity : 4]);
    if (lbl_art)
        lv_obj_set_style_text_color(
            lbl_art, (state.tint & BUDDY_TINT_CREATURE) ? rc : THEME_TEXT, 0);
    if (lbl_meta)
        lv_obj_set_style_text_color(
            lbl_meta, (state.tint & BUDDY_TINT_NAME) ? rc : THEME_TEXT, 0);
    if (lbl_stars)
        lv_obj_set_style_text_color(
            lbl_stars, (state.tint & BUDDY_TINT_STARS) ? rc : THEME_ACCENT, 0);
}

static void render_all(void) {
    render_art();
    render_meta();
    apply_rarity_colors();
    apply_view();
}

void buddy_init(lv_obj_t* parent) {
    const BoardCaps& c = board_caps();
    bool large = c.height >= 460;
    BL.creature_font = large ? &font_mono_32 : &font_mono_18;
    BL.meta_font     = large ? &font_styrene_28 : &font_styrene_20;
    BL.stats_font    = &font_mono_18;
    BL.remark_font   = large ? &font_styrene_24 : &font_styrene_20;
    BL.meta_y     = large ? 30 : 24;
    BL.stars_y    = large ? 70 : 54;
    BL.creature_y = large ? 112 : 84;
    BL.stats_y    = large ? -28 : -22;   // from bottom

    root = lv_obj_create(parent);
    lv_obj_set_size(root, c.width, c.height);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lbl_meta = lv_label_create(root);
    lv_obj_set_style_text_font(lbl_meta, BL.meta_font, 0);
    lv_obj_set_style_text_color(lbl_meta, THEME_TEXT, 0);
    lv_obj_set_style_text_align(lbl_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_meta, "Hatching...");
    lv_obj_align(lbl_meta, LV_ALIGN_TOP_MID, 0, BL.meta_y);

    lbl_stars = lv_label_create(root);
    lv_obj_set_style_text_font(lbl_stars, &font_mono_18, 0);  // Mono has ★
    lv_obj_set_style_text_color(lbl_stars, THEME_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_stars, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_stars, "");
    lv_obj_align(lbl_stars, LV_ALIGN_TOP_MID, 0, BL.stars_y);

    lbl_art = lv_label_create(root);
    lv_obj_set_style_text_font(lbl_art, BL.creature_font, 0);
    lv_obj_set_style_text_color(lbl_art, THEME_TEXT, 0);
    lv_obj_set_style_text_align(lbl_art, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_art, "");
    lv_obj_align(lbl_art, LV_ALIGN_TOP_MID, 0, BL.creature_y);

    lbl_stats = lv_label_create(root);
    lv_obj_set_style_text_font(lbl_stats, BL.stats_font, 0);
    lv_obj_set_style_text_color(lbl_stats, THEME_DIM, 0);
    lv_obj_set_style_text_align(lbl_stats, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(lbl_stats, "");
    lv_obj_align(lbl_stats, LV_ALIGN_BOTTOM_MID, 0, BL.stats_y);

    // Speech bubble — an opaque rounded panel that floats over the creature
    // when the buddy has something to say, then auto-hides (see buddy_tick).
    bubble = lv_obj_create(root);
    lv_obj_set_width(bubble, c.width - 48);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bubble, THEME_ACCENT, 0);
    lv_obj_set_style_border_width(bubble, 2, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bubble, LV_ALIGN_TOP_MID, 0, BL.stars_y + (large ? 36 : 28));

    lbl_remark = lv_label_create(bubble);
    lv_obj_set_width(lbl_remark, lv_pct(100));
    lv_label_set_long_mode(lbl_remark, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_remark, BL.remark_font, 0);
    lv_obj_set_style_text_color(lbl_remark, THEME_TEXT, 0);
    lv_obj_set_style_text_align(lbl_remark, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_remark, "");

    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

void buddy_set_state(const BuddyState* s) {
    if (!s) return;
    state = *s;
    state.valid = true;
    if (s->bubble_secs) remark_show_ms = (uint32_t)s->bubble_secs * 1000u;
    cur_frame = 0;
    frame_started_ms = millis();
    render_all();

    // A new, non-empty remark pops the bubble. We compare to the last shown
    // line so re-sending identical state (every poll) doesn't re-trigger it.
    if (s->remark[0] && strcmp(s->remark, shown_remark) != 0) {
        strlcpy(shown_remark, s->remark, sizeof(shown_remark));
        show_bubble(shown_remark);
    } else if (!s->remark[0]) {
        shown_remark[0] = '\0';
    }
}

void buddy_tick(void) {
    if (!root || !state.valid) return;
    uint32_t now = millis();

    // Expire the speech bubble even while the screen is hidden, so we don't
    // return to a stale remark next time the buddy is shown.
    if (remark_active && (int32_t)(now - remark_until_ms) >= 0) hide_bubble();

    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;

    bool dirty = false;
    if (!blinking && now - blink_started_ms >= BLINK_EVERY_MS) {
        blinking = true;
        blink_started_ms = now;
        dirty = true;
    } else if (blinking && now - blink_started_ms >= BLINK_LEN_MS) {
        blinking = false;
        blink_started_ms = now;
        dirty = true;
    }

    if (now - frame_started_ms >= FRAME_HOLD_MS) {
        cur_frame = (cur_frame + 1) % BUDDY_FRAMES;
        frame_started_ms = now;
        dirty = true;
    }

    if (dirty) render_art();
}

void buddy_show(void) {
    if (!root) return;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    view = BUDDY_VIEW_FULL;   // always (re)enter on the full view
    frame_started_ms = blink_started_ms = millis();
    render_all();
}

void buddy_cycle_view(void) {
    view = (BuddyView)((view + 1) % BUDDY_VIEW_COUNT);
    apply_view();
}

void buddy_say_random(void) {
    static uint32_t last = 0xFFFFFFFF;
    // No daemon round-trip: pick from the local pool. millis() makes the choice
    // effectively random at a human press; skip an immediate repeat.
    uint32_t i = (millis() >> 3) % BUDDY_PHRASE_COUNT;
    if (i == last) i = (i + 1) % BUDDY_PHRASE_COUNT;
    last = i;
    strlcpy(shown_remark, BUDDY_PHRASES[i], sizeof(shown_remark));
    show_bubble(shown_remark);
}

void buddy_set_bubble_ms(uint32_t ms) { if (ms) remark_show_ms = ms; }
uint32_t buddy_get_bubble_ms(void) { return remark_show_ms; }

void buddy_hide(void) {
    if (root) lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* buddy_get_root(void) { return root; }
