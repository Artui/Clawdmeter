#pragma once
#include <stdbool.h>

void idle_init(void);
void idle_tick(void);
void idle_note_activity(void);

// Force the screen off now (manual sleep, e.g. PWR long-press). Unlike the
// auto-idle timeout, a manual sleep stays asleep even on USB power; any button
// press (via idle_consume_wake_press) wakes it again.
void idle_sleep_now(void);

// Returns true if this press was consumed as a wake-up (caller MUST skip the
// button's normal action). Returns false when already awake — also notes the
// activity, so callers don't need a separate idle_note_activity() call.
bool idle_consume_wake_press(void);

// Touch should NOT count as activity (avoids accidental wakes from pets,
// sleeves, etc.). Callers use this to silently drop touch events while the
// panel is dark.
bool idle_is_asleep(void);
