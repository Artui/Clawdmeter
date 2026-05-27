#pragma once
#include <stdint.h>

// Configurable physical-button mapping. Each button "slot" is assigned a UI
// action, persisted in NVS (namespace "clawd"). Defaults to a board-aware
// "navigation" layout. There is no HID action — the device no longer acts as
// a keyboard (see ble.cpp). Set live via the `buttons` serial command.

enum BtnAction : uint8_t {
    BTN_ACTION_NONE = 0,
    BTN_ACTION_SCREEN_NEXT,    // top-level: Data -> Splash -> Buddy -> Data
    BTN_ACTION_SCREEN_PREV,    // top-level, reverse
    BTN_ACTION_CYCLE_WITHIN,   // within screen: Usage<->BT / next-art / buddy view
    BTN_ACTION_SLEEP,          // turn the screen off now
    BTN_ACTION_COUNT,
};

enum BtnSlot : uint8_t {
    BTN_SLOT_PRIMARY = 0,      // left / BOOT (GPIO0)
    BTN_SLOT_SECONDARY,        // right (GPIO18) — boards with button_count >= 2
    BTN_SLOT_PWR_SHORT,        // PWR short press
    BTN_SLOT_PWR_LONG,         // PWR long press (~700 ms)
    BTN_SLOT_COUNT,
};

void controls_init(void);                          // load from NVS or defaults
BtnAction controls_get(BtnSlot slot);
void controls_set(BtnSlot slot, BtnAction action); // persists to NVS
void controls_apply_preset_navigation(void);       // board-aware default + persist

// Serial-UI helpers.
const char* controls_slot_name(BtnSlot slot);
const char* controls_action_name(BtnAction a);
bool controls_slot_from_str(const char* s, BtnSlot* out);
bool controls_action_from_str(const char* s, BtnAction* out);
