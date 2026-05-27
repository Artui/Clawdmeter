#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// Companion ("Claude Buddy") state. The daemon (daemon/buddy.py) is the brain:
// it derives species/rarity/stats deterministically from the account identity
// and computes mood; the device is the body and just renders what it's told.
// Indices match the upstream tables vendored in buddy_art.h.
struct BuddyState {
    uint8_t species;   // index into BUDDY_SPECIES_ART (0..BUDDY_SPECIES_COUNT-1)
    uint8_t rarity;    // 0=common .. 4=legendary; stars shown = rarity + 1
    uint8_t level;     // XP level (Phase 1: cosmetic, supplied by daemon)
    uint8_t mood;      // 0=happy 1=focused 2=excited 3=tired 4=melancholy 5=chaotic
    uint8_t hat;       // 0=none .. 7=tinyduck (index into BUDDY_HAT_ART)
    uint8_t stats[5];  // DEBUGGING, PATIENCE, CHAOS, WISDOM, SNARK (each 0..100)
    char name[16];
    char remark[96];   // transient line the buddy "says" (ASCII; "" when silent)
    bool remark_force; // daemon asks the device to pop the buddy screen to front
    uint8_t tint;      // rarity-color targets bitmask: 1=stars 2=name 4=creature
    uint8_t bubble_secs; // how long a remark bubble stays up (0 = keep default)
    bool valid;        // false until first buddy block parsed
};
