#include "controls.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <Preferences.h>

#define PREFS_NS "clawd"

// One NVS key per slot, ordered as BtnSlot. 0xFF = "unset" → use the default.
static const char* const SLOT_KEY[BTN_SLOT_COUNT] = {
    "b_pri", "b_sec", "b_pwrs", "b_pwrl",
};
static const char* const SLOT_NAME[BTN_SLOT_COUNT] = {
    "primary", "secondary", "pwr-short", "pwr-long",
};
static const char* const ACTION_NAME[BTN_ACTION_COUNT] = {
    "none", "screen-next", "screen-prev", "cycle-within", "sleep",
};

static BtnAction action_map[BTN_SLOT_COUNT];

// Board-aware navigation default. Two-button boards get prev/next on
// left/right; single-button boards cycle forward-only on the primary.
static BtnAction nav_default(BtnSlot slot) {
    bool two = board_caps().button_count >= 2;
    switch (slot) {
    case BTN_SLOT_PRIMARY:   return two ? BTN_ACTION_SCREEN_PREV : BTN_ACTION_SCREEN_NEXT;
    case BTN_SLOT_SECONDARY: return BTN_ACTION_SCREEN_NEXT;
    case BTN_SLOT_PWR_SHORT: return BTN_ACTION_CYCLE_WITHIN;
    case BTN_SLOT_PWR_LONG:  return BTN_ACTION_SLEEP;
    default:                 return BTN_ACTION_NONE;
    }
}

void controls_init(void) {
    Preferences prefs;
    prefs.begin(PREFS_NS, /*readOnly=*/true);
    for (int i = 0; i < BTN_SLOT_COUNT; i++) {
        uint8_t v = prefs.getUChar(SLOT_KEY[i], 0xFF);
        action_map[i] = (v < BTN_ACTION_COUNT) ? (BtnAction)v : nav_default((BtnSlot)i);
    }
    prefs.end();
}

BtnAction controls_get(BtnSlot slot) {
    return slot < BTN_SLOT_COUNT ? action_map[slot] : BTN_ACTION_NONE;
}

void controls_set(BtnSlot slot, BtnAction action) {
    if (slot >= BTN_SLOT_COUNT || action >= BTN_ACTION_COUNT) return;
    action_map[slot] = action;
    Preferences prefs;
    prefs.begin(PREFS_NS, /*readOnly=*/false);
    prefs.putUChar(SLOT_KEY[slot], (uint8_t)action);
    prefs.end();
}

void controls_apply_preset_navigation(void) {
    for (int i = 0; i < BTN_SLOT_COUNT; i++)
        controls_set((BtnSlot)i, nav_default((BtnSlot)i));
}

const char* controls_slot_name(BtnSlot slot) {
    return slot < BTN_SLOT_COUNT ? SLOT_NAME[slot] : "?";
}
const char* controls_action_name(BtnAction a) {
    return a < BTN_ACTION_COUNT ? ACTION_NAME[a] : "?";
}

bool controls_slot_from_str(const char* s, BtnSlot* out) {
    for (int i = 0; i < BTN_SLOT_COUNT; i++)
        if (strcmp(s, SLOT_NAME[i]) == 0) { *out = (BtnSlot)i; return true; }
    return false;
}
bool controls_action_from_str(const char* s, BtnAction* out) {
    for (int i = 0; i < BTN_ACTION_COUNT; i++)
        if (strcmp(s, ACTION_NAME[i]) == 0) { *out = (BtnAction)i; return true; }
    return false;
}
