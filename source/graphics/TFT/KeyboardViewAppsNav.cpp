#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "util/GeoMath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kbview;

/**
 * Nearby: who is close, how far and which way, and a page that points at one of them.
 * Distances are straight lines; a position shared with reduced precision is never shown closer than it can be off.
 */

namespace
{

constexpr size_t c_nearbyFilter = 16; // characters of the typed filter
constexpr int32_t c_ringSize = 150;   // of the pointer page
constexpr int32_t c_ringLeft = 12;
constexpr int32_t c_ringTop = 34;
constexpr int32_t c_plotRadius = 100; // of the vector view
constexpr int32_t c_plotX = 108;      // its center
constexpr int32_t c_plotY = 109;
constexpr size_t c_plotLines = 24;    // nodes drawn at most, the nearest ones
constexpr size_t c_plotFit = 3;       // the scale takes in this many of the nearest, and the focused node
constexpr size_t c_plotNames = 8;     // lines that get a name at their end, where there is room for it
constexpr int32_t c_hintHeight = 16;  // the line below the list that names the other view

/**
 * The distance the outer ring stands for: the next of 1, 2, 5 times a power of ten that takes `meters` in.
 */
double ringScale(double meters, bool imperial)
{
    const double unit = imperial ? (meters < 0.3 * 1609.344 ? 0.3048 : 1609.344) : 1.0;
    const double want = std::max(meters / unit, 1e-3);
    double step = pow(10.0, floor(log10(want)));
    for (double factor : {1.0, 2.0, 5.0, 10.0}) {
        if (step * factor >= want) {
            step *= factor;
            break;
        }
    }
    return step * unit;
}

void radiusText(double radius, bool imperial, char *buf, size_t len)
{
    char distance[16];
    geo::distanceText(radius, imperial, distance, sizeof(distance));
    snprintf(buf, len, "±%s", distance);
}

} // namespace

/**
 * A node as seen from here. Without an own position there is no distance and no bearing.
 */
KeyboardView_320x240::Near KeyboardView_320x240::nearTo(const NodeRecord &node) const
{
    Near near;
    near.node = &node;
    near.radius = geo::precisionRadius(node.position.precision);
    const NodeRecord *own = nodeStore.find(ownNode);
    if (own && own->position.hasCoordinates() && node.position.hasCoordinates()) {
        const geo::Leg leg =
            geo::leg(own->position.latitude, own->position.longitude, node.position.latitude, node.position.longitude);
        near.meters = std::max(leg.meters, near.radius);
        near.bearing = leg.bearing;
        near.ranged = true;
    }
    return near;
}

/**
 * Every node with a position that passes the filter, nearest first; while this device has no position, last
 * heard first.
 */
std::vector<KeyboardView_320x240::Near> KeyboardView_320x240::nearbyList(void) const
{
    const std::string filter = folded(nearbyFilter.c_str());
    std::vector<Near> list;
    for (const auto &[id, node] : nodeStore.records()) {
        if (id == ownNode || !node.position.hasCoordinates() || ignoredNodes.count(id))
            continue;
        if (!filter.empty() && folded(node.user.long_name).find(filter) == std::string::npos &&
            folded(node.user.short_name).find(filter) == std::string::npos)
            continue;
        list.push_back(nearTo(node));
    }
    std::sort(list.begin(), list.end(), [](const Near &a, const Near &b) {
        if (a.ranged && b.ranged && a.meters != b.meters)
            return a.meters < b.meters;
        return a.node->lastHeard != b.node->lastHeard ? a.node->lastHeard > b.node->lastHeard : a.node->id < b.node->id;
    });
    return list;
}

/**
 * How old the position is. The radio hands on positions without their time, so the age is known only for
 * those that came in while the view ran; for the others it says when the node was last heard.
 */
void KeyboardView_320x240::positionAge(uint32_t nodeNum, uint32_t lastHeard, char *buf, size_t len, bool brief) const
{
    char age[16];
    const auto at = positionTimes.find(nodeNum);
    const bool known = at != positionTimes.end();
    agoString(known ? at->second : lastHeard, now(), age, sizeof(age));
    if (!age[0])
        buf[0] = '\0';
    else if (brief) // a list row has room for ten characters: "fix" marks an age that is the position's own
        snprintf(buf, len, "%s%s", known ? "fix " : "", age);
    else if (!strcmp(age, "now"))
        snprintf(buf, len, known ? "Position just in" : "Heard just now");
    else
        snprintf(buf, len, known ? "Position %s old" : "Heard %s ago", age);
}

void KeyboardView_320x240::nearbyKey(uint32_t key)
{
    if (app == App::Pointer) {
        if (key == LV_KEY_LEFT)
            closeApp();
        return;
    }
    if (key == LV_KEY_BACKSPACE) {
        if (!nearbyFilter.empty())
            nearbyFilter.pop_back();
    } else if (printable(key) && key != '@' && nearbyFilter.size() < c_nearbyFilter) {
        nearbyFilter.push_back((char)key);
    } else if (key == LV_KEY_LEFT && prefs.nearbyVectors) {
        prefs.nearbyVectors = false; // the list is the view to the left
        savePrefs();
    } else if (key == LV_KEY_LEFT) {
        closeApp();
    } else if (key == LV_KEY_RIGHT) {
        if (!prefs.nearbyVectors) {
            prefs.nearbyVectors = true;
            savePrefs();
        }
    } else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        const std::vector<Near> list = nearbyList();
        if (list.empty())
            return;
        auto it = std::find_if(list.begin(), list.end(), [this](const Near &near) { return near.node->id == nearbyFocus; });
        const int index = it == list.end() ? 0 : (int)(it - list.begin()) + (key == LV_KEY_UP ? -1 : 1);
        nearbyFocus = list[std::clamp(index, 0, (int)list.size() - 1)].node->id;
    }
}

/**
 * OK: from the list to the pointer page of the focused node, and there to what can be done with the node.
 */
void KeyboardView_320x240::nearbyActivate(void)
{
    if (app == App::Pointer) {
        if (pointerPlace)
            showPlace(placeLatitude * 1e-7, placeLongitude * 1e-7);
        else if (nodeStore.find(pointerNode))
            openNodeMenu(pointerNode);
        return;
    }
    if (!nodeStore.find(nearbyFocus))
        return;
    pointerNode = nearbyFocus;
    pointerPlace = false;
    pointerFrom = App::Nearby;
    pointerChat = UINT32_MAX;
    app = App::Pointer;
}

void KeyboardView_320x240::nearbyHold(void)
{
    const uint32_t node = app == App::Pointer ? pointerNode : nearbyFocus;
    if (nodeStore.find(node))
        openNodeMenu(node);
}

/**
 * Back inside Nearby; false when there is nothing left to step out of.
 */
/**
 * The pointer page on a place that is not a node. Back leads to where it was opened from.
 */
void KeyboardView_320x240::pointTo(double latitude, double longitude, const std::string &name)
{
    const bool fromChat = inConversation;
    const uint32_t chat = convKey;
    const App from = inApp() ? app : App::None;
    openApp(App::Pointer);
    pointerPlace = true;
    placeLatitude = (int32_t)lround(latitude * 1e7);
    placeLongitude = (int32_t)lround(longitude * 1e7);
    placeName = name;
    pointerFrom = from;
    pointerChat = fromChat ? chat : UINT32_MAX;
}

bool KeyboardView_320x240::nearbyBack(void)
{
    if (app == App::Pointer) {
        if (pointerChat != UINT32_MAX) {
            // it was opened from a message: back to that chat
            const uint32_t chat = pointerChat;
            pointerChat = UINT32_MAX;
            app = App::None;
            appFromList = false;
            openChat(chat);
        } else {
            app = pointerFrom;
            if (app == App::None && tab == Tab::Home && !trail.empty()) {
                // it was opened from another tab: back to that one
                tab = trail.back();
                trail.pop_back();
            }
        }
        return true;
    }
    if (app == App::Nearby && !nearbyFilter.empty()) {
        nearbyFilter.clear();
        return true;
    }
    return false;
}

void KeyboardView_320x240::renderNearby(void)
{
    const std::vector<Near> list = nearbyList();
    const NodeRecord *own = nodeStore.find(ownNode);
    const bool fix = own && own->position.hasCoordinates();
    const bool imperial = imperialUnits();
    int32_t top = 2;
    if (!nearbyFilter.empty() || !fix) {
        // one strip for what is typed and for what the list cannot know
        lv_obj_t *strip = plain(content, 6, 3, c_width - 12, c_filterHeight - 4);
        flexRow(strip, LV_FLEX_ALIGN_START, 8);
        if (!nearbyFilter.empty()) {
            lv_obj_t *chip = box(strip, 0, 0, LV_SIZE_CONTENT, c_filterHeight - 4, c_raised);
            outline(chip, c_focus, 1, 4);
            lv_obj_set_style_pad_hor(chip, 6, LV_PART_MAIN);
            flexRow(chip, LV_FLEX_ALIGN_START, 4);
            label(chip, 0, 0, 0, font12, c_muted, "Filter");
            label(chip, 0, 0, 0, font12, c_text, nearbyFilter.c_str());
        }
        if (!fix)
            label(strip, 0, 0, 0, font12, c_focus, "No GPS fix: last heard first");
        top += c_filterHeight;
    }
    if (list.empty()) {
        emptyNote(content, nearbyFilter.empty() ? "No node has shared a position" : "No node matches");
        return;
    }
    auto it = std::find_if(list.begin(), list.end(), [this](const Near &near) { return near.node->id == nearbyFocus; });
    const size_t focus = it == list.end() ? 0 : (size_t)(it - list.begin());
    nearbyFocus = list[focus].node->id;
    if (prefs.nearbyVectors && fix) {
        // lines need an own position to start from; without one the list is all there is
        lv_obj_clean(content);
        renderVectors(list, focus);
        return;
    }
    const int32_t height = lv_obj_get_height(content);
    const size_t rows = (height - top - 2 - c_hintHeight) / c_rowHeight;
    nearbyTop = scrollTop(nearbyTop, focus, list.size(), rows);
    label(content, 8, height - c_hintHeight, c_width - 16, font12, c_focusHint,
          prefs.nearbyVectors ? "Lines need a GPS fix" : "Right: lines   OK: pointer", LV_TEXT_ALIGN_CENTER);

    for (size_t i = nearbyTop; i < list.size() && i < nearbyTop + rows; i++) {
        const Near &near = list[i];
        const NodeRecord &node = *near.node;
        const std::string chip = shortName(node.id);
        const std::string name = chatName(node.id);
        char distance[16] = "", age[24], part[24];
        std::string sub;
        if (near.ranged) {
            geo::distanceText(near.meters, imperial, distance, sizeof(distance));
            snprintf(part, sizeof(part), "%s %d°", geo::cardinal(near.bearing), (int)lround(near.bearing) % 360);
            sub = part;
        }
        if (near.radius > 0) {
            radiusText(near.radius, imperial, part, sizeof(part));
            sub += sub.empty() ? part : std::string(" · ") + part;
        }
        if (node.hopsAway == 0 && !node.viaMqtt && node.lastHeard) {
            snprintf(part, sizeof(part), "%.1f dB", node.snr);
            sub += sub.empty() ? part : std::string(" · ") + part;
        }
        positionAge(node.id, node.lastHeard, age, sizeof(age), true);
        const auto [chipBg, chipFg] = colorOf(node.id);
        listRow(content, top + (int32_t)(i - nearbyTop) * c_rowHeight, i == focus,
                RowSpec{chip.c_str(), chipBg, chipFg, false, name.c_str(), sub.c_str(), distance, age, 0,
                        ownedNodes.count(node.id) ? c_owned : 0});
    }
    listScrollbar(content, top, nearbyTop, list.size(), rows);
}

/**
 * The vector view: this device in the middle, north at the top, a line to every node. The outer ring is
 * a round distance that takes in the nearest few and the focused node; what is farther ends on the ring,
 * drawn muted. Up and Down walk through the nodes from near to far.
 */
void KeyboardView_320x240::renderVectors(const std::vector<Near> &list, size_t focus)
{
    const bool imperial = imperialUnits();
    const size_t count = std::min(list.size(), c_plotLines);
    const size_t shownFocus = std::min(focus, list.size() - 1);
    const double fit = std::max(list[std::min(count, c_plotFit) - 1].meters, list[shownFocus].meters);
    const double scale = ringScale(std::max(fit, 1.0), imperial);
    char buf[40];

    for (int32_t radius : {c_plotRadius, c_plotRadius / 2}) {
        lv_obj_t *ring = plain(content, c_plotX - radius, c_plotY - radius, 2 * radius, 2 * radius);
        outline(ring, c_border, 1, radius);
        if (radius == c_plotRadius) {
            lv_obj_set_style_bg_color(ring, lv_color_hex(c_surface), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
    label(content, c_plotX - 10, c_plotY - c_plotRadius + 3, 20, font12, c_muted, "N", LV_TEXT_ALIGN_CENTER);

    // the focused line is drawn last, on top of the others; lines beyond the plot's count are left out
    // unless one of them has the focus
    std::vector<size_t> order;
    for (size_t i = 0; i < count; i++) {
        if (i != shownFocus)
            order.push_back(i);
    }
    order.push_back(shownFocus);
    vectorPoints.assign(order.size(), {});
    // names are placed for the focused node first and then from near to far; one that would cover another is left out
    struct Name {
        int32_t x, y;
        size_t line;
    };
    std::vector<Name> names;
    auto place = [&names](int32_t x, int32_t y, size_t line) {
        for (const Name &other : names) {
            if (abs(other.x - x) < 38 && abs(other.y - y) < 14)
                return;
        }
        names.push_back(Name{x, y, line});
    };
    for (size_t n = 0; n < order.size(); n++) {
        const Near &near = list[order[n]];
        const bool focused = order[n] == shownFocus, beyond = near.meters > scale;
        const double rad = near.bearing * M_PI / 180.0;
        const double length = c_plotRadius * std::min(near.meters / scale, 1.0);
        const int32_t x = c_plotX + (int32_t)lround(sin(rad) * length), y = c_plotY - (int32_t)lround(cos(rad) * length);
        vectorPoints[n][0].x = c_plotX;
        vectorPoints[n][0].y = c_plotY;
        vectorPoints[n][1].x = x;
        vectorPoints[n][1].y = y;
        const uint32_t color = focused ? c_focus : beyond ? c_scrollbar : c_blue;
        lv_obj_t *line = lv_line_create(content);
        lv_obj_remove_style_all(line);
        lv_line_set_points(line, vectorPoints[n].data(), 2);
        lv_obj_set_style_line_width(line, focused ? 3 : 1, LV_PART_MAIN);
        lv_obj_set_style_line_color(line, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
        const int32_t dot = focused ? 4 : 3;
        lv_obj_set_style_radius(box(content, x - dot, y - dot, 2 * dot, 2 * dot, color), dot, LV_PART_MAIN);
    }
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t n = 0; n < order.size(); n++) {
            const bool focused = order[n] == shownFocus;
            if (focused != (pass == 0) || (!focused && order[n] >= c_plotNames))
                continue;
            // the name goes outward of the end of the line, and stays inside the plot's square
            const double rad = list[order[n]].bearing * M_PI / 180.0;
            const int32_t x = (int32_t)vectorPoints[n][1].x, y = (int32_t)vectorPoints[n][1].y;
            place(std::clamp<int32_t>(x + (sin(rad) >= 0 ? 6 : -40), 2, 2 * c_plotX - 36),
                  std::clamp<int32_t>(y + (cos(rad) >= 0 ? -16 : 4), 2, 2 * c_plotY - 16), n);
        }
    }
    for (const Name &name : names) {
        const Near &near = list[order[name.line]];
        const bool focused = order[name.line] == shownFocus;
        label(content, name.x, name.y, 0, font12, focused ? c_focus : near.meters > scale ? c_muted : c_text,
              shortName(near.node->id).c_str());
    }
    lv_obj_set_style_radius(box(content, c_plotX - 3, c_plotY - 3, 6, 6, c_text), 3, LV_PART_MAIN);

    // the focused node, in words
    const Near &near = list[shownFocus];
    const int32_t left = 2 * c_plotX + 6, width = c_width - left - 4;
    lv_obj_t *name = label(content, left, 6, width, font14, c_text, chatName(near.node->id).c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(name, 2 * lv_font_get_line_height(font14));
    geo::distanceText(near.meters, imperial, buf, sizeof(buf));
    label(content, left, 46, width, font20, c_focus, buf);
    snprintf(buf, sizeof(buf), "%s %d°", geo::cardinal(near.bearing), (int)lround(near.bearing) % 360);
    label(content, left, 72, width, font14, c_text, buf);
    int32_t y = 94;
    if (near.radius > 0) {
        radiusText(near.radius, imperial, buf, sizeof(buf));
        label(content, left, y, width, font12, c_focus, buf);
        y += 16;
    }
    positionAge(near.node->id, near.node->lastHeard, buf, sizeof(buf), true);
    if (buf[0]) {
        label(content, left, y, width, font12, c_muted, buf);
        y += 16;
    }
    if (!nearbyFilter.empty())
        label(content, left, y, width, font12, c_muted, ("Filter " + nearbyFilter).c_str());
    // the ring is a round number: no decimals that say nothing
    const double large = imperial ? scale / 1609.344 : scale / 1000.0;
    if (large >= 0.999)
        snprintf(buf, sizeof(buf), "Ring %d %s", (int)lround(large), imperial ? "mi" : "km");
    else
        snprintf(buf, sizeof(buf), "Ring %d %s", (int)lround(imperial ? scale / 0.3048 : scale), imperial ? "ft" : "m");
    label(content, left, 150, width, font12, c_muted, buf);
    snprintf(buf, sizeof(buf), "%u of %u", (unsigned)(shownFocus + 1), (unsigned)list.size());
    label(content, left, 166, width, font12, c_muted, buf);
    label(content, left, 184, width, font12, c_focusHint, "Left: list");
    label(content, left, 200, width, font12, c_focusHint, "OK: pointer");
}

/**
 * The pointer page: an arrow on a ring with north at the top, and the numbers next to it.
 */
void KeyboardView_320x240::renderPointer(void)
{
    const NodeRecord *node = pointerPlace ? nullptr : nodeStore.find(pointerNode);
    if (!pointerPlace && (!node || !node->position.hasCoordinates())) {
        emptyNote(content, "This node has no position any more");
        return;
    }
    Near near;
    if (node) {
        near = nearTo(*node);
    } else {
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates()) {
            const geo::Leg leg = geo::leg(own->position.latitude, own->position.longitude, placeLatitude, placeLongitude);
            near.meters = leg.meters;
            near.bearing = leg.bearing;
            near.ranged = true;
        }
    }
    const bool imperial = imperialUnits();
    const int32_t centerX = c_ringLeft + c_ringSize / 2, centerY = c_ringTop + c_ringSize / 2;
    char buf[40];

    lv_obj_t *title = label(content, 8, 6, c_width - 16, font16, c_text, node ? chatName(node->id).c_str() : placeName.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(title, lv_font_get_line_height(font16));

    lv_obj_t *ring = plain(content, c_ringLeft, c_ringTop, c_ringSize, c_ringSize);
    outline(ring, c_border, 2, c_ringSize / 2);
    lv_obj_set_style_bg_color(ring, lv_color_hex(c_surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    // With a compass the top of the ring is where the device points, and north goes around it.
    float facing = 0;
    const bool turning = compassHeading(facing);
    compassShown = turning ? facing : -1000;
    {
        const double north = -facing * M_PI / 180.0;
        const double reach = c_ringSize / 2 - 12;
        label(content, centerX + (int32_t)lround(sin(north) * reach) - 10, centerY - (int32_t)lround(cos(north) * reach) - 7, 20,
              font12, turning ? c_blue : c_muted, "N", LV_TEXT_ALIGN_CENTER);
    }

    if (near.ranged) {
        // tail, tip, one barb, back to the tip, the other barb: one line object draws the arrow
        const double rad = (near.bearing - facing) * M_PI / 180.0;
        const double dx = sin(rad), dy = -cos(rad);
        const double tip = c_ringSize / 2 - 22, tail = -(c_ringSize / 2 - 40), barb = 20, spread = 0.5;
        auto point = [&](double along, double turn) {
            const double c = cos(turn), s = sin(turn);
            lv_point_precise_t p;
            p.x = (lv_value_precise_t)lround(centerX + (dx * c - dy * s) * along);
            p.y = (lv_value_precise_t)lround(centerY + (dx * s + dy * c) * along);
            return p;
        };
        const lv_point_precise_t head = point(tip, 0);
        pointerPoints[0] = point(tail, 0);
        pointerPoints[1] = head;
        for (int side = 0; side < 2; side++) {
            const double turn = M_PI + (side ? spread : -spread); // back from the tip, off to one side
            const double c = cos(turn), s = sin(turn);
            pointerPoints[2 + 2 * side].x = (lv_value_precise_t)lround(head.x + (dx * c - dy * s) * barb);
            pointerPoints[2 + 2 * side].y = (lv_value_precise_t)lround(head.y + (dx * s + dy * c) * barb);
        }
        pointerPoints[3] = head;
        lv_obj_t *arrow = lv_line_create(content);
        lv_obj_remove_style_all(arrow);
        lv_line_set_points(arrow, pointerPoints, 5);
        lv_obj_set_style_line_width(arrow, 5, LV_PART_MAIN);
        lv_obj_set_style_line_color(arrow, lv_color_hex(c_focus), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(arrow, true, LV_PART_MAIN);
    } else {
        label(content, c_ringLeft, centerY - 8, c_ringSize, font12, c_focus, "No GPS fix", LV_TEXT_ALIGN_CENTER);
    }

    const int32_t left = c_ringLeft + c_ringSize + 14, width = c_width - left - 6;
    if (near.ranged) {
        geo::distanceText(near.meters, imperial, buf, sizeof(buf));
        label(content, left, 44, width, font20, c_text, buf);
        snprintf(buf, sizeof(buf), "%s %d°", geo::cardinal(near.bearing), (int)lround(near.bearing) % 360);
        label(content, left, 72, width, font16, c_text, buf);
    } else {
        label(content, left, 44, width, font16, c_text, "Distance unknown");
    }
    int32_t y = 100;
    if (near.radius > 0) {
        radiusText(near.radius, imperial, buf, sizeof(buf));
        label(content, left, y, width, font12, c_focus, (std::string("Position ") + buf).c_str());
        y += 17;
    }
    if (node) {
        positionAge(node->id, node->lastHeard, buf, sizeof(buf), false);
        if (buf[0]) {
            label(content, left, y, width, font12, c_muted, buf);
            y += 17;
        }
        if (node->hopsAway == 0 && !node->viaMqtt && node->lastHeard) {
            snprintf(buf, sizeof(buf), "%.1f dB, %d dBm", node->snr, (int)node->rssi);
            label(content, left, y, width, font12, c_muted, buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "%.5f", placeLatitude * 1e-7);
        label(content, left, y, width, font12, c_muted, buf);
        snprintf(buf, sizeof(buf), "%.5f", placeLongitude * 1e-7);
        label(content, left, y + 17, width, font12, c_muted, buf);
    }
    if (node) {
        const std::string town = townOf(node->position.latitude, node->position.longitude);
        if (!town.empty())
            label(content, left, 134, width, font12, c_muted, ("In " + town).c_str());
    }
    label(content, left, 150, width, font12, c_muted, "Straight line");
    label(content, left, 166, width, font12, turning ? c_blue : c_muted, turning ? "Turns with you" : "North is up");
    label(content, 8, 196, c_width - 16, font12, c_focusMuted, node ? "OK: node menu   Back: list" : "OK: show on the map   Back", LV_TEXT_ALIGN_CENTER);
}

/**
 * The Nearby tile: the closest node.
 */
KeyboardView_320x240::TileView KeyboardView_320x240::nearbyTile(void) const
{
    TileView view;
    const NodeRecord *own = nodeStore.find(ownNode);
    if (!own || !own->position.hasCoordinates()) {
        view.value = "No fix";
        view.sub = "Waiting for GPS";
        return view;
    }
    const Near *closest = nullptr;
    Near best;
    for (const auto &[id, node] : nodeStore.records()) {
        if (id == ownNode || !node.position.hasCoordinates() || ignoredNodes.count(id))
            continue;
        const Near near = nearTo(node);
        if (!closest || near.meters < best.meters) {
            best = near;
            closest = &best;
        }
    }
    if (!closest) {
        view.value = "--";
        view.sub = "No node with a position";
        return view;
    }
    char buf[16];
    geo::distanceText(best.meters, imperialUnits(), buf, sizeof(buf));
    view.value = buf;
    view.sub = chatName(best.node->id) + " " + geo::cardinal(best.bearing);
    return view;
}

#endif
