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

void buddy_show(void);
void buddy_hide(void);

// Root container, so ui.cpp can attach the mode-switch click handler.
lv_obj_t* buddy_get_root(void);
