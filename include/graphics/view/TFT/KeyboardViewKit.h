#pragma once

#include "fonts.h" // fonts generated for the 320x240 layout
#include "graphics/common/NodeStore.h"
#include "lvgl.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

extern "C" {
extern const lv_image_dsc_t kbview_logo; // KeyboardViewLogo.c, made by the harness from the logo's SVG
}

/**
 * Drawing helpers, palette and layout constants shared by the source files of KeyboardView_320x240.
 */
namespace kbview
{
// palette: of the dark look to begin with; setTheme() changes it, and the next drawing uses what it finds
inline uint32_t c_ground = 0x0B0F14;
inline uint32_t c_surface = 0x151B23;
inline uint32_t c_raised = 0x1E2630;
inline uint32_t c_border = 0x2B3542;
inline uint32_t c_text = 0xEEF2F6;
inline uint32_t c_muted = 0x9FB0C3;
inline uint32_t c_status = 0xC9D4E0;
inline uint32_t c_focus = 0xFFB02E;
inline uint32_t c_focusTint = 0x2E2616;
inline uint32_t c_focusMuted = 0xE8D9B8;
inline uint32_t c_focusHint = 0x8C7A52;
inline uint32_t c_blue = 0x6CB6FF;
inline uint32_t c_ownBubble = 0x14283D;
inline uint32_t c_ownBorder = 0x2F5E8C;
inline uint32_t c_ownMeta = 0xA9CDF2;
inline uint32_t c_scrollbar = 0x5A6878;
inline bool c_lightTheme = false;

inline void setTheme(bool light)
{
    c_lightTheme = light;
    c_ground = light ? 0xEEF1F4 : 0x0B0F14;
    c_surface = light ? 0xFFFFFF : 0x151B23;
    c_raised = light ? 0xDDE3E9 : 0x1E2630;
    c_border = light ? 0xB9C3CE : 0x2B3542;
    c_text = light ? 0x10151A : 0xEEF2F6;
    c_muted = light ? 0x4C5A68 : 0x9FB0C3;
    c_status = light ? 0x27323D : 0xC9D4E0;
    c_focus = light ? 0xC56A00 : 0xFFB02E;
    c_focusTint = light ? 0xFFEFD2 : 0x2E2616;
    c_focusMuted = light ? 0x6A4A0C : 0xE8D9B8;
    c_focusHint = light ? 0x9A7E48 : 0x8C7A52;
    c_blue = light ? 0x0B63C4 : 0x6CB6FF;
    c_ownBubble = light ? 0xD9E9FA : 0x14283D;
    c_ownBorder = light ? 0x86B2E0 : 0x2F5E8C;
    c_ownMeta = light ? 0x2B5A88 : 0xA9CDF2;
    c_scrollbar = light ? 0x8C99A7 : 0x5A6878;
}

// colors the user gives to tiles and nodes; 0 is none
const uint32_t c_palette[] = {0, 0xE5484D, 0xF2811D, 0xE2B93B, 0x46A758, 0x12A594, 0x3E8EF7, 0x8E63CE, 0xD6409F};
const char *const c_paletteNames[] = {"None", "Red", "Orange", "Yellow", "Green", "Teal", "Blue", "Purple", "Pink"};
constexpr uint8_t c_paletteSize = 9;
constexpr uint32_t c_planLine = 0xD05CE3; // the line through the stops of a route on the map
constexpr uint32_t c_owned = 0x3DDC84; // outline of a node that is one's own

// a color between two: percent of the second in the first
inline uint32_t mixColor(uint32_t a, uint32_t b, uint32_t percent)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
        out |= ((x * (100 - percent) + y * percent) / 100) << shift;
    }
    return out;
}

// black or white, whichever reads better on a color
inline uint32_t inkOn(uint32_t color)
{
    const uint32_t light = ((color >> 16) & 0xFF) * 3 + ((color >> 8) & 0xFF) * 6 + (color & 0xFF);
    return light > 1400 ? 0x10151A : 0xFFFFFF;
}

// layout
constexpr int32_t c_width = 320;
constexpr int32_t c_height = 240;
constexpr int32_t c_statusHeight = 22;
constexpr int32_t c_tabHeight = 24;
constexpr int32_t c_rowHeight = 38;
constexpr int32_t c_settingHeight = 27;
constexpr int32_t c_filterHeight = 24;
constexpr int32_t c_bubbleWidth = 250;
constexpr size_t c_quoteChars = 60; // of a quoted message, the label cuts the rest

constexpr size_t c_maxMessages = 1000;      // kept per conversation
constexpr size_t c_maxTotalMessages = 5000; // kept in all conversations together
constexpr size_t c_shownMessages = 20;      // drawn per conversation, a window that follows the picked message
constexpr size_t c_signalSamples = 24;       // of the signal of a node
constexpr size_t c_signalNodes = 64;         // nodes a signal history is kept of
constexpr size_t c_maxWaypoints = 32;
constexpr size_t c_maxChats = 150;           // chats kept; channels and pinned chats always stay
constexpr size_t c_maxPins = 16;             // pinned chats
constexpr size_t c_maxBreadcrumbs = 120;
constexpr size_t c_maxJumps = 50;            // mentions or search hits listed
constexpr size_t c_maxDraft = 200;     // bytes
constexpr size_t c_draftWarn = 40;     // show the remaining count below this
constexpr size_t c_maxTrail = 8;
constexpr uint32_t c_renderInterval = 250; // ms between redraws caused by mesh traffic
constexpr uint32_t c_toastTime = 2000;     // ms
constexpr uint32_t c_peekTime = 1000;      // ms the letter typed last can be read in a password
constexpr uint32_t c_pairingTime = 60000;  // ms a pairing PIN stays up when the radio does not take it off
constexpr uint32_t c_onlineSecs = 60 * 60 + 60;

const lv_font_t *const font12 = &ui_font_montserrat_12;
const lv_font_t *const font14 = &ui_font_montserrat_14;
const lv_font_t *const font16 = &ui_font_montserrat_16;
const lv_font_t *const font20 = &ui_font_montserrat_20;

const char *const c_tabNames[] = {"Home", "Chats", "Nodes", "Map", "Apps", "Settings"};
const char *const c_pageNames[] = {"Settings", "Radio", "Channels", "Device", "Display", "Position", "System", "Sounds", "Wi-Fi and files"};

const uint32_t c_timeouts[] = {0, 15, 30, 60, 120, 300, 900}; // screen off after, seconds; 0: never

inline std::string durationName(uint32_t secs)
{
    char buf[16];
    if (secs == 0)
        return "Never";
    if (secs < 60)
        snprintf(buf, sizeof(buf), "%u s", (unsigned)secs);
    else if (secs < 3600)
        snprintf(buf, sizeof(buf), "%u min", (unsigned)(secs / 60));
    else
        snprintf(buf, sizeof(buf), "%u h", (unsigned)(secs / 3600));
    return buf;
}

inline bool validTime(uint32_t t)
{
    return t > 1000000 && t < UINT32_MAX;
}

/**
 * Role of a node as text. Nodes heard before their details arrived have none, and that is no error.
 */
inline const char *roleText(uint8_t role)
{
    static const char *const names[] = {"Client",      "Client Mute", "Router",     "Router Client", "Repeater",
                                        "Tracker",     "Sensor",      "TAK",        "Client Hidden", "Lost & Found",
                                        "TAK Tracker", "Router Late", "Client Base"};
    return role < sizeof(names) / sizeof(names[0]) ? names[role] : "No details yet";
}

inline bool printable(uint32_t key)
{
    return key >= 0x20 && key < 0x7F;
}

inline lv_obj_t *plain(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

inline lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg)
{
    lv_obj_t *obj = plain(parent, x, y, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    return obj;
}

inline void outline(lv_obj_t *obj, uint32_t color, int32_t width, int32_t radius)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, width, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
}

/**
 * Focus is a 2 px accent border plus a tinted fill. The padding makes up for the thinner
 * idle border so that children do not move when the focus does.
 */
inline void focusFrame(lv_obj_t *obj, bool focused, int32_t radius)
{
    outline(obj, focused ? c_focus : c_border, focused ? 2 : 1, radius);
    lv_obj_set_style_pad_all(obj, focused ? 0 : 1, LV_PART_MAIN);
    if (focused)
        lv_obj_set_style_bg_color(obj, lv_color_hex(c_focusTint), LV_PART_MAIN);
}

/**
 * Label with a fixed width shows one line and ends in dots; width 0 sizes to the text.
 */
inline lv_obj_t *label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font, uint32_t color, const char *text,
                       lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(obj, align, LV_PART_MAIN);
    lv_obj_set_pos(obj, x, y);
    if (w > 0) {
        lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(obj, w, lv_font_get_line_height(font));
    }
    lv_label_set_text(obj, text);
    return obj;
}

inline void flexRow(lv_obj_t *obj, lv_flex_align_t mainAlign, int32_t gap)
{
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, mainAlign, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, gap, LV_PART_MAIN);
}

/**
 * Clock of a time, 24-hour or 12-hour; AM and PM are left out where room is short.
 */
inline void clockString(const struct tm &tm, char *buf, size_t len, bool twelve = false, bool suffix = true)
{
    const int hour = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    if (!twelve)
        strftime(buf, len, "%H:%M", &tm);
    else if (suffix)
        snprintf(buf, len, "%d:%02d %s", hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    else
        snprintf(buf, len, "%d:%02d", hour, tm.tm_min);
}

inline void clockString(uint32_t t, char *buf, size_t len, bool twelve = false, bool suffix = true)
{
    time_t tt = t;
    struct tm tm;
    localtime_r(&tt, &tm);
    clockString(tm, buf, len, twelve, suffix);
}

/**
 * Date as year-month-day, such as "2026-11-18", with the weekday in front where asked for.
 */
inline void dateString(const struct tm &tm, char *buf, size_t len, bool weekday = false, bool longNames = false)
{
    char name[16] = "";
    if (weekday)
        strftime(name, sizeof(name), longNames ? "%A " : "%a ", &tm);
    snprintf(buf, len, "%s%04d-%02d-%02d", name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/**
 * Time of a message as shown in the chat list: clock today, weekday this week, date before.
 */
inline void chatTimeString(uint32_t t, uint32_t now, char *buf, size_t len, bool twelve = false)
{
    buf[0] = '\0';
    if (!validTime(t))
        return;
    time_t tt = t, tn = now;
    struct tm tm, tmNow;
    localtime_r(&tt, &tm);
    localtime_r(&tn, &tmNow);
    if (tm.tm_year == tmNow.tm_year && tm.tm_yday == tmNow.tm_yday)
        clockString(tm, buf, len, twelve);
    else if (now > t && now - t < 6 * 86400)
        strftime(buf, len, "%a", &tm);
    else
        dateString(tm, buf, len);
}

inline void agoString(uint32_t lastHeard, uint32_t now, char *buf, size_t len)
{
    buf[0] = '\0';
    if (!lastHeard || !validTime(now))
        return;
    uint32_t diff = now > lastHeard ? now - lastHeard : 0;
    if (diff < 60)
        snprintf(buf, len, "now");
    else if (diff < 3600)
        snprintf(buf, len, "%u min", (unsigned)(diff / 60));
    else if (diff < 86400)
        snprintf(buf, len, "%u h", (unsigned)(diff / 3600));
    else
        snprintf(buf, len, "%u d", (unsigned)(diff / 86400));
}

inline void hopString(const NodeRecord &node, char *buf, size_t len)
{
    if (node.viaMqtt)
        snprintf(buf, len, "MQTT");
    else if (node.hopsAway < 0)
        snprintf(buf, len, "? hops");
    else if (node.hopsAway == 0)
        snprintf(buf, len, "direct");
    else
        snprintf(buf, len, "%d hop%s", node.hopsAway, node.hopsAway == 1 ? "" : "s");
}

inline std::string folded(const char *text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)tolower(c); });
    return out;
}

inline std::string oneLine(const std::string &text)
{
    std::string out(text);
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

/**
 * Lists do not wrap; the focused row keeps one row of context above and below while there is more to see.
 */
inline size_t scrollTop(size_t top, size_t focus, size_t count, size_t rows)
{
    if (count <= rows)
        return 0;
    if (focus < top + 1)
        top = focus ? focus - 1 : 0;
    else if (focus + 2 > top + rows)
        top = focus + 2 - rows;
    return std::min(top, count - rows);
}

struct RowSpec {
    const char *chip;
    uint32_t chipBg;
    uint32_t chipFg;
    bool chipOutline;
    const char *title;
    const char *sub;
    const char *right;
    const char *rightSub;
    uint32_t badge;
    uint32_t chipRing = 0; // color of a heavier outline of the chip; 0: none
};

/**
 * Two-line list row shared by Chats and Nodes: chip, title over a muted line, and a right column.
 */
inline void listRow(lv_obj_t *parent, int32_t y, bool focused, const RowSpec &spec)
{
    lv_obj_t *row = plain(parent, 2, y, c_width - 6, c_rowHeight);
    if (focused) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        focusFrame(row, true, 5);
    } else {
        outline(row, c_raised, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 1, LV_PART_MAIN);
    }
    const uint32_t muted = focused ? c_focusMuted : c_muted;

    lv_obj_t *chip = box(row, 4, 5, 42, 24, spec.chipBg);
    outline(chip, spec.chipRing ? spec.chipRing : c_scrollbar, spec.chipRing ? 2 : spec.chipOutline ? 1 : 0, 4);
    lv_obj_center(label(chip, 0, 0, 0, font12, spec.chipFg, spec.chip));

    label(row, 52, 0, 186, font14, c_text, spec.title);
    label(row, 52, 18, 186, font12, muted, spec.sub);
    label(row, 242, 2, 64, font12, c_text, spec.right, LV_TEXT_ALIGN_RIGHT);
    if (spec.badge) {
        char buf[8];
        if (spec.badge > 99)
            snprintf(buf, sizeof(buf), "99+");
        else
            snprintf(buf, sizeof(buf), "%u", (unsigned)spec.badge);
        const int32_t w = spec.badge > 99 ? 30 : spec.badge > 9 ? 24 : 18;
        lv_obj_t *pill = box(row, 306 - w, 17, w, 16, c_blue);
        lv_obj_set_style_radius(pill, 8, LV_PART_MAIN);
        lv_obj_center(label(pill, 0, 0, 0, font12, c_ground, buf));
    } else {
        label(row, 242, 18, 64, font12, muted, spec.rightSub, LV_TEXT_ALIGN_RIGHT);
    }
}

inline void listScrollbar(lv_obj_t *parent, int32_t y, size_t top, size_t count, size_t rows, int32_t rowHeight = c_rowHeight,
                          int32_t x = c_width - 3)
{
    if (count <= rows)
        return;
    const int32_t track = (int32_t)rows * rowHeight - 4;
    const int32_t h = std::max<int32_t>(12, track * (int32_t)rows / (int32_t)count);
    const int32_t offset = (track - h) * (int32_t)top / (int32_t)(count - rows);
    lv_obj_set_style_radius(box(parent, x, y + 2 + offset, 2, h, c_scrollbar), 1, LV_PART_MAIN);
}

/**
 * One line of a settings page: what it is on the left, what it is set to on the right.
 */
inline void settingRow(lv_obj_t *parent, int32_t y, bool focused, const char *title, const char *value, bool alert)
{
    lv_obj_t *row = plain(parent, 2, y, c_width - 6, c_settingHeight);
    if (focused) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        focusFrame(row, true, 5);
    } else {
        outline(row, c_raised, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 1, LV_PART_MAIN);
    }
    // each side takes what it needs; when that is too much, the name keeps at least 120 px and the value the rest
    constexpr int32_t room = c_width - 6 - 16 - 10;
    lv_obj_t *titleLabel = label(row, 8, 3, 0, font14, c_text, title);
    lv_obj_t *valueLabel =
        label(row, 0, 5, 0, font12, alert ? c_focus : focused ? c_focusMuted : c_muted, value, LV_TEXT_ALIGN_RIGHT);
    lv_obj_update_layout(row);
    const int32_t titleWidth =
        std::min<int32_t>(lv_obj_get_width(titleLabel), std::max<int32_t>(room - lv_obj_get_width(valueLabel), 120));
    for (lv_obj_t *obj : {titleLabel, valueLabel})
        lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(titleLabel, titleWidth, lv_font_get_line_height(font14));
    lv_obj_set_size(valueLabel, room - titleWidth, lv_font_get_line_height(font12));
    lv_obj_set_x(valueLabel, 8 + titleWidth + 10);
}

/**
 * Text field that only shows text: the view owns the text and the caret, keys arrive through its key sink.
 */
inline lv_obj_t *textInput(lv_obj_t *parent, const char *text, size_t caret, const char *placeholder, bool oneLine)
{
    lv_obj_t *input = lv_textarea_create(parent);
    lv_group_remove_obj(input);
    lv_obj_remove_style_all(input);
    lv_obj_set_clickable(input, false);
    lv_obj_set_scrollbar_mode(input, LV_SCROLLBAR_MODE_OFF);
    lv_textarea_set_one_line(input, oneLine);
    lv_obj_set_height(input, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(input, 3 * lv_font_get_line_height(font14), LV_PART_MAIN);
    lv_obj_set_style_text_font(input, font14, LV_PART_MAIN);
    lv_obj_set_style_text_color(input, lv_color_hex(c_text), LV_PART_MAIN);
    lv_obj_set_style_text_color(input, lv_color_hex(c_focusHint), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_side(input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_color(input, lv_color_hex(c_focus), LV_PART_CURSOR);
    lv_obj_set_style_border_opa(input, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_border_width(input, 2, LV_PART_CURSOR);
    lv_textarea_set_placeholder_text(input, placeholder);
    lv_textarea_set_text(input, text);
    lv_textarea_set_cursor_pos(input, (int32_t)caret);
    return input;
}

inline void emptyNote(lv_obj_t *parent, const char *text)
{
    lv_obj_center(label(parent, 0, 0, 0, font14, c_muted, text));
}
} // namespace kbview
