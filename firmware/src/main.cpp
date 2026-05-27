#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <Preferences.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "buddy.h"
#include "controls.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"

static UsageData usage = {};
static BuddyState buddy = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData, plus an optional buddy block "b".
// `buddy_present` is set true when a "b" object was found and decoded.
static bool parse_json(const char* json, UsageData* out,
                       BuddyState* bud, bool* buddy_present) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;

    *buddy_present = false;
    JsonObjectConst b = doc["b"];
    if (!b.isNull()) {
        bud->species = b["sp"] | 0;
        bud->rarity  = b["ra"] | 0;
        bud->level   = b["lv"] | 0;
        bud->mood    = b["mo"] | 0;
        bud->hat     = b["ht"] | 0;
        strlcpy(bud->name, b["nm"] | "Buddy", sizeof(bud->name));
        JsonArrayConst st = b["st"];
        for (int i = 0; i < 5; i++) bud->stats[i] = i < (int)st.size() ? (uint8_t)st[i] : 0;
        bud->valid = true;
        *buddy_present = true;
    }
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

// Wipe persisted device state (BLE bonds + NVS prefs) for a clean re-pair.
static void factory_reset() {
    Serial.println("FACTORY_RESET: clearing bonds + NVS");
    ble_clear_bonds();
    Preferences prefs;
    if (prefs.begin("clawd", /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }
    Serial.flush();
    delay(100);
    esp_restart();
}

// `buttons`                     — print the current map
// `buttons preset navigation`   — reset to the board-aware default
// `buttons <slot> <action>`     — assign one slot
//   slots: primary secondary pwr-short pwr-long
//   actions: none screen-next screen-prev cycle-within sleep
static void handle_buttons_cmd(const char* args) {
    while (*args == ' ') args++;
    if (*args == '\0') {
        for (int i = 0; i < BTN_SLOT_COUNT; i++)
            Serial.printf("buttons %s = %s\n", controls_slot_name((BtnSlot)i),
                          controls_action_name(controls_get((BtnSlot)i)));
        return;
    }
    char a[24] = {0}, b[24] = {0};
    int n = sscanf(args, "%23s %23s", a, b);
    if (n == 2 && strcmp(a, "preset") == 0 && strcmp(b, "navigation") == 0) {
        controls_apply_preset_navigation();
        Serial.println("buttons: navigation preset applied");
        return;
    }
    BtnSlot slot; BtnAction action;
    if (n == 2 && controls_slot_from_str(a, &slot) && controls_action_from_str(b, &action)) {
        controls_set(slot, action);
        Serial.printf("buttons %s = %s\n", controls_slot_name(slot), controls_action_name(action));
        return;
    }
    Serial.println("usage: buttons | buttons preset navigation | buttons <slot> <action>");
}

static void handle_serial_cmd(const char* cmd) {
    if (strcmp(cmd, "screenshot") == 0) {
        send_screenshot();
    } else if (strncmp(cmd, "buttons", 7) == 0) {
        handle_buttons_cmd(cmd + 7);
    } else if (strcmp(cmd, "reset") == 0) {
        Serial.println("RESET: restarting");
        Serial.flush();
        delay(100);
        esp_restart();
    } else if (strcmp(cmd, "reset --factory") == 0) {
        factory_reset();
    } else if (strcmp(cmd, "bonding on") == 0) {
        ble_set_bonding(true);
        Serial.println("RESET: restarting to apply");
        Serial.flush();
        delay(100);
        esp_restart();
    } else if (strcmp(cmd, "bonding off") == 0) {
        ble_set_bonding(false);
        Serial.println("RESET: restarting to apply");
        Serial.flush();
        delay(100);
        esp_restart();
    } else if (strcmp(cmd, "bonding") == 0) {
        Serial.printf("bonding=%s\n", ble_get_bonding() ? "on" : "off");
    } else if (cmd[0] != '\0') {
        Serial.printf("unknown cmd: %s\n", cmd);
    }
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            handle_serial_cmd(cmd_buf);
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Run a configured button action.
static void dispatch_action(BtnAction a) {
    switch (a) {
    case BTN_ACTION_SCREEN_NEXT:  ui_screen_next();  break;
    case BTN_ACTION_SCREEN_PREV:  ui_screen_prev();  break;
    case BTN_ACTION_CYCLE_WITHIN: ui_cycle_within(); break;
    case BTN_ACTION_SLEEP:        idle_sleep_now();  break;
    case BTN_ACTION_NONE:
    default:                      break;
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();   // takes over brightness (DISPLAY_DEFAULT_BRIGHTNESS) and starts the idle timer

    power_hal_init();
    imu_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();
    controls_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    splash_tick();
    buddy_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons (configurable; see controls.{h,cpp}) ----
    // Each button slot maps to a UI action. Navigation actions fire on the
    // press edge (momentary, not hold-to-send). The first press from sleep is
    // consumed as a wake-only event by idle_consume_wake_press(), which also
    // notes activity — so no separate idle_note_activity() is needed.
    {
        static bool primary_was = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now && !primary_was)
            if (!idle_consume_wake_press()) dispatch_action(controls_get(BTN_SLOT_PRIMARY));
        primary_was = primary_now;

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now && !secondary_was)
                if (!idle_consume_wake_press()) dispatch_action(controls_get(BTN_SLOT_SECONDARY));
            secondary_was = secondary_now;
        }

        if (power_hal_pwr_pressed())
            if (!idle_consume_wake_press()) dispatch_action(controls_get(BTN_SLOT_PWR_SHORT));
        if (power_hal_pwr_long_pressed())
            if (!idle_consume_wake_press()) dispatch_action(controls_get(BTN_SLOT_PWR_LONG));
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        bool buddy_present = false;
        if (parse_json(ble_get_data(), &usage, &buddy, &buddy_present)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            if (buddy_present) ui_update_buddy(&buddy);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
