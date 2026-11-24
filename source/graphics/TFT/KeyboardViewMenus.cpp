#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "util/GeoMath.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace kbview;

namespace
{

const char *const c_quickReplies[] = {"OK",      "Yes",       "No",      "On my way", "Where are you?",
                                      "Call me", "Need help", "At home", "Back soon", "Thanks"};

constexpr size_t c_pickRows = 6;     // items of a picker on screen
constexpr int32_t c_pickHeight = 24; // one picker item
constexpr int32_t c_pickWidth = 264; // picker panel
constexpr int32_t c_editWidth = 292; // editor panel
constexpr size_t c_infoRows = 9;     // lines of an info sheet on screen
constexpr int32_t c_infoPitch = 16;  // line distance on an info sheet
constexpr int32_t c_qrSize = 160;    // fits 53 modules at 3 px, enough for a channel link
constexpr size_t c_noteWrap = 38;    // notes longer than this take two lines
constexpr uint32_t c_pinSettleTime = 3000; // ms without a change of the channels before the pins are looked at

lv_obj_t *wrapped(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, uint32_t color, const char *text)
{
    lv_obj_t *obj = label(parent, x, y, 0, font12, color, text);
    lv_obj_set_width(obj, w);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    return obj;
}

std::vector<std::string> confirmItems(const char *action)
{
    return {"Cancel", action};
}

} // namespace

// -------- popups --------

/**
 * List to choose from. Values default to the item index; current marks the choice in effect.
 */
void KeyboardView_320x240::openPicker(Setting what, const char *title, std::vector<std::string> &&items,
                                      std::vector<uint32_t> &&values, int32_t current, const char *note, uint32_t target)
{
    std::vector<std::string> list = std::move(items);
    std::vector<uint32_t> codes = std::move(values);
    const std::string heading = title ? title : "", hint = note ? note : "";
    closeMenu();
    if (list.empty())
        return;
    if (codes.size() != list.size()) {
        codes.resize(list.size());
        for (size_t i = 0; i < codes.size(); i++)
            codes[i] = (uint32_t)i;
    }
    popup.what = what;
    popup.target = target;
    popup.title = heading;
    popup.note = hint;
    popup.items = std::move(list);
    popup.values = std::move(codes);
    popup.current = current >= 0 && (size_t)current < popup.items.size() ? current : -1;
    popup.focus = popup.current >= 0 ? (size_t)popup.current : 0;
    menu = Menu::Pick;
    dirty = urgent = true;
}

void KeyboardView_320x240::openEditor(Setting what, const char *title, const std::string &text, size_t maxLen,
                                      const char *placeholder, const char *note, uint32_t target)
{
    // cut to the length, but never inside a character
    size_t keep = std::min(text.size(), maxLen);
    while (keep > 0 && keep < text.size() && ((uint8_t)text[keep] & 0xC0) == 0x80)
        keep--;
    const std::string heading = title ? title : "", hint = note ? note : "", value = text.substr(0, keep);
    const std::string ghost = placeholder ? placeholder : "";
    closeMenu();
    popup.what = what;
    popup.target = target;
    popup.title = heading;
    popup.note = hint;
    popup.text = value;
    popup.caret = value.size();
    popup.maxLen = maxLen;
    popup.placeholder = ghost;
    menu = Menu::Edit;
    dirty = urgent = true;
}

/**
 * Sheet of read-only lines, with a QR code beside them when one is given.
 */
void KeyboardView_320x240::openInfo(const char *title, std::vector<std::string> &&lines, const char *qr)
{
    std::vector<std::string> text = std::move(lines);
    const std::string heading = title ? title : "", code = qr ? qr : "";
    closeMenu();
    popup.title = heading;
    popup.lines = std::move(text);
    popup.qr = code;
    menu = Menu::Info;
    dirty = urgent = true;
}

void KeyboardView_320x240::closeMenu(void)
{
    // a brightness that was only tried out goes back to the stored one
    if (menu == Menu::Pick && popup.what == Setting::Brightness && displaydriver->hasLight() && uiConfig.screen_brightness)
        displaydriver->setBrightness((uint8_t)uiConfig.screen_brightness);
    popup = Popup{};
    menu = Menu::None;
    traceShown = false;
    dirty = urgent = true;
}

void KeyboardView_320x240::handleMenuKey(uint32_t key)
{
    if (key == LV_KEY_HOME) {
        closeMenu();
        goHome();
        return;
    }
    if (key == LV_KEY_ESC) {
        closeMenu();
        return;
    }
    if (key == LV_KEY_ENTER)
        return; // acts on release, see onKeyEvent

    switch (menu) {
    case Menu::Pick: {
        const size_t count = popup.items.size();
        if (count == 0)
            break;
        if (key == LV_KEY_UP) {
            popup.focus = (popup.focus + count - 1) % count;
        } else if (key == LV_KEY_DOWN) {
            popup.focus = (popup.focus + 1) % count;
        } else if (popup.what == Setting::Control && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
            // the control center steps a row's value both ways
            if (popup.focus < popup.values.size())
                controlStep(popup.values[popup.focus], key == LV_KEY_LEFT ? -1 : 1);
            return;
        } else if (key == LV_KEY_LEFT) {
            closeMenu();
            return;
        } else if (key == LV_KEY_RIGHT) {
            activateMenuItem();
            return;
        } else if (printable(key)) {
            // a letter jumps to the next item that starts with it
            for (size_t step = 1; step <= count; step++) {
                const size_t i = (popup.focus + step) % count;
                if (!popup.items[i].empty() && tolower((unsigned char)popup.items[i][0]) == tolower((int)key)) {
                    popup.focus = i;
                    break;
                }
            }
        }
        previewPick();
        break;
    }
    case Menu::Edit: {
        // The caret counts bytes, the text may hold characters of several (a name set from a phone): the caret
        // only ever stands between characters, and Del takes a whole one.
        const auto inside = [this](size_t at) { return at < popup.text.size() && ((uint8_t)popup.text[at] & 0xC0) == 0x80; };
        popup.caret = std::min(popup.caret, popup.text.size());
        while (popup.caret > 0 && inside(popup.caret))
            popup.caret--;
        if (printable(key)) {
            if (popup.text.size() < popup.maxLen) {
                popup.text.insert(popup.caret, 1, (char)key);
                popup.caret++;
                popup.peek = popup.secret;
                popup.peekStart = lv_tick_get();
            }
            break;
        }
        popup.peek = false;
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_LEFT) {
            size_t to = popup.caret;
            if (to > 0)
                to--;
            while (to > 0 && inside(to))
                to--;
            if (key == LV_KEY_BACKSPACE)
                popup.text.erase(to, popup.caret - to);
            popup.caret = to;
        } else if (key == LV_KEY_RIGHT) {
            if (popup.caret < popup.text.size())
                popup.caret++;
            while (inside(popup.caret))
                popup.caret++;
        }
        break;
    }
    case Menu::Info:
        if (key == LV_KEY_UP) {
            if (popup.top > 0)
                popup.top--;
        } else if (key == LV_KEY_DOWN) {
            popup.top++; // clamped when drawn
        }
        break;
    default:
        break;
    }
}

/**
 * OK on a popup. The popup closes first, so the action is free to open the next one.
 */
void KeyboardView_320x240::activateMenuItem(void)
{
    const Popup done = popup;
    const Menu kind = menu;
    // keep a brightness that is about to be stored from flickering back
    if (kind == Menu::Pick && done.what == Setting::Brightness)
        popup.what = Setting::None;
    closeMenu();

    switch (kind) {
    case Menu::Pick:
        if (done.focus < done.values.size())
            applyPick(done.what, done.values[done.focus], done.target);
        break;
    case Menu::Edit:
        applyEdit(done.what, done.text, done.target);
        break;
    case Menu::Info:
        if (done.what == Setting::NodeMenu)
            openNodeMenu(done.target);
        break;
    default:
        break;
    }
}

void KeyboardView_320x240::previewPick(void)
{
    if (menu != Menu::Pick || popup.focus >= popup.values.size())
        return;
    if (popup.what == Setting::AlertTone || popup.what == Setting::ToneDirect || popup.what == Setting::ToneEmergency ||
        popup.what == Setting::ToneTimer || popup.what == Setting::ChannelTone) {
        // a list of tunes: hear the one the focus is on
        if (popup.what == Setting::ChannelTone && popup.values[popup.focus] == c_pickMuted)
            return;
        playTone(toneNameAt(popup.what, popup.values[popup.focus]));
        return;
    }
    if (popup.what != Setting::Brightness)
        return;
    if (displaydriver->hasLight())
        displaydriver->setBrightness((uint8_t)(popup.values[popup.focus] * 255 / 100));
}

void KeyboardView_320x240::renderMenu(void)
{
    if (menu == Menu::None)
        return;

    lv_obj_t *dim = box(overlay, 0, 0, c_width, c_height, c_ground);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, LV_PART_MAIN);

    switch (menu) {
    case Menu::Pick: {
        const size_t count = popup.items.size();
        const size_t rows = std::min(count, c_pickRows);
        popup.focus = std::min(popup.focus, count ? count - 1 : 0);
        popup.top = scrollTop(popup.top, popup.focus, count, rows);
        const int32_t noteHeight = popup.note.empty() ? 0 : popup.note.size() > c_noteWrap ? 34 : 20;
        const int32_t h = 26 + (int32_t)rows * c_pickHeight + 6 + noteHeight;
        lv_obj_t *panel = box(overlay, (c_width - c_pickWidth) / 2, (c_height - h) / 2, c_pickWidth, h, c_raised);
        outline(panel, c_scrollbar, 1, 6);
        label(panel, 10, 5, c_pickWidth - 20, font12, c_muted, popup.title.c_str());
        for (size_t i = popup.top; i < count && i < popup.top + rows; i++) {
            const bool focused = i == popup.focus;
            lv_obj_t *row = plain(panel, 4, 24 + (int32_t)(i - popup.top) * c_pickHeight, c_pickWidth - 14, c_pickHeight);
            if (focused) {
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
                focusFrame(row, true, 4);
            }
            label(row, focused ? 6 : 8, focused ? 2 : 4, c_pickWidth - 46, font14, c_text, popup.items[i].c_str());
            if ((int32_t)i == popup.current)
                lv_obj_set_style_radius(box(row, c_pickWidth - 32, focused ? 7 : 9, 6, 6, c_blue), 3, LV_PART_MAIN);
        }
        listScrollbar(panel, 24, popup.top, count, rows, c_pickHeight, c_pickWidth - 6);
        if (noteHeight)
            wrapped(panel, 10, 24 + (int32_t)rows * c_pickHeight + 5, c_pickWidth - 20, c_muted, popup.note.c_str());
        break;
    }
    case Menu::Edit: {
        char buf[16];
        const int32_t noteHeight = popup.note.empty() ? 0 : popup.note.size() > c_noteWrap + 4 ? 34 : 20;
        const int32_t h = 86 + noteHeight;
        lv_obj_t *panel = box(overlay, (c_width - c_editWidth) / 2, 34, c_editWidth, h, c_raised);
        outline(panel, c_scrollbar, 1, 6);
        label(panel, 10, 5, c_editWidth - 20, font12, c_muted, popup.title.c_str());
        lv_obj_t *field = box(panel, 8, 24, c_editWidth - 18, 30, c_ground);
        outline(field, c_focus, 2, 5);
        popup.caret = std::min(popup.caret, popup.text.size());
        // a password shows as stars, one for one, so the caret needs no translation
        std::string shown = popup.secret ? std::string(popup.text.size(), '*') : popup.text;
        if (popup.secret && popup.peek && popup.caret > 0)
            shown[popup.caret - 1] = popup.text[popup.caret - 1];
        lv_obj_t *input = textInput(field, shown.c_str(), popup.caret, popup.placeholder.c_str(), true);
        lv_obj_set_pos(input, 6, 4);
        lv_obj_set_width(input, c_editWidth - 34);
        label(panel, 10, 60, 150, font12, c_focusMuted, "OK: save   Back: cancel");
        snprintf(buf, sizeof(buf), "%u/%u", (unsigned)popup.text.size(), (unsigned)popup.maxLen);
        label(panel, c_editWidth - 80, 60, 70, font12, popup.text.size() >= popup.maxLen ? c_focus : c_muted, buf,
              LV_TEXT_ALIGN_RIGHT);
        if (noteHeight)
            wrapped(panel, 10, 80, c_editWidth - 20, c_muted, popup.note.c_str());
        break;
    }
    case Menu::Info: {
        const bool code = !popup.qr.empty();
        const int32_t w = 312, h = code ? 230 : 206;
        lv_obj_t *panel = box(overlay, (c_width - w) / 2, (c_height - h) / 2, w, h, c_raised);
        outline(panel, c_scrollbar, 1, 6);
        label(panel, 10, 6, w - 20, font14, c_text, popup.title.c_str());
        const bool chart = !popup.bars.empty();
        const size_t rows = code ? 10 : chart ? c_infoRows - 4 : c_infoRows;
        const size_t count = popup.lines.size();
        popup.top = count > rows ? std::min(popup.top, count - rows) : 0;
        int32_t x = 10, textWidth = w - 24;
        int32_t y = 30;
        if (chart) {
            // one bar per sample, oldest on the left
            lv_obj_t *area = box(panel, 10, 30, w - 24, 58, c_ground);
            lv_obj_set_style_radius(area, 4, LV_PART_MAIN);
            const int32_t pitch = (w - 32) / (int32_t)c_signalSamples;
            for (size_t i = 0; i < popup.bars.size() && i < c_signalSamples; i++) {
                const int32_t height = 4 + popup.bars[i] * 46 / 100;
                box(area, 4 + (int32_t)i * pitch, 54 - height, pitch - 2, height, popup.bars[i] >= 50 ? c_blue : c_focus);
            }
            y = 94;
        }
        if (code) {
            lv_obj_t *quiet = box(panel, 8, 28, c_qrSize + 16, c_qrSize + 16, 0xFFFFFF);
            lv_obj_t *qr = lv_qrcode_create(quiet);
            lv_qrcode_set_size(qr, c_qrSize);
            lv_qrcode_set_dark_color(qr, lv_color_black());
            lv_qrcode_set_light_color(qr, lv_color_white());
            lv_qrcode_update(qr, popup.qr.c_str(), (uint32_t)popup.qr.size());
            lv_obj_center(qr);
            x = c_qrSize + 32;
            textWidth = w - x - 8;
        }
        for (size_t i = popup.top; i < count && i < popup.top + rows; i++)
            label(panel, x, y + (int32_t)(i - popup.top) * c_infoPitch, textWidth, font12, c_text, popup.lines[i].c_str());
        listScrollbar(panel, y - 2, popup.top, count, rows, c_infoPitch, w - 6);
        label(panel, 10, h - 20, w - 20, font12, c_focusMuted,
              popup.what == Setting::NodeMenu ? "OK: actions   Back: close" : "Back: close");
        break;
    }
    default:
        break;
    }
}

/**
 * Choices made in the node, chat and quick reply menus; false passes the pick on to the settings.
 */
bool KeyboardView_320x240::applyMenuPick(Setting what, uint32_t value, uint32_t target)
{
    switch (what) {
    case Setting::NodeMenu: {
        const std::string name = chatName(target);
        meshtastic_AdminMessage msg{};
        switch (value) {
        case 1:
            openChat(target);
            break;
        case 2:
            openNodeDetail(target);
            break;
        case 3:
            showOnMap(target);
            break;
        case 4:
            startTraceRoute(target);
            break;
        case 5:
            startPing(target);
            break;
        case 6:
            if (favorites.count(target)) {
                msg.which_payload_variant = meshtastic_AdminMessage_remove_favorite_node_tag;
                msg.remove_favorite_node = target;
                favorites.erase(target);
                toast((name + " is no longer a favorite").c_str());
            } else {
                msg.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
                msg.set_favorite_node = target;
                favorites.insert(target);
                toast((name + " is a favorite now").c_str());
            }
            controller->sendAdminMessage(msg, ownNode);
            if (mapView == c_viewFavorites)
                placeMarker(target);
            break;
        case 7:
            if (ignoredNodes.count(target)) {
                msg.which_payload_variant = meshtastic_AdminMessage_remove_ignored_node_tag;
                msg.remove_ignored_node = target;
                ignoredNodes.erase(target);
                toast((name + " is heard again").c_str());
            } else {
                msg.which_payload_variant = meshtastic_AdminMessage_set_ignored_node_tag;
                msg.set_ignored_node = target;
                ignoredNodes.insert(target);
                toast((name + " is ignored").c_str());
            }
            controller->sendAdminMessage(msg, ownNode);
            break;
        case 9:
            // the radio keeps the flag and stops its own alerts for the node as well
            msg.which_payload_variant = meshtastic_AdminMessage_toggle_muted_node_tag;
            msg.toggle_muted_node = target;
            controller->sendAdminMessage(msg, ownNode);
            if (mutedNodes.erase(target)) {
                toast((name + " alerts again").c_str());
            } else {
                mutedNodes.insert(target);
                toast((name + " is muted").c_str());
            }
            break;
        case 10:
            openSignalHistory(target);
            break;
        case 11: {
            // the pointer page on the node: it follows the node when a newer position comes in
            const NodeRecord *node = nodeStore.find(target);
            if (!node || !node->position.hasCoordinates())
                break;
            const App from = !inApp() ? App::None : app == App::Pointer ? pointerFrom : app;
            openApp(App::Pointer);
            pointerPlace = false;
            pointerNode = target;
            pointerFrom = from;
            pointerChat = UINT32_MAX;
            break;
        }
        case 13: {
            std::vector<std::string> colors(std::begin(c_paletteNames), std::end(c_paletteNames));
            colors[0] = "Automatic";
            const auto set = nodeColors.find(target);
            openPicker(Setting::NodeColor, ("Color of " + name).c_str(), std::move(colors), {},
                       set == nodeColors.end() ? 0 : set->second, "Of its chip in the lists and its mark on the map", target);
            break;
        }
        case 14:
            if (!ownedNodes.erase(target)) {
                if (ownedNodes.size() >= 32) {
                    toast("32 nodes of one's own are the most");
                    break;
                }
                ownedNodes.insert(target);
            }
            toast((name + (ownedNodes.count(target) ? " is one of my nodes" : " is not one of my nodes")).c_str());
            savePrefs();
            dropMarker(target); // drawn again with or without the ring
            placeMarker(target);
            break;
        case 12:
            openPicker(Setting::NodeNavigate, ("To " + name).c_str(),
                       {"Straight line: the arrow", "By road, driving", "On foot, roads and trails"}, {0, 1, 2}, -1,
                       "Roads and trails need /geo/roads.bin on the card", target);
            break;
        case 8:
            openPicker(Setting::NodeRemove, ("Remove " + name + "?").c_str(), confirmItems("Remove the node"), {0, 1}, -1,
                       "It comes back when it is heard again", target);
            break;
        default:
            break;
        }
        return true;
    }
    case Setting::NodeColor:
        if (value == 0 || value >= c_paletteSize) {
            nodeColors.erase(target);
        } else if (nodeColors.count(target) || nodeColors.size() < 64) {
            nodeColors[target] = (uint8_t)value;
        } else {
            toast("64 nodes with a color are the most");
            return true;
        }
        savePrefs();
        dropMarker(target);
        placeMarker(target);
        return true;
    case Setting::NodeNavigate: {
        const NodeRecord *node = nodeStore.find(target);
        if (!node || !node->position.hasCoordinates())
            return true;
        if (value == 0)
            return applyMenuPick(Setting::NodeMenu, 11, target);
        navigateTo(node->position.latitude, node->position.longitude, chatName(target), value == 2 ? 1 : 0, target);
        return true;
    }
    case Setting::NodeRemove: {
        if (value != 1 || target == ownNode)
            return true;
        const std::string name = chatName(target);
        meshtastic_AdminMessage msg{};
        msg.which_payload_variant = meshtastic_AdminMessage_remove_by_nodenum_tag;
        msg.remove_by_nodenum = target;
        controller->sendAdminMessage(msg, ownNode);
        removeNode(target);
        toast((name + " removed").c_str());
        return true;
    }
    case Setting::ChatMenu: {
        const bool channel = target < c_max_channels;
        switch (value) {
        case 1:
            if (channel)
                toggleMute((uint8_t)target);
            else
                applyMenuPick(Setting::NodeMenu, 9, target);
            break;
        case 7:
            openSearch();
            break;
        case 6:
            if (channel)
                openPositionPicker((uint8_t)target);
            break;
        case 2:
            chatFor(target).unread = 0;
            break;
        case 3:
            if (channel)
                controller->removeTextMessages(ownNode, UINT32_MAX, (uint8_t)target);
            else
                controller->removeTextMessages(ownNode, target, 0);
            conversations.erase(target);
            if (!channel && pinned(target))
                setPinned(target, false); // a deleted chat does not stay on top of the list
            toast("Messages deleted");
            break;
        case 4: {
            openSettings(Page::Channels);
            const std::vector<SettingRow> rows = settingRows(Page::Channels);
            for (size_t i = 0; i < rows.size(); i++) {
                if (rows[i].id == Setting::Channel && rows[i].arg == target)
                    settingFocus = i;
            }
            break;
        }
        case 5:
            openNodeDetail(target);
            break;
        case 8:
            // the map with the nodes of this channel only
            if (channel) {
                setMapView((uint8_t)target);
                switchTab(Tab::Map);
            }
            break;
        case 9:
            setPinned(target, !pinned(target));
            break;
        case 12:
            openQuickReplies(target);
            break;
        case 10:
        case 11: {
            // the menu stays, so that a chat can be moved more than one place
            movePin(target, value == 10 ? -1 : 1);
            openChatMenu(target);
            const auto same = std::find(popup.values.begin(), popup.values.end(), value);
            if (menu == Menu::Pick && same != popup.values.end())
                popup.focus = (size_t)(same - popup.values.begin());
            break;
        }
        default:
            break;
        }
        return true;
    }
    case Setting::QuickReply: {
        const std::vector<std::string> replies = quickReplies();
        if (value == replies.size())
            openMecpComposer(target);
        else if (value == replies.size() + 1)
            openQuickList();
        if (value >= replies.size())
            return true;
        if (inConversation && convKey == target) {
            // what was being typed stays in the composer
            const std::string draft = chatFor(convKey).draft;
            const size_t at = caret;
            chatFor(convKey).draft = replies[value];
            sendDraft();
            chatFor(convKey).draft = draft;
            caret = std::min(at, draft.size());
        } else {
            // from the list of chats
            sendText(target, replies[value]);
            toast(("Sent to " + std::string(target < c_max_channels ? "# " : "") + chatName(target)).c_str());
        }
        return true;
    }
    default:
        return extraPick(what, value, target);
    }
}

// -------- nodes --------

uint32_t KeyboardView_320x240::focusedNode(void) const
{
    const std::vector<const NodeRecord *> nodes = visibleNodes();
    for (const NodeRecord *node : nodes) {
        if (node->id == nodesFocus)
            return node->id;
    }
    return nodes.empty() ? 0 : nodes.front()->id;
}

/**
 * Background and ink of a node's chip and marker: the color the user gave it, else the one its number gives.
 */
std::pair<uint32_t, uint32_t> KeyboardView_320x240::colorOf(uint32_t nodeNum)
{
    const auto set = nodeColors.find(nodeNum);
    if (set != nodeColors.end() && set->second > 0 && set->second < c_paletteSize)
        return {c_palette[set->second], inkOn(c_palette[set->second])};
    const auto [bg, fg] = nodeColor(nodeNum);
    return {bg, fg};
}

void KeyboardView_320x240::openNodeMenu(uint32_t nodeNum)
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    if (!node)
        return;
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    auto add = [&items, &values](const char *name, uint32_t value) {
        items.push_back(name);
        values.push_back(value);
    };
    const bool own = nodeNum == ownNode;
    if (!own)
        add("Message", 1);
    add("Details", 2);
    if (node->position.hasCoordinates())
        add("Show on map", 3);
    if (!own && node->position.hasCoordinates()) {
        add("Arrow to last reported position", 11);
        add("Navigate to last reported position", 12);
    }
    if (!own) {
        add("Trace route", 4);
        add("Ping", 5);
        add(favorites.count(nodeNum) ? "Remove from favorites" : "Add to favorites", 6);
        add(mutedNodes.count(nodeNum) ? "Unmute" : "Mute", 9);
        if (signalLog.count(nodeNum))
            add("Signal history", 10);
        add("Color", 13);
        add(ownedNodes.count(nodeNum) ? "Not one of my nodes" : "One of my nodes", 14);
        add(ignoredNodes.count(nodeNum) ? "Stop ignoring" : "Ignore", 7);
        add("Remove", 8);
    }
    openPicker(Setting::NodeMenu, chatName(nodeNum).c_str(), std::move(items), std::move(values), -1, "", nodeNum);
}

void KeyboardView_320x240::openNodeDetail(uint32_t nodeNum)
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    if (!node)
        return;
    const bool imperial = hasDisplay && display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
    char buf[80], ago[16], hops[16];
    std::vector<std::string> lines;

    snprintf(buf, sizeof(buf), "!%08x · %s · %s", (unsigned)nodeNum, shortName(nodeNum).c_str(), roleText(node->user.role));
    lines.push_back(buf);
    if (nodeNum == ownNode) {
        lines.push_back("This device");
    } else {
        agoString(node->lastHeard, now(), ago, sizeof(ago));
        hopString(*node, hops, sizeof(hops));
        if (ago[0])
            snprintf(buf, sizeof(buf), "Heard %s%s · %s", ago, strcmp(ago, "now") ? " ago" : "", hops);
        else
            snprintf(buf, sizeof(buf), "Not heard yet · %s", hops);
        lines.push_back(buf);
        if (node->hopsAway == 0 && !node->viaMqtt && (node->rssi != 0 || node->snr != 0.0f)) {
            snprintf(buf, sizeof(buf), "SNR %.1f dB · RSSI %d dBm", node->snr, (int)node->rssi);
            lines.push_back(buf);
        }
    }
    if (node->hasDeviceMetrics) {
        if (node->deviceMetrics.battery_level > 100)
            snprintf(buf, sizeof(buf), "Powered · %.2f V", node->deviceMetrics.voltage);
        else
            snprintf(buf, sizeof(buf), "Battery %u%% · %.2f V", (unsigned)node->deviceMetrics.battery_level,
                     node->deviceMetrics.voltage);
        lines.push_back(buf);
        snprintf(buf, sizeof(buf), "Channel use %.1f%% · air time %.1f%%", node->deviceMetrics.channel_utilization,
                 node->deviceMetrics.air_util_tx);
        lines.push_back(buf);
    }
    if (node->position.hasCoordinates()) {
        if (imperial)
            snprintf(buf, sizeof(buf), "%.5f, %.5f · %d ft", node->position.latitude * 1e-7, node->position.longitude * 1e-7,
                     (int)lround(node->position.altitude * 3.28084));
        else
            snprintf(buf, sizeof(buf), "%.5f, %.5f · %d m", node->position.latitude * 1e-7, node->position.longitude * 1e-7,
                     (int)node->position.altitude);
        lines.push_back(buf);
        const std::string distance = distanceTo(*node);
        if (!distance.empty())
            lines.push_back(distance + " from here");
        const std::string town = townOf(node->position.latitude, node->position.longitude);
        if (!town.empty())
            lines.push_back("In " + town);
    } else {
        lines.push_back("No position shared");
    }
    if (nodeNum != ownNode) {
        // the channels something of this node was decoded on; the map views go by this
        const auto heard = heardOn.find(nodeNum);
        std::string on;
        for (uint8_t i = 0; heard != heardOn.end() && i < c_max_channels; i++) {
            if ((heard->second & (1u << i)) && channelInfo[i].enabled)
                on += (on.empty() ? "Heard on #" : ", #") + chatName(i);
        }
        if (!on.empty())
            lines.push_back(on);
    }
    std::string flags;
    auto flag = [&flags](bool set, const char *name) {
        if (set)
            flags += (flags.empty() ? "" : " · ") + std::string(name);
    };
    flag(favorites.count(nodeNum), "Favorite");
    flag(ignoredNodes.count(nodeNum), "Ignored");
    flag(node->viaMqtt, "Via MQTT");
    flag(node->unmessagable, "Takes no messages");
    flag(node->hasBadKey, "Key mismatch");
    flag(node->user.is_licensed, "Licensed operator");
    if (!flags.empty())
        lines.push_back(flags);

    openInfo(chatName(nodeNum).c_str(), std::move(lines));
    popup.what = Setting::NodeMenu;
    popup.target = nodeNum;
}

/**
 * Distance and compass direction from this device, empty while either end has no position.
 */
std::string KeyboardView_320x240::distanceTo(const NodeRecord &node) const
{
    const NodeRecord *own = nodeStore.find(ownNode);
    if (!own || own->id == node.id || !own->position.hasCoordinates() || !node.position.hasCoordinates())
        return "";
    const geo::Leg leg =
        geo::leg(own->position.latitude, own->position.longitude, node.position.latitude, node.position.longitude);
    char distance[16], buf[32];
    geo::distanceText(leg.meters, imperialUnits(), distance, sizeof(distance));
    snprintf(buf, sizeof(buf), "%s %s", distance, geo::cardinal(leg.bearing));
    return buf;
}

void KeyboardView_320x240::updateNodeFlags(uint32_t nodeNum, bool favorite, bool ignored, bool muted)
{
    if (muted)
        mutedNodes.insert(nodeNum);
    else
        mutedNodes.erase(nodeNum);
    if (favorite != (favorites.count(nodeNum) > 0)) {
        if (favorite)
            favorites.insert(nodeNum);
        else
            favorites.erase(nodeNum);
        if (mapView == c_viewFavorites)
            placeMarker(nodeNum);
    }
    if (ignored)
        ignoredNodes.insert(nodeNum);
    else
        ignoredNodes.erase(nodeNum);
    dirty = true;
}

// -------- requests to a node --------

void KeyboardView_320x240::startTraceRoute(uint32_t nodeNum)
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    if (!node || nodeNum == ownNode)
        return;
    if (traceRequest)
        requests.removeRequest(traceRequest, ResponseHandler::TraceRouteRequest);
    traceRequest =
        requests.addRequest(nodeNum, ResponseHandler::TraceRouteRequest, (void *)(uintptr_t)nodeNum,
                            [this](const ResponseHandler::Request &req, ResponseHandler::EventType event, int32_t) {
                                if (event != ResponseHandler::timeout)
                                    return;
                                const uint32_t target = (uint32_t)(uintptr_t)req.cookie;
                                traceRequest = 0;
                                if (!traceInfo(target, {"No answer within 60 seconds.", "",
                                                        "The node may be off or out of reach, or it",
                                                        "does not answer trace routes."}))
                                    toast((chatName(target) + ": no trace route answer").c_str(), 3000);
                            });
    controller->traceRoute(nodeNum, node->channel, lora.hop_limit ? (uint8_t)lora.hop_limit : 3, traceRequest);
    // the answer takes its time and may never come: a popup that stays says so, a toast is gone after 2 seconds
    closeMenu();
    char hops[40];
    snprintf(hops, sizeof(hops), "Sent with up to %u hops.", (unsigned)(lora.hop_limit ? lora.hop_limit : 3));
    if (!traceInfo(nodeNum, {"Asking for the route.", hops, "", "Waiting for the answer, up to 60 seconds.",
                             "Back: go on, the answer shows when it comes"}))
        toast(("Tracing the route to " + chatName(nodeNum)).c_str());
}

/**
 * What a trace route has to say goes into its popup: when that is still up, or when nothing else is in the
 * way. False when somebody is typing or choosing; then a toast has to do.
 */
bool KeyboardView_320x240::traceInfo(uint32_t nodeNum, std::vector<std::string> &&lines)
{
    if (!traceShown && (menu != Menu::None || inConversation))
        return false;
    openInfo(("Route to " + chatName(nodeNum)).c_str(), std::move(lines));
    traceShown = true;
    return true;
}

/**
 * A ping is a position request: any answer shows that the node is reachable.
 */
void KeyboardView_320x240::startPing(uint32_t nodeNum)
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    if (!node || nodeNum == ownNode)
        return;
    if (pingRequest)
        requests.removeRequest(pingRequest, ResponseHandler::PositionRequest);
    pingTarget = nodeNum;
    pingRequest = requests.addRequest(nodeNum, ResponseHandler::PositionRequest, (void *)(uintptr_t)nodeNum,
                                      [this](const ResponseHandler::Request &req, ResponseHandler::EventType event, int32_t) {
                                          if (event != ResponseHandler::timeout)
                                              return;
                                          pingTarget = pingRequest = 0;
                                          toast((chatName((uint32_t)(uintptr_t)req.cookie) + " did not answer").c_str(), 3000);
                                      });
    controller->requestPosition(nodeNum, node->channel, pingRequest);
    toast(("Pinging " + chatName(nodeNum)).c_str());
}

void KeyboardView_320x240::handleResponse(uint32_t from, uint32_t id, const meshtastic_RouteDiscovery &route)
{
    const ResponseHandler::Request req = requests.findRequest(id, ResponseHandler::TraceRouteRequest);
    if (req.type != ResponseHandler::TraceRouteRequest)
        return;
    requests.removeRequest(id, ResponseHandler::TraceRouteRequest);
    if (id == traceRequest)
        traceRequest = 0;
    const uint32_t target = (uint32_t)(uintptr_t)req.cookie;

    auto hop = [this](const char *lead, uint32_t nodeNum, const int8_t *snr, pb_size_t snrCount, size_t index) {
        char buf[80];
        const std::string name = nodeNum == UINT32_MAX ? "Unknown" : nodeNum == ownNode ? "You" : chatName(nodeNum);
        if (index < snrCount && snr[index] != INT8_MIN)
            snprintf(buf, sizeof(buf), "%s%s · %.1f dB", lead, name.c_str(), snr[index] / 4.0f);
        else
            snprintf(buf, sizeof(buf), "%s%s", lead, name.c_str());
        return std::string(buf);
    };

    std::vector<std::string> lines;
    lines.push_back("You");
    for (pb_size_t i = 0; i < route.route_count; i++)
        lines.push_back(hop("> ", route.route[i], route.snr_towards, route.snr_towards_count, i));
    lines.push_back(hop("> ", target, route.snr_towards, route.snr_towards_count, route.route_count));
    if (route.route_back_count || route.snr_back_count) {
        lines.push_back("");
        lines.push_back("Way back:");
        lines.push_back(chatName(target));
        for (pb_size_t i = 0; i < route.route_back_count; i++)
            lines.push_back(hop("> ", route.route_back[i], route.snr_back, route.snr_back_count, i));
        lines.push_back(hop("> ", ownNode, route.snr_back, route.snr_back_count, route.route_back_count));
    }

    if (!traceInfo(target, std::move(lines))) {
        // do not take the screen away from someone typing or choosing
        char buf[80];
        snprintf(buf, sizeof(buf), "%s: %u hop%s away", chatName(target).c_str(), (unsigned)route.route_count + 1,
                 route.route_count ? "s" : "");
        toast(buf, 4000);
    }
}

void KeyboardView_320x240::handleResponse(uint32_t from, uint32_t id, const meshtastic_Routing &routing,
                                          const meshtastic_MeshPacket &p)
{
    if (routing.which_variant == meshtastic_Routing_route_reply_tag) {
        handleResponse(from, id, routing.route_reply);
        return;
    }
    const ResponseHandler::Request req = requests.findRequest(id);
    const bool failed =
        routing.which_variant == meshtastic_Routing_error_reason_tag && routing.error_reason != meshtastic_Routing_Error_NONE;
    const uint32_t target = (uint32_t)(uintptr_t)req.cookie;

    // text message requests stay: the delivery state of a message comes through textMessageStatus()
    if (req.type == ResponseHandler::TraceRouteRequest) {
        if (!failed) {
            // an acknowledgment is not the route, but it tells how far the request got
            if (traceShown && id == traceRequest && routing.which_variant == meshtastic_Routing_error_reason_tag)
                traceInfo(target, {"Asking for the route.",
                                   from == target     ? "The node has the request."
                                   : from == ownNode ? "Sent; a neighbor passed it on."
                                                     : "Sent; the mesh passed it on.",
                                   "", "Waiting for the answer, up to 60 seconds.",
                                   "Back: go on, the answer shows when it comes"});
            return;
        }
        requests.removeRequest(id, ResponseHandler::TraceRouteRequest);
        traceRequest = 0;
        const char *why = "The radio reports an error.";
        switch (routing.error_reason) {
        case meshtastic_Routing_Error_MAX_RETRANSMIT:
            why = "Nobody passed it on: no neighbor heard it.";
            break;
        case meshtastic_Routing_Error_NO_CHANNEL:
            why = "No channel in common with that node.";
            break;
        case meshtastic_Routing_Error_NO_RESPONSE:
            why = "The node got it and does not answer.";
            break;
        case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
            why = "The radio has used up its airtime.";
            break;
        case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
            why = "Too many requests: wait 30 seconds.";
            break;
        default:
            break;
        }
        char code[32];
        snprintf(code, sizeof(code), "(routing error %u)", (unsigned)routing.error_reason);
        if (!traceInfo(target, {"The trace route failed.", why, code}))
            toast((chatName(target) + ": trace route failed").c_str(), 3000);
    } else if (req.type == ResponseHandler::PositionRequest) {
        if (!failed && from != target)
            return; // only relayed so far
        requests.removeRequest(id, ResponseHandler::PositionRequest);
        pingTarget = pingRequest = 0;
        toast((chatName(target) + (failed ? " did not answer" : " is reachable")).c_str(), 3000);
    }
}

void KeyboardView_320x240::handlePositionResponse(uint32_t from, uint32_t request_id, int32_t rx_rssi, float rx_snr,
                                                  bool isNeighbor)
{
    const ResponseHandler::Request req = requests.findRequest(request_id, ResponseHandler::PositionRequest);
    if (req.type != ResponseHandler::PositionRequest)
        return;
    requests.removeRequest(request_id, ResponseHandler::PositionRequest);
    pingTarget = pingRequest = 0;
    char buf[96];
    if (isNeighbor)
        snprintf(buf, sizeof(buf), "%s answered: SNR %.1f dB, RSSI %d", chatName(from).c_str(), rx_snr, (int)rx_rssi);
    else
        snprintf(buf, sizeof(buf), "%s answered over the mesh", chatName(from).c_str());
    toast(buf, 4000);
}

// -------- chats --------

void KeyboardView_320x240::openChatMenu(uint32_t key)
{
    if (key == UINT32_MAX)
        return;
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    auto add = [&items, &values](const char *name, uint32_t value) {
        items.push_back(name);
        values.push_back(value);
    };
    const auto chat = conversations.find(key);
    const bool unread = chat != conversations.end() && chat->second.unread;
    const bool any = chat != conversations.end() && !chat->second.messages.empty();
    // a pinned chat can be moved among the pinned ones
    const std::vector<uint32_t> top = pinnedChats();
    const auto place = std::find(top.begin(), top.end(), key);
    auto addPin = [this, &add, &top, place, key](void) {
        if (place == top.end()) {
            // a channel that is switched off is only listed for its old messages
            if (key >= c_max_channels || channelInfo[key].enabled)
                add("Pin to top", 9);
            return;
        }
        add("Unpin", 9);
        if (place != top.begin())
            add("Move up", 10);
        if (place + 1 != top.end())
            add("Move down", 11);
    };
    if (key < c_max_channels) {
        if (unread)
            add("Mark as read", 2);
        add("Quick reply", 12);
        addPin();
        add(channelInfo[key].muted ? "Unmute" : "Mute", 1);
        add(("Position: " + precisionName(channelInfo[key].precision)).c_str(), 6);
        add("Show on the map", 8);
        add("Channel settings", 4);
        add("Search messages", 7);
        if (any)
            add("Delete the messages", 3);
        openPicker(Setting::ChatMenu, ("# " + chatName(key)).c_str(), std::move(items), std::move(values), -1, "", key);
    } else {
        if (unread)
            add("Mark as read", 2);
        add("Quick reply", 12);
        addPin();
        add(mutedNodes.count(key) ? "Unmute" : "Mute", 1);
        if (nodeStore.find(key))
            add("Node details", 5);
        add("Search messages", 7);
        add("Delete the chat", 3);
        openPicker(Setting::ChatMenu, chatName(key).c_str(), std::move(items), std::move(values), -1, "", key);
    }
}

// -------- pinned chats --------

/**
 * How a pinned chat is written down: a channel by its name, since its slot may change, a channel without a name
 * by its slot, a direct chat by node number.
 */
std::string KeyboardView_320x240::pinName(uint32_t key) const
{
    if (key < c_max_channels)
        return channelInfo[key].name.empty() ? "=" + std::to_string(key) : "#" + channelInfo[key].name;
    char buf[12];
    snprintf(buf, sizeof(buf), "!%08x", (unsigned)key);
    return buf;
}

/**
 * The chat a pin stands for, UINT32_MAX while there is none: its channel may not have arrived yet.
 */
uint32_t KeyboardView_320x240::pinKey(const std::string &pin) const
{
    if (pin.size() < 2)
        return UINT32_MAX;
    if (pin[0] == '#') {
        for (uint32_t i = 0; i < c_max_channels; i++) {
            if (channelInfo[i].enabled && !pin.compare(1, std::string::npos, channelInfo[i].name))
                return i;
        }
        return UINT32_MAX;
    }
    if (pin[0] == '=') {
        const uint32_t slot = (uint32_t)(pin[1] - '0');
        return slot < c_max_channels && channelInfo[slot].enabled && channelInfo[slot].name.empty() ? slot : UINT32_MAX;
    }
    const uint32_t node = (uint32_t)strtoul(pin.c_str() + 1, nullptr, 16);
    return node < c_max_channels ? UINT32_MAX : node; // the low numbers are channels
}

/**
 * The pinned chats there are right now, in their order. A pin without its chat is skipped and kept.
 */
std::vector<uint32_t> KeyboardView_320x240::pinnedChats(void) const
{
    std::vector<uint32_t> keys;
    for (const std::string &pin : pins) {
        const uint32_t key = pinKey(pin);
        if (key != UINT32_MAX && std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    }
    return keys;
}

bool KeyboardView_320x240::pinned(uint32_t key) const
{
    const std::vector<uint32_t> keys = pinnedChats();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

/**
 * A new pin goes below the ones there are.
 */
void KeyboardView_320x240::setPinned(uint32_t key, bool on)
{
    if (on && key < c_max_channels && !channelInfo[key].enabled)
        return;
    const std::string name = pinName(key);
    const auto it = std::find(pins.begin(), pins.end(), name);
    if (on && it != pins.end()) {
        toast("A channel of that name is pinned"); // two channels of one name share the pin of the first
        return;
    }
    if (on && pins.size() >= c_maxPins) {
        toast("No room for another pinned chat");
        return;
    }
    if (on)
        pins.push_back(name);
    else if (it != pins.end())
        pins.erase(it);
    else
        return;
    savePrefs();
    dirty = true;
}

/**
 * One place up or down among the pinned chats that are on screen.
 */
void KeyboardView_320x240::movePin(uint32_t key, int delta)
{
    const std::vector<uint32_t> keys = pinnedChats();
    const auto at = std::find(keys.begin(), keys.end(), key);
    if (at == keys.end())
        return;
    const int to = (int)(at - keys.begin()) + delta;
    if (to < 0 || to >= (int)keys.size())
        return;
    const auto a = std::find(pins.begin(), pins.end(), pinName(key));
    const auto b = std::find(pins.begin(), pins.end(), pinName(keys[(size_t)to]));
    if (a == pins.end() || b == pins.end())
        return;
    std::iter_swap(a, b);
    savePrefs();
    dirty = true;
}

/**
 * The channel in this slot changed its name or went away. What becomes of its pin is decided in pinsTask().
 */
void KeyboardView_320x240::channelChanged(uint8_t index, const std::string &oldName, bool same)
{
    const std::string pin = oldName.empty() ? "=" + std::to_string(index) : "#" + oldName;
    if (std::find(pins.begin(), pins.end(), pin) == pins.end())
        return;
    pinChanges.emplace_back(pin, same ? pinName(index) : "");
    pinChangeTime = lv_tick_get();
}

/**
 * Once the channels have settled: a pin whose channel is still there, in whatever slot, stays; it follows a new
 * name of the same channel and goes with a channel that is gone.
 */
void KeyboardView_320x240::pinsTask(void)
{
    if (pinChanges.empty() || lv_tick_elaps(pinChangeTime) < c_pinSettleTime)
        return;
    bool changed = false;
    for (const auto &[pin, renamed] : pinChanges) {
        const auto it = std::find(pins.begin(), pins.end(), pin);
        if (it == pins.end() || pinKey(pin) != UINT32_MAX)
            continue;
        if (!renamed.empty() && pinKey(renamed) != UINT32_MAX && std::find(pins.begin(), pins.end(), renamed) == pins.end())
            *it = renamed;
        else
            pins.erase(it);
        changed = true;
    }
    pinChanges.clear();
    if (changed) {
        savePrefs();
        dirty = true;
    }
}

/**
 * The quick replies: the user's own once they changed the list, the built-in ones until then.
 */
std::vector<std::string> KeyboardView_320x240::quickReplies(void) const
{
    if (prefs.quickOwn)
        return prefs.quick;
    return std::vector<std::string>(std::begin(c_quickReplies), std::end(c_quickReplies));
}

void KeyboardView_320x240::openQuickReplies(uint32_t key)
{
    if (key == UINT32_MAX) {
        if (!inConversation)
            return;
        key = convKey;
    }
    std::vector<std::string> items = quickReplies();
    items.push_back("Emergency message (MECP) ..."); // not a reply: opens the four steps that write one
    items.push_back("Change this list ...");
    const bool open = inConversation && convKey == key;
    openPicker(Setting::QuickReply, ("Quick reply to " + std::string(key < c_max_channels ? "# " : "") + chatName(key)).c_str(),
               std::move(items), {}, -1, open ? "Sent right away; your draft stays" : "Sent right away", key);
}

/**
 * The quick replies as a list to change: OK on one offers to rewrite, move or remove it.
 */
void KeyboardView_320x240::openQuickList(int32_t focus)
{
    std::vector<std::string> items = quickReplies();
    std::vector<uint32_t> values;
    for (size_t i = 0; i < items.size(); i++)
        values.push_back((uint32_t)i);
    if (items.size() < c_maxQuick) {
        items.push_back("Add a reply ...");
        values.push_back(1000);
    }
    if (prefs.quickOwn) {
        items.push_back("Back to the built-in list");
        values.push_back(1001);
    }
    openPicker(Setting::QuickList, "Quick replies", std::move(items), std::move(values), -1, "OK: change, move or remove");
    if (menu == Menu::Pick && focus >= 0 && (size_t)focus < popup.values.size())
        popup.focus = (size_t)focus;
}

#endif
