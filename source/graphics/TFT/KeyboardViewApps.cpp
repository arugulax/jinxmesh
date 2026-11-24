#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/common/LoRaPresets.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "util/GeoMath.h"
#include "util/ILog.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace kbview;

/**
 * What stands on top of the Home tab: the tiles of Home and their catalog, the Home manager that
 * arranges them, and the list of apps.
 */

namespace
{

struct TileName {
    const char *key;  // in the preferences file
    const char *name; // on screen
};

// in the order of KeyboardView_320x240::Tile
const TileName c_tiles[] = {{"time", "Time"},           {"unread", "Unread"},   {"radio", "Radio"},
                            {"gps", "GPS"},             {"battery", "Battery"}, {"nodes", "Nodes"},
                            {"nearby", "Nearby"},       {"airtime", "Airtime"}, {"town", "Town"},
                            {"trip", "Trip"},           {"mecp", "MECP"},       {"compass", "Compass"},
                            {"wardrive", "Wardrive"},   {"stopwatch", "Stopwatch"}, {"timer", "Timer"},
                            {"navigate", "Navigate"},   {"empty", "Empty"}};

constexpr int32_t c_tileWidth = 74;
constexpr int32_t c_tileHeight = 58;
constexpr int32_t c_tileGap = 4;
constexpr int32_t c_tileRowGap = 5;

const char *tileName(uint8_t tile)
{
    return tile < sizeof(c_tiles) / sizeof(c_tiles[0]) ? c_tiles[tile].name : "";
}

} // namespace

// -------- Home tiles --------

/**
 * What a tile says right now. A tile whose data is missing says so in plain words.
 */
KeyboardView_320x240::TileView KeyboardView_320x240::tileView(Tile tile)
{
    TileView view;
    char buf[64];
    const uint32_t time = now();
    const NodeRecord *own = nodeStore.find(ownNode);

    switch (tile) {
    case Tile::Time:
        view.value = "--:--";
        view.sub = "No time yet";
        if (validTime(time)) {
            time_t t = time;
            struct tm tm;
            localtime_r(&t, &tm);
            clockString(tm, buf, sizeof(buf), twelveHour());
            view.value = buf;
            dateString(tm, buf, sizeof(buf), true);
            view.sub = buf;
        }
        break;
    case Tile::Unread: {
        const uint32_t unread = totalUnread();
        view.value = std::to_string(unread);
        view.sub = "No new messages";
        view.accent = unread > 0;
        if (!unread)
            break;
        const Chat *newest = nullptr;
        uint32_t newestKey = 0;
        for (const auto &[key, chat] : conversations) {
            if (chat.unread && !chat.messages.empty() && (!newest || chat.lastTime > newest->lastTime)) {
                newest = &chat;
                newestKey = key;
            }
        }
        if (newest) {
            auto msg = std::find_if(newest->messages.rbegin(), newest->messages.rend(), [](const Message &m) { return !m.own; });
            if (msg != newest->messages.rend())
                view.sub =
                    (newestKey < c_max_channels ? shortName(msg->from) : chatName(newestKey)) + ": " + oneLine(msg->text.c_str());
        }
        break;
    }
    case Tile::Radio:
        view.value = "--";
        if (hasLora && lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            // without a region the radio neither sends nor listens
            view.value = "No region";
            view.sub = "OK: choose a region";
        } else if (hasLora) {
            if (!lora.use_preset)
                view.value = "Custom";
            else if (lora.modem_preset <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX)
                view.value = LoRaPresets::modemPresetToString(lora.modem_preset);
            // the long names do not fit a tile
            for (const auto &[full, brief] : {std::pair<const char *, const char *>{"Medium", "Med"}, {"Moderate", "Mod"}}) {
                const size_t at = view.value.find(full);
                if (at != std::string::npos)
                    view.value.replace(at, strlen(full), brief);
            }
            if (hasSnr)
                snprintf(buf, sizeof(buf), "Slot %u · %.0f dB", (unsigned)activeSlot(), lastSnr);
            else
                snprintf(buf, sizeof(buf), "Slot %u", (unsigned)activeSlot());
            view.sub = buf;
        }
        break;
    case Tile::Gps:
        view.value = "No fix";
        view.sub = "Waiting for GPS";
        if (own && own->position.hasCoordinates()) {
            view.value = "Fix";
            if (own->position.satellites) {
                snprintf(buf, sizeof(buf), "%u satellites", (unsigned)own->position.satellites);
                view.sub = buf;
            } else {
                view.sub = "Position known";
            }
        }
        break;
    case Tile::Battery:
        view.value = "--";
        if (own && own->hasDeviceMetrics) {
            if (own->deviceMetrics.battery_level > 100) {
                view.value = "Power";
            } else {
                snprintf(buf, sizeof(buf), "%u%%", (unsigned)own->deviceMetrics.battery_level);
                view.value = buf;
            }
            snprintf(buf, sizeof(buf), "%.2f V", own->deviceMetrics.voltage);
            view.sub = buf;
        }
        break;
    case Tile::Nodes: {
        size_t total = 0, online = 0;
        for (const auto &[id, node] : nodeStore.records()) {
            if (id == ownNode)
                continue;
            total++;
            if (node.lastHeard && time - node.lastHeard <= c_onlineSecs)
                online++;
        }
        view.value = std::to_string(online);
        snprintf(buf, sizeof(buf), "of %u known", (unsigned)total);
        view.sub = buf;
        break;
    }
    case Tile::Mecp: {
        const std::string state = appState(App::Mecp);
        view.value = state.empty() ? "0" : state.substr(0, state.find(' '));
        view.sub = "Emergency";
        view.accent = !state.empty();
        break;
    }
    case Tile::Compass: {
        float heading = 0;
        view.value = "--";
        view.sub = "OK: open";
        if (compassHeading(heading)) {
            snprintf(buf, sizeof(buf), "%.0f°", heading);
            view.value = buf;
            view.sub = geo::cardinal(heading);
        }
        break;
    }
    case Tile::Wardrive:
        view.value = prefs.wardrive ? std::to_string(wardriveRows) : "Off";
        view.sub = prefs.wardrive ? "rows logged" : "OK: open";
        view.accent = prefs.wardrive;
        break;
    case Tile::Stopwatch:
        return clockTile(false);
    case Tile::Timer:
        return clockTile(true);
    case Tile::Navigate:
        return routeTile();
    case Tile::Nearby:
        return nearbyTile();
    case Tile::Airtime:
        return airtimeTile();
    case Tile::Town:
        return townTile();
    case Tile::Trip:
        return tripTile();
    default:
        break;
    }
    return view;
}

/**
 * OK on a tile.
 */
void KeyboardView_320x240::openTile(Tile tile)
{
    switch (tile) {
    case Tile::Time:
        openSettings(Page::Device, Setting::TimeZone);
        break;
    case Tile::Unread:
        openUnread();
        break;
    case Tile::Radio:
        openSettings(Page::Radio, Setting::Region);
        break;
    case Tile::Gps:
        switchTab(Tab::Map);
        break;
    case Tile::Battery:
        if (nodeStore.find(ownNode))
            openNodeDetail(ownNode);
        else
            toast("Nothing known about this device yet");
        break;
    case Tile::Nodes:
        switchTab(Tab::Nodes);
        break;
    case Tile::Mecp:
        openApp(App::Mecp);
        break;
    case Tile::Compass:
        openApp(App::Compass);
        break;
    case Tile::Wardrive:
        openApp(App::Wardrive);
        break;
    case Tile::Stopwatch:
        openApp(App::Stopwatch);
        break;
    case Tile::Timer:
        openApp(App::Timer);
        break;
    case Tile::Navigate:
        openApp(App::Navigate);
        break;
    case Tile::Empty:
        openApp(App::Arrange); // on the place that is free
        break;
    case Tile::Nearby:
        openApp(App::Nearby);
        break;
    case Tile::Airtime:
        openApp(App::Radio);
        break;
    case Tile::Town:
        openApp(App::Town);
        break;
    case Tile::Trip:
        openApp(App::Trip);
        break;
    default:
        break;
    }
}

/**
 * The grid of four by three, on Home and in the Home manager. A tile that is being carried stands out of the row.
 */
void KeyboardView_320x240::renderTiles(int32_t focus, bool carried)
{
    for (size_t i = 0; i < c_homeSlots; i++) {
        const TileView view = tileView(homeTiles[i]);
        const bool focused = (int32_t)i == focus, lifted = focused && carried;
        const bool empty = homeTiles[i] == Tile::Empty;
        lv_obj_t *tile = box(content, 6 + (int32_t)(i % c_homeColumns) * (c_tileWidth + c_tileGap) - (lifted ? 2 : 0),
                             5 + (int32_t)(i / c_homeColumns) * (c_tileHeight + c_tileRowGap) - (lifted ? 3 : 0), c_tileWidth,
                             c_tileHeight, empty ? c_ground : c_surface);
        if (empty && !focused)
            outline(tile, c_border, 1, 5);
        focusFrame(tile, focused, 5);
        // a tile with a color: a border of it, and the tile itself in a shade of it
        const uint8_t color = empty ? 0 : tileColors[(size_t)homeTiles[i]];
        if (color > 0 && color < c_paletteSize) {
            lv_obj_set_style_bg_color(tile, lv_color_hex(mixColor(focused ? c_focusTint : c_surface, c_palette[color], 22)),
                                      LV_PART_MAIN);
            if (!focused)
                outline(tile, c_palette[color], 2, 5);
        }
        if (lifted) {
            lv_obj_set_style_shadow_width(tile, 8, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(tile, lv_color_hex(c_focus), LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(tile, LV_OPA_50, LV_PART_MAIN);
        }
        if (empty)
            continue;
        const int32_t room = c_tileWidth - 8;
        label(tile, 4, 2, room, font12, focused ? c_focusMuted : c_muted, tileName((uint8_t)homeTiles[i]));
        // long values such as preset names step down a size instead of being cut
        const size_t length = view.value.size();
        const lv_font_t *valueFont = length > 7 ? font12 : length > 5 ? font14 : font16;
        lv_obj_t *value = label(tile, 4, length > 7 ? 21 : length > 5 ? 19 : 17, room, valueFont, view.accent ? c_blue : c_text,
                                view.value.c_str());
        lv_obj_set_height(value, lv_font_get_line_height(valueFont));
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_t *sub = label(tile, 4, 39, room, font12, focused ? c_focusMuted : c_muted, view.sub.c_str());
        lv_obj_set_height(sub, lv_font_get_line_height(font12));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    }
}

/**
 * The tiles of Home from their line in the preferences. A name that is not known, or that came before in the
 * line, leaves its slot to the default layout; no tile stands on Home twice.
 */
void KeyboardView_320x240::setHomeTiles(const std::string &line)
{
    std::array<bool, (size_t)Tile::Count> used{};
    std::array<bool, c_homeSlots> set{};
    size_t at = 0;
    for (size_t slot = 0; slot < c_homeSlots && at <= line.size(); slot++) {
        size_t end = line.find(',', at);
        if (end == std::string::npos)
            end = line.size();
        const std::string name = line.substr(at, end - at);
        at = end + 1;
        for (uint8_t tile = 0; tile < (uint8_t)Tile::Count; tile++) {
            if (name == c_tiles[tile].key && (!used[tile] || (Tile)tile == Tile::Empty) && !set[slot]) {
                homeTiles[slot] = (Tile)tile;
                used[tile] = set[slot] = true;
            }
        }
    }
    for (size_t slot = 0; slot < c_homeSlots; slot++) {
        if (set[slot])
            continue;
        uint8_t tile = (uint8_t)c_defaultHome[slot];
        // the default of the slot may stand elsewhere by now: then the place stays free
        if (used[tile])
            tile = (uint8_t)Tile::Empty;
        homeTiles[slot] = (Tile)tile;
        used[tile] = true;
    }
}

std::string KeyboardView_320x240::homeLine(void) const
{
    std::string line;
    for (size_t slot = 0; slot < c_homeSlots; slot++) {
        if (slot)
            line += ',';
        line += c_tiles[(uint8_t)homeTiles[slot]].key;
    }
    return line;
}

// -------- screens on top of Home --------

/**
 * OK held on Home. The Apps tile can be taken off Home, this menu cannot.
 */
void KeyboardView_320x240::openHomeMenu(void)
{
    const Tile tile = homeTiles[std::min<size_t>(homeFocus, c_homeSlots - 1)];
    std::vector<std::string> items = {"Arrange tiles", "Apps"};
    std::vector<uint32_t> values = {1, 0};
    if (tile != Tile::Empty) {
        items.insert(items.begin(), std::string("Color of ") + tileName((uint8_t)tile));
        values.insert(values.begin(), 2);
    }
    openPicker(Setting::HomeMenu, "Home", std::move(items), std::move(values), -1, "", (uint32_t)tile);
}

void KeyboardView_320x240::openApp(App which, bool fromList)
{
    closeMenu();
    if (which == App::List) {
        switchTab(Tab::Apps); // the list is a tab of its own
        dirty = urgent = true;
        return;
    }
    inConversation = false;
    barFocus = false;
    if (tab != Tab::Home && tab != Tab::Apps) {
        trail.push_back(tab);
        if (trail.size() > c_maxTrail)
            trail.erase(trail.begin());
        tab = Tab::Home;
    }
    app = which;
    appFromList = tab == Tab::Apps;
    carrying = false;
    if (which == App::Arrange)
        arrangeFocus = std::min<size_t>(homeFocus, c_homeSlots - 1);
    if (which == App::Nearby)
        nearbyFilter.clear();
    if (which == App::Mecp)
        mecpListFocus = mecpListTop = 0;
    if (which == App::Town) {
        townSearch.clear();
        townHits.clear();
        townFocus = townTop = 0;
        if (towns.state() != TownIndex::State::Ok)
            townsTried = 0; // look for the file again: the card may have been put in since
    }
    if (which == App::Radio) {
        radioFocus = radioTop = 0;
        statsAsked = lv_tick_get() - 60000; // ask on the next round of the task
    }
    dirty = urgent = true;
}

/**
 * Back: first out of what is going on in the screen, then to where it was opened from.
 */
void KeyboardView_320x240::closeApp(void)
{
    if (app == App::Arrange && carrying) {
        carrying = false; // the tile stays where it was carried to
        return;
    }
    if (nearbyBack() || mecpBack() || townBack() || routeBack())
        return;
    if (app == App::Timer)
        timerRinging = false; // leaving the notice is as good as a key on it
    app = App::None; // the tiles of Home, or the list of the Apps tab
    appFromList = false;
}

const char *KeyboardView_320x240::appTitle(void) const
{
    switch (app) {
    case App::Arrange:
        return "Arrange tiles";
    case App::Nearby:
        return "Nearby";
    case App::Pointer:
        return "Pointer";
    case App::Radio:
        return "RF monitor";
    case App::Mecp:
        return "Emergency messages";
    case App::MecpNew:
        return mecpDrill ? "DRILL message" : "Emergency message";
    case App::Town:
        return "Town locator";
    case App::Trip:
        return "Trip";
    case App::Wardrive:
        return "Wardrive";
    case App::Compass:
        return "Compass";
    case App::Stopwatch:
        return "Stopwatch";
    case App::Timer:
        return "Timer";
    case App::Navigate:
        return "Navigate";
    default:
        return "Apps";
    }
}

namespace
{

struct AppRow {
    uint8_t app; // KeyboardView_320x240::App, which is not open to this file's helpers
    const char *name;
    const char *about; // shown while the app has nothing to say
};

constexpr uint8_t c_appArrange = 2, c_appNearby = 3, c_appRadio = 5, c_appMecp = 6, c_appTown = 8, c_appTrip = 9,
                  c_appWardrive = 10, c_appCompass = 11, c_appStopwatch = 12,
                  c_appTimer = 13, c_appNavigate = 14;

// the Apps list, in its order
const AppRow c_apps[] = {{c_appMecp, "MECP", "Emergency messages in plain words"},
                         {c_appNearby, "Nearby", "Who is close, how far, which way"},
                         {c_appNavigate, "Navigate", "By road or trail to a node, turn by turn"},
                         {c_appTown, "Town locator", "Which town this is, and the towns around"},
                         {c_appCompass, "Compass", "Which way the device points"},
                         {c_appTrip, "Trip", "Distance and time of this outing"},
                         {c_appWardrive, "Wardrive", "Log where the mesh is heard, to the card"},
                         {c_appStopwatch, "Stopwatch", "Elapsed time with splits"},
                         {c_appTimer, "Timer", "Counts down and says when it is over"},
                         {c_appRadio, "RF monitor", "Channel use, counters, who is heard"},
                         {c_appArrange, "Home manager", "Choose and order the tiles"}};

constexpr size_t c_appRows = 7; // rows of the Apps list on screen

// the tile an app has for Home; Tile::Count for none
uint8_t tileOfApp(uint8_t app)
{
    // in the order of KeyboardView_320x240::Tile: ..., Nearby 6, Airtime 7, Town 8, Trip 9, MECP 10, Compass 11,
    // Wardrive 12, Stopwatch 13, Timer 14
    switch (app) {
    case c_appNearby:
        return 6;
    case c_appRadio:
        return 7;
    case c_appTown:
        return 8;
    case c_appTrip:
        return 9;
    case c_appMecp:
        return 10;
    case c_appCompass:
        return 11;
    case c_appWardrive:
        return 12;
    case c_appStopwatch:
        return 13;
    case c_appTimer:
        return 14;
    case c_appNavigate:
        return 15;
    default:
        return 0xFF;
    }
}

} // namespace

/**
 * The keys of the Apps tab: a list like the others, left and right walk the tabs.
 */
void KeyboardView_320x240::appsKey(uint32_t key)
{
    const size_t count = sizeof(c_apps) / sizeof(c_apps[0]);
    appsFocus = std::min(appsFocus, count - 1);
    if (key == LV_KEY_ENTER) {
        openApp((App)c_apps[appsFocus].app, true);
    } else if (printable(key) && isalpha((int)key)) {
        // a letter jumps to the next app that starts with it
        for (size_t i = 1; i <= count; i++) {
            const size_t at = (appsFocus + i) % count;
            if (tolower(c_apps[at].name[0]) == tolower((int)key)) {
                appsFocus = at;
                break;
            }
        }
    } else if (key == LV_KEY_UP) {
        if (appsFocus > 0)
            appsFocus--;
    } else if (key == LV_KEY_DOWN) {
        if (appsFocus + 1 < count)
            appsFocus++;
    } else if (key == LV_KEY_LEFT) {
        walkTabs(-1);
    } else if (key == LV_KEY_RIGHT) {
        walkTabs(1);
    }
}

/**
 * OK held on an app of the list: onto Home with it, or off it.
 */
void KeyboardView_320x240::openAppMenu(void)
{
    const size_t count = sizeof(c_apps) / sizeof(c_apps[0]);
    appsFocus = std::min(appsFocus, count - 1);
    const uint8_t tile = tileOfApp(c_apps[appsFocus].app);
    std::vector<std::string> items = {"Open"};
    std::vector<uint32_t> values = {0};
    if (tile < (uint8_t)Tile::Count) {
        const bool onHome = std::find(homeTiles.begin(), homeTiles.end(), (Tile)tile) != homeTiles.end();
        items.push_back(onHome ? "Remove from Home" : "Add to Home");
        values.push_back(onHome ? 2 : 1);
    }
    openPicker(Setting::AppMenu, c_apps[appsFocus].name, std::move(items), std::move(values), -1, "", tile);
}

/**
 * Puts a tile on Home: on the first free place, and when there is none the user says which tile makes room.
 */
void KeyboardView_320x240::addTile(Tile tile)
{
    if (std::find(homeTiles.begin(), homeTiles.end(), tile) != homeTiles.end()) {
        toast("Already on Home");
        return;
    }
    const auto free = std::find(homeTiles.begin(), homeTiles.end(), Tile::Empty);
    if (free != homeTiles.end()) {
        *free = tile;
        savePrefs();
        toast((std::string(tileName((uint8_t)tile)) + " is on Home now").c_str());
        return;
    }
    std::vector<std::string> items;
    for (size_t slot = 0; slot < c_homeSlots; slot++)
        items.push_back(tileName((uint8_t)homeTiles[slot]));
    openPicker(Setting::TileSlot, "Home is full. Replace which tile?", std::move(items), {}, -1, "", (uint32_t)tile);
}

void KeyboardView_320x240::handleAppKey(uint32_t key)
{
    switch (app) {
    case App::Arrange: {
        const size_t rows = c_homeSlots / c_homeColumns;
        if (arrangeFocus >= c_homeSlots) {
            // the row below the tiles
            if (key == LV_KEY_UP)
                arrangeFocus = c_homeSlots - c_homeColumns;
            break;
        }
        size_t to = arrangeFocus;
        if (key == LV_KEY_LEFT && arrangeFocus % c_homeColumns > 0)
            to--;
        else if (key == LV_KEY_RIGHT && arrangeFocus % c_homeColumns + 1 < c_homeColumns)
            to++;
        else if (key == LV_KEY_UP && arrangeFocus >= c_homeColumns)
            to -= c_homeColumns;
        else if (key == LV_KEY_DOWN && arrangeFocus / c_homeColumns + 1 < rows)
            to += c_homeColumns;
        else if (key == LV_KEY_DOWN && !carrying)
            to = c_arrangeReset;
        if (to == arrangeFocus)
            break;
        if (carrying) {
            std::swap(homeTiles[arrangeFocus], homeTiles[to]);
            savePrefs();
        }
        arrangeFocus = to;
        break;
    }
    case App::Nearby:
    case App::Pointer:
        nearbyKey(key);
        break;
    case App::Radio:
        radioKey(key);
        break;
    case App::Mecp:
    case App::MecpNew:
        mecpKey(key);
        break;
    case App::Town:
        townKey(key);
        break;
    case App::Trip:
    case App::Wardrive:
        tripKey(key);
        break;
    case App::Compass:
        compassKey(key);
        break;
    case App::Stopwatch:
    case App::Timer:
        clockKey(key);
        break;
    case App::Navigate:
        routeKey(key);
        break;
    default:
        break;
    }
}

void KeyboardView_320x240::activateApp(void)
{
    switch (app) {
    case App::Arrange:
        if (carrying) {
            carrying = false;
        } else if (arrangeFocus >= c_homeSlots) {
            homeTiles = c_defaultHome;
            savePrefs();
            toast("Default layout");
        } else {
            std::vector<std::string> items;
            for (const TileName &tile : c_tiles)
                items.push_back(tile.name);
            openPicker(Setting::TilePick, "Tile for this place", std::move(items), {}, (int32_t)homeTiles[arrangeFocus],
                       "A tile that is on Home swaps places", (uint32_t)arrangeFocus);
        }
        break;
    case App::Nearby:
    case App::Pointer:
        nearbyActivate();
        break;
    case App::Radio:
        radioActivate();
        break;
    case App::Mecp:
    case App::MecpNew:
        mecpActivate();
        break;
    case App::Town:
        townActivate();
        break;
    case App::Trip:
        tripToggle();
        break;
    case App::Wardrive:
        wardriveToggle();
        break;
    case App::Compass:
        compassHold();
        break;
    case App::Stopwatch:
    case App::Timer:
        clockActivate();
        break;
    case App::Navigate:
        routeActivate();
        break;
    default:
        break;
    }
}

void KeyboardView_320x240::holdApp(void)
{
    // the Home manager: pick the tile up, or put it down
    if (app == App::Arrange && arrangeFocus < c_homeSlots)
        carrying = !carrying;
    else if (app == App::Nearby || app == App::Pointer)
        nearbyHold();
    else if (app == App::Radio)
        radioActivate();
    else if (app == App::Mecp || app == App::MecpNew)
        mecpHold();
    else if (app == App::Town)
        townActivate();
    else if (app == App::Trip)
        tripHold();
    else if (app == App::Wardrive)
        wardriveHold();
    else if (app == App::Compass)
        compassHold();
    else if (app == App::Stopwatch || app == App::Timer)
        clockHold();
    else if (app == App::Navigate)
        routeHold();
}

/**
 * What an app has to say in its row of the Apps list; empty while it has nothing.
 */
std::string KeyboardView_320x240::appState(App which) const
{
    switch (which) {
    case App::Nearby: {
        const TileView tile = nearbyTile();
        const NodeRecord *own = nodeStore.find(ownNode);
        if (!own || !own->position.hasCoordinates())
            return "No GPS fix";
        return tile.value == "--" ? "" : tile.value + " to " + tile.sub;
    }
    case App::Radio: {
        if (!hasAirtime)
            return "";
        char buf[32];
        snprintf(buf, sizeof(buf), "Channel %.1f%%", channelUse);
        return buf;
    }
    case App::Trip: {
        const TileView tile = tripTile();
        return tile.value == "--" ? "" : tile.value + " · " + tile.sub;
    }
    case App::Wardrive:
        return prefs.wardrive ? std::to_string(wardriveRows) + " rows logged" : "";
    case App::Stopwatch:
        return stopwatchRunning ? "Running" : stopwatchNow() ? "Stopped" : "";
    case App::Timer:
        return timerRinging ? "Time is up" : timerRunning ? "Running" : "";
    case App::Navigate: {
        const TileView tile = routeTile();
        return routeActive || routeSearching ? tile.value + " · " + tile.sub : "";
    }
    case App::Town:
        return ownTownKnown && ownTown.town.valid() ? ownTown.town.name + ", " + ownTown.town.area : "";
    case App::Mecp: {
        // counted at most every few seconds: the list of apps is drawn often, and counting reads every chat
        static uint32_t counted = 0;
        static size_t count = 0;
        if (!counted || lv_tick_elaps(counted) >= 3000) {
            counted = lv_tick_get() | 1;
            count = mecpMessages().size();
        }
        return count ? std::to_string(count) + (count == 1 ? " message" : " messages") : "";
    }
    default:
        return "";
    }
}

bool KeyboardView_320x240::appsPick(Setting what, uint32_t value, uint32_t target)
{
    switch (what) {
    case Setting::HomeMenu:
        if (value == 2 && target < (uint32_t)Tile::Count) {
            std::vector<std::string> colors(std::begin(c_paletteNames), std::end(c_paletteNames));
            openPicker(Setting::TileColor, (std::string("Color of ") + tileName((uint8_t)target)).c_str(), std::move(colors), {},
                       tileColors[target], "A border in the color, the tile in a shade of it", target);
        } else {
            openApp(value == 0 ? App::List : App::Arrange);
        }
        return true;
    case Setting::TileColor:
        if (target < (uint32_t)Tile::Count && value < c_paletteSize) {
            tileColors[target] = (uint8_t)value;
            savePrefs();
        }
        return true;
    case Setting::AppMenu:
        if (value == 0) {
            appsKey(LV_KEY_ENTER);
        } else if (target < (uint32_t)Tile::Count && value == 1) {
            addTile((Tile)target);
        } else if (target < (uint32_t)Tile::Count) {
            std::replace(homeTiles.begin(), homeTiles.end(), (Tile)target, Tile::Empty);
            savePrefs();
            toast("Taken off Home");
        }
        return true;
    case Setting::TileSlot:
        if (value < c_homeSlots && target < (uint32_t)Tile::Count &&
            std::find(homeTiles.begin(), homeTiles.end(), (Tile)target) == homeTiles.end()) {
            homeTiles[value] = (Tile)target;
            savePrefs();
            toast((std::string(tileName((uint8_t)target)) + " is on Home now").c_str());
        }
        return true;
    case Setting::TilePick: {
        if (target >= c_homeSlots || value >= (uint32_t)Tile::Count)
            return true;
        const Tile tile = (Tile)value;
        const auto other = tile == Tile::Empty ? homeTiles.end() : std::find(homeTiles.begin(), homeTiles.end(), tile);
        if (other != homeTiles.end())
            *other = homeTiles[target];
        homeTiles[target] = tile;
        savePrefs();
        return true;
    }
    default:
        return false;
    }
}

void KeyboardView_320x240::renderApp(void)
{
    static_assert((uint8_t)Tile::Count == sizeof(c_tiles) / sizeof(c_tiles[0]), "a name for every tile");
    static_assert((uint8_t)Tile::Nearby == 6 && (uint8_t)Tile::Airtime == 7 && (uint8_t)Tile::Town == 8 &&
                      (uint8_t)Tile::Trip == 9 && (uint8_t)Tile::Mecp == 10 && (uint8_t)Tile::Compass == 11 &&
                      (uint8_t)Tile::Wardrive == 12 && (uint8_t)Tile::Stopwatch == 13 && (uint8_t)Tile::Timer == 14 &&
                      (uint8_t)Tile::Navigate == 15 && (uint8_t)App::Navigate == c_appNavigate,
                  "tileOfApp() names the tiles by number");
    static_assert((uint8_t)App::Arrange == c_appArrange && (uint8_t)App::Nearby == c_appNearby &&
                      (uint8_t)App::Radio == c_appRadio && (uint8_t)App::Mecp == c_appMecp && (uint8_t)App::Town == c_appTown &&
                      (uint8_t)App::Trip == c_appTrip && (uint8_t)App::Wardrive == c_appWardrive && (uint8_t)App::Compass == c_appCompass &&
                      (uint8_t)App::Stopwatch == c_appStopwatch && (uint8_t)App::Timer == c_appTimer,
                  "the Apps list names the screens by number");
    switch (app) {
    case App::Arrange:
        renderArrange();
        break;
    case App::Nearby:
        renderNearby();
        break;
    case App::Pointer:
        renderPointer();
        break;
    case App::Radio:
        renderRadio();
        break;
    case App::Mecp:
        renderMecpList();
        break;
    case App::MecpNew:
        renderMecpComposer();
        break;
    case App::Town:
        renderTown();
        break;
    case App::Trip:
        renderTrip();
        break;
    case App::Wardrive:
        renderWardrive();
        break;
    case App::Compass:
        renderCompass();
        break;
    case App::Stopwatch:
        renderStopwatch();
        break;
    case App::Timer:
        renderTimer();
        break;
    case App::Navigate:
        renderRoute();
        break;
    default:
        renderAppList();
        break;
    }
}

void KeyboardView_320x240::renderAppList(void)
{
    const size_t count = sizeof(c_apps) / sizeof(c_apps[0]);
    appsFocus = std::min(appsFocus, count - 1);
    appsTop = scrollTop(appsTop, appsFocus, count, c_appRows);
    for (size_t i = appsTop; i < count && i < appsTop + c_appRows; i++) {
        const std::string state = appState((App)c_apps[i].app);
        settingRow(content, 2 + (int32_t)(i - appsTop) * c_settingHeight, i == appsFocus, c_apps[i].name,
                   state.empty() ? c_apps[i].about : state.c_str(), false);
    }
    listScrollbar(content, 2, appsTop, count, c_appRows, c_settingHeight);
}

/**
 * The Home manager shows Home as it will be, and below it what the keys do right now.
 */
void KeyboardView_320x240::renderArrange(void)
{
    const bool onReset = arrangeFocus >= c_homeSlots;
    renderTiles(onReset ? -1 : (int32_t)arrangeFocus, carrying);
    settingRow(content, 3 * (c_tileHeight + c_tileRowGap) + 3, onReset, "Default layout",
               onReset ? "OK: back to the default" : carrying ? "Arrows move, OK drops" : "OK: change  Hold OK: move",
               false);
}

#endif
