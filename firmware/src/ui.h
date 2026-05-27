#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_BLUETOOTH,
    SCREEN_BUDDY,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_buddy(const BuddyState* buddy);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_screen_next(void);     // top-level: Data -> Splash -> Buddy -> Data
void ui_screen_prev(void);     // top-level, reverse
void ui_cycle_within(void);    // within current screen (Usage/BT, art, buddy view)
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
