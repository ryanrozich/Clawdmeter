#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GOLD      THEME_GOLD
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle
static char s_account[64] = ""; // account email from the daemon payload (shown in the rotation)

// ---- Accounts pacing screen ----
#define ACCT_WINDOW_MIN 10080   // 7-day limit window
#define ACCT_BAR_W      280
static lv_obj_t* acct_container;
static lv_obj_t* acct_title;
static lv_obj_t* acct_rows[MAX_ACCOUNTS];      // the card panels (accent border = active)
static lv_obj_t* acct_star[MAX_ACCOUNTS];       // ✶ next to the name = recommended
static lv_obj_t* acct_email[MAX_ACCOUNTS];
static lv_obj_t* acct_used[MAX_ACCOUNTS];
static lv_obj_t* acct_bar[MAX_ACCOUNTS];
static lv_obj_t* acct_marker[MAX_ACCOUNTS];
static lv_obj_t* acct_left[MAX_ACCOUNTS];       // "4d 1h left"
static lv_obj_t* acct_elapsed[MAX_ACCOUNTS];    // "42% through"
static lv_obj_t* acct_status[MAX_ACCOUNTS];     // error message in place of usage (invalid token / rate limited)

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}


static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

static void init_accounts_screen(lv_obj_t* scr) {
    acct_container = lv_obj_create(scr);
    lv_obj_set_size(acct_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(acct_container, 0, 0);
    lv_obj_set_style_bg_opa(acct_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(acct_container, 0, 0);
    lv_obj_set_style_pad_all(acct_container, 0, 0);
    lv_obj_clear_flag(acct_container, LV_OBJ_FLAG_SCROLLABLE);

    // No "Accounts" title — it's a known page behind the PWR button, so the
    // vertical space goes to bigger, more glanceable cards instead.
    acct_title = NULL;

    // One card per account, matching the usage screen's panels. A top tag row
    // carries LIVE (active) / USE THIS (recommended); name + used% below it;
    // bar; then "Resets in …" and the week-elapsed readout. Bigger type now
    // that names are short aliases. Card height/position set in ui_update.
    const int pad = 20;
    const int bar_w = L.content_w - 2 * pad;
    for (int i = 0; i < MAX_ACCOUNTS; i++) {
        lv_obj_t* card = lv_obj_create(acct_container);
        lv_obj_set_width(card, L.content_w);
        lv_obj_set_x(card, L.margin);
        lv_obj_set_style_bg_color(card, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_border_color(card, COL_ACCENT, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        acct_rows[i] = card;

        // Mirrors the usage card: BIG used% on the left (pace-colored), the
        // account name as a pill on the right, an elevated bar centered below,
        // then "Resets in …" and the week-% footer.
        lv_obj_t* used = lv_label_create(card);       // big number, left
        lv_obj_set_style_text_font(used, &font_styrene_48, 0);
        lv_obj_align(used, LV_ALIGN_TOP_LEFT, pad, 8);
        acct_used[i] = used;

        lv_obj_t* email = make_pill(card, "");        // name pill, right (usage-pill size)
        lv_obj_set_style_text_font(email, &font_styrene_28, 0);
        lv_obj_align(email, LV_ALIGN_TOP_RIGHT, -pad, 14);
        acct_email[i] = email;

        // Gold ✶ recommendation star — placed just left of the name pill in
        // ui_update (once the pill has sized to its text). Gold reads distinct
        // from the terra-cotta "active" border.
        lv_obj_t* star = lv_label_create(card);
        lv_label_set_text(star, "\xE2\x9C\xB6");
        lv_obj_set_style_text_font(star, &font_mono_32, 0);
        lv_obj_set_style_text_color(star, COL_GOLD, 0);
        acct_star[i] = star;

        // Elevated bar (lighter opaque track = raised pill, no border), centered.
        lv_obj_t* bar = lv_bar_create(card);
        lv_obj_set_size(bar, bar_w, 24);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, pad, 62);   // high, right under the number
        lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 12, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        acct_bar[i] = bar;

        lv_obj_t* marker = lv_obj_create(card);    // tick = where the clock is
        lv_obj_set_size(marker, 3, 34);
        lv_obj_set_style_bg_color(marker, COL_TEXT, 0);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(marker, 0, 0);
        lv_obj_set_style_radius(marker, 0, 0);
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
        acct_marker[i] = marker;

        lv_obj_t* left = lv_label_create(card);    // "Resets in 4d 1h" (bigger now)
        lv_obj_set_style_text_font(left, &font_styrene_28, 0);
        lv_obj_set_style_text_color(left, COL_DIM, 0);
        lv_obj_align(left, LV_ALIGN_TOP_LEFT, pad, 100);
        acct_left[i] = left;

        lv_obj_t* el = lv_label_create(card);      // just the week % e.g. "42%"
        lv_obj_set_style_text_font(el, &font_styrene_28, 0);
        lv_obj_set_style_text_color(el, COL_DIM, 0);
        lv_obj_align(el, LV_ALIGN_TOP_RIGHT, -pad, 100);
        acct_elapsed[i] = el;

        // Shown in place of the number/bar/footer when this account has no usable
        // token (invalid token / rate limited / unavailable) — so a failed
        // account reads as actionable feedback instead of silently disappearing.
        lv_obj_t* status = lv_label_create(card);
        lv_obj_set_style_text_font(status, &font_styrene_28, 0);
        lv_obj_align(status, LV_ALIGN_LEFT_MID, pad, 0);
        lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
        acct_status[i] = status;

        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN); // ui_update_accounts reveals
    }

    lv_obj_add_flag(acct_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    init_accounts_screen(scr);
    splash_init(scr);

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);

}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);

    // Account email: shown in the bottom status-line rotation (ui_tick_anim).
    strlcpy(s_account, data->account, sizeof(s_account));
}

static void format_resets_in(int mins, char* buf, size_t len) {
    // A weekly window doesn't get a next reset timestamp from Anthropic until
    // the first prompt of the new window — confirmed live: right after a
    // rollover (or a fresh account's first-ever login), the usage API reports
    // 0% used with no reset time at all. "Resets soon" was misleading there
    // (nothing is imminent — it's just idle), so mins<0 reads as this instead.
    if (mins < 0)            snprintf(buf, len, "Needs a prompt");
    else if (mins >= 1440)   snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    else if (mins >= 60)     snprintf(buf, len, "Resets in %dh", mins / 60);
    else                     snprintf(buf, len, "Resets in %dm", mins);
}

void ui_update_accounts(const AccountsData* data) {
    if (!data->valid) return;
    int n = data->count > MAX_ACCOUNTS ? MAX_ACCOUNTS : data->count;

    // Recommendation = design-panel winner ("Burn-Rate"): the account wasting
    // the most weekly credit per day if left idle.  score = unused% / days_left
    // (days floored at 0.25 ≈ 6h so an about-to-reset account spikes finite).
    // Cap-guard: skip accounts ≥85% used (near their wall, little left) unless
    // every account is.  Tie-break: prefer the NON-active account so a real tie
    // gives an actionable "switch" signal.
    bool any_below_cap = false;
    for (int i = 0; i < n; i++)
        if (data->accounts[i].status == 0 && data->accounts[i].used_pct < 85) any_below_cap = true;
    int rec = -1;
    float best = -1e9f;
    for (int i = 0; i < n; i++) {
        const Account* a = &data->accounts[i];
        if (a->status != 0) continue;   // no usable usage → can't recommend it
        if (any_below_cap && a->used_pct >= 85) continue;
        float days = a->reset_mins / 1440.0f;
        if (days < 0.25f) days = 0.25f;
        float score = (100.0f - a->used_pct) / days;
        bool win = score > best + 0.5f;
        if (!win && rec >= 0 && score > best - 0.5f &&
            data->accounts[rec].active && !a->active) win = true;   // tie -> non-active
        if (win) { best = score; rec = i; }
    }

    const int pad = 20;
    const int bar_w = L.content_w - 2 * pad;
    int top = 16;
    int gap = 12;
    int avail = L.scr_h - top - 12;
    int card_h = (avail - gap * (n - 1)) / (n > 0 ? n : 1);
    if (card_h > 150) card_h = 150;

    for (int i = 0; i < MAX_ACCOUNTS; i++) {
        if (i >= n) { lv_obj_add_flag(acct_rows[i], LV_OBJ_FLAG_HIDDEN); continue; }
        const Account* a = &data->accounts[i];
        lv_obj_clear_flag(acct_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(acct_rows[i], top + i * (card_h + gap));
        lv_obj_set_height(acct_rows[i], card_h);

        lv_label_set_text(acct_email[i], a->email);   // name pill always shown

        if (a->status != 0) {
            // No usable token — show a status message where the number/bar go,
            // so a failed account reads as feedback ("Rozich: Invalid token")
            // rather than silently vanishing from the list.
            lv_obj_add_flag(acct_used[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(acct_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(acct_marker[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(acct_left[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(acct_elapsed[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(acct_star[i], LV_OBJ_FLAG_HIDDEN);
            const char* msg = a->status == 1 ? "Invalid token"
                            : a->status == 2 ? "Rate limited"
                                             : "Unavailable";
            lv_label_set_text(acct_status[i], msg);
            lv_obj_set_style_text_color(acct_status[i],
                                        a->status == 1 ? COL_RED : COL_DIM, 0);
            lv_obj_clear_flag(acct_status[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_width(acct_rows[i], 0, 0);   // errored ≠ active
            continue;
        }

        // Normal (ok) card — reveal the usage widgets in case this slot last
        // rendered an error state, and hide the status label.
        lv_obj_clear_flag(acct_used[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(acct_bar[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(acct_marker[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(acct_left[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(acct_elapsed[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(acct_status[i], LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text_fmt(acct_used[i], "%d%%", a->used_pct);

        int elapsed = 0;
        if (a->reset_mins >= 0) {
            elapsed = (int)((long)(ACCT_WINDOW_MIN - a->reset_mins) * 100 / ACCT_WINDOW_MIN);
            if (elapsed < 0) elapsed = 0;
            if (elapsed > 100) elapsed = 100;
        }
        // Right after a weekly reset, elapsed% is near zero, so almost any
        // usage reads as "over pace" — not a meaningful signal that early.
        // Skip the red/green pace comparison for the window's first 5%
        // (~8.4h of the 7-day window) and stay green during the grace period.
        const int PACE_GRACE_PCT = 5;
        bool over = elapsed >= PACE_GRACE_PCT && a->used_pct > elapsed;
        lv_color_t c = over ? COL_RED : COL_GREEN;
        lv_bar_set_value(acct_bar[i], a->used_pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(acct_bar[i], c, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(acct_used[i], c, 0);   // big number = pace color

        int mx = pad + elapsed * bar_w / 100;       // clock marker over the bar
        lv_obj_align(acct_marker[i], LV_ALIGN_TOP_LEFT, mx - 1, 57);

        char buf[24];
        format_resets_in(a->reset_mins, buf, sizeof(buf));
        lv_label_set_text(acct_left[i], buf);
        lv_label_set_text_fmt(acct_elapsed[i], "%d%%", elapsed);

        // accent BORDER = the account you're using now (active); gold ✶ left of
        // the name = recommended. Same card => you're on the right one;
        // different => switch toward the star.
        lv_obj_set_style_border_width(acct_rows[i], a->active ? 3 : 0, 0);
        if (i == rec) {
            lv_obj_update_layout(acct_email[i]);    // pill sized to its text
            lv_obj_align_to(acct_star[i], acct_email[i], LV_ALIGN_OUT_LEFT_MID, -10, 0);
            lv_obj_clear_flag(acct_star[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(acct_star[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    bool is_email = false;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else if (s_account[0] && (anim_msg_idx % 3 == 2)) {
        text = s_account;              // style B: flash the account every 3rd slot (~4s)
        is_email = true;
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // Whimsical style is "<glyph> <Title-case word>…"; the email skips the
    // trailing ellipsis since it isn't an action word.
    static char buf[80];
    snprintf(buf, sizeof(buf), is_email ? "%s %s" : "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    // Logo + battery chrome only on the usage screen; the splash and accounts
    // screens own their full canvas.
    if (current_screen == SCREEN_USAGE) lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                 lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (acct_container) lv_obj_add_flag(acct_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:    splash_show(); break;
    case SCREEN_USAGE:     lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_ACCOUNTS:  if (acct_container) lv_obj_clear_flag(acct_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_USAGE) lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

// PWR button: cycle usage -> accounts -> splash -> usage.
void ui_cycle_screen(void) {
    switch (current_screen) {
    case SCREEN_USAGE:     ui_show_screen(SCREEN_ACCOUNTS); break;
    case SCREEN_ACCOUNTS:  ui_show_screen(SCREEN_SPLASH); break;
    default:               ui_show_screen(SCREEN_USAGE); break;
    }
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
