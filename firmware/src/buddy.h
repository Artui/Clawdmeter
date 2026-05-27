#pragma once
#include "data.h"
#include <lvgl.h>

// Claude Buddy screen — renders the companion the daemon describes. The device
// holds no buddy logic of its own; it draws species/mood/stats from BuddyState.

// Build the (hidden) buddy widgets inside `parent`. Call once from ui_init().
void buddy_init(lv_obj_t* parent);

// Replace the displayed buddy. Safe to call before buddy_init (latched).
void buddy_set_state(const BuddyState* s);

// Advance the idle animation (frame cycle + blink). Call every loop.
void buddy_tick(void);

// Cycle the on-screen sub-view: Full -> Creature-only -> Stats-only.
void buddy_cycle_view(void);

// Pop a random device-local phrase (e.g. on a LEFT+RIGHT button combo).
void buddy_say_random(void);

// Bubble lifetime in ms (configurable via the daemon's `bd` field). The getter
// lets ui.cpp keep the forced-remark foreground window in sync.
void buddy_set_bubble_ms(uint32_t ms);
uint32_t buddy_get_bubble_ms(void);

void buddy_show(void);
void buddy_hide(void);

// Root container, so ui.cpp can attach the mode-switch click handler.
lv_obj_t* buddy_get_root(void);
