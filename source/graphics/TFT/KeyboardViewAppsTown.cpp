#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include "util/GeoMath.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace kbview;

/**
 * Town locator: says in words where this device is and where others are. The towns come from /geo/towns.bin on
 * the card (util/TownIndex.h); without that file the screen says where it goes.
 */

namespace
{

const char *const c_townFile = "/geo/towns.bin";
constexpr uint32_t c_retryTime = 30000;  // ms between two looks for the file
constexpr int32_t c_movedLat = 4500;     // 1e-7 degrees, about 50 m: the own town is looked up again after that
constexpr size_t c_aroundCount = 12;
constexpr size_t c_hitCount = 20;
constexpr size_t c_cacheSize = 24;
constexpr size_t c_searchMax = 16;
constexpr int32_t c_rowsTop = 66;
constexpr size_t c_rows = 5;

const char *kindNote(uint8_t kind)
{
    switch (kind) {
    case TownIndex::Statistical:
        return " · census area";
    case TownIndex::Water:
        return " · water";
    case TownIndex::Unorganized:
        return " · unorganized";
    default:
        return "";
    }
}

} // namespace

/**
 * Opens the town data once it is there. A card that came later is found on the next look.
 */
bool KeyboardView_320x240::openTowns(void)
{
    if (towns.state() == TownIndex::State::Ok)
        return true;
    if (townsTried && lv_tick_elaps(townsTried) < c_retryTime)
        return false;
    townsTried = lv_tick_get() | 1;
    if (!ensureStorage() || !storage->exists(c_townFile))
        return false;
    std::shared_ptr<IFile> file(storage->open(c_townFile, OpenMode::Read).release());
    if (!file)
        return false;
    const TownIndex::State state = towns.open([file](uint32_t offset, void *buffer, uint32_t length) {
        return file->seek(offset) && file->read(buffer, length) == (int32_t)length;
    });
    ILOG_INFO("town data: %s, %u towns", state == TownIndex::State::Ok ? "open" : "not usable", (unsigned)towns.count());
    ownTownKnown = false;
    townCache.clear();
    return state == TownIndex::State::Ok;
}

/**
 * The town this device is in. Looked up again only after it has moved some 50 m.
 */
const TownIndex::Place &KeyboardView_320x240::ownPlace(void)
{
    static const TownIndex::Place nowhere;
    const NodeRecord *own = nodeStore.find(ownNode);
    if (!own || !own->position.hasCoordinates() || !openTowns())
        return nowhere;
    const int32_t latitude = own->position.latitude, longitude = own->position.longitude;
    if (!ownTownKnown || std::abs(latitude - ownTownLatitude) > c_movedLat || std::abs(longitude - ownTownLongitude) > c_movedLat) {
        ownTown = towns.find(latitude, longitude);
        townsAround = towns.around(latitude, longitude, c_aroundCount, ownTown.town.number);
        ownTownLatitude = latitude;
        ownTownLongitude = longitude;
        ownTownKnown = true;
    }
    return ownTown;
}

/**
 * The name of the town a position is in; empty when the data does not say. Positions asked for before are
 * remembered, since a screen asks again with every drawing.
 */
std::string KeyboardView_320x240::townOf(int32_t latitude, int32_t longitude)
{
    if (!openTowns())
        return "";
    // to about 20 m
    const uint64_t key = ((uint64_t)(uint32_t)(latitude / 2000) << 32) | (uint32_t)(longitude / 2000);
    for (const auto &[cached, name] : townCache)
        if (cached == key)
            return name;
    const TownIndex::Place place = towns.find(latitude, longitude);
    const std::string name = place.town.valid() ? place.town.name : std::string();
    if (townCache.size() >= c_cacheSize)
        townCache.erase(townCache.begin());
    townCache.emplace_back(key, name);
    return name;
}

/**
 * The rows below the own town: what was typed for, or else the towns around.
 */
std::vector<TownIndex::Entry> KeyboardView_320x240::townRows(void)
{
    if (!townSearch.empty())
        return townHits;
    ownPlace();
    return ownTownKnown ? townsAround : std::vector<TownIndex::Entry>();
}

void KeyboardView_320x240::townKey(uint32_t key)
{
    const size_t count = townRows().size();
    if (key == LV_KEY_BACKSPACE || (printable(key) && townSearch.size() < c_searchMax)) {
        if (key == LV_KEY_BACKSPACE) {
            if (!townSearch.empty())
                townSearch.pop_back();
        } else {
            townSearch.push_back((char)key);
        }
        townHits = openTowns() && !townSearch.empty() ? towns.search(townSearch, c_hitCount) : std::vector<TownIndex::Entry>();
        // the same name is in many states: the nearest first
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates()) {
            const int32_t latitude = own->position.latitude, longitude = own->position.longitude;
            std::stable_sort(townHits.begin(), townHits.end(), [latitude, longitude](const TownIndex::Entry &a, const TownIndex::Entry &b) {
                return geo::leg(latitude, longitude, a.latitude, a.longitude).meters <
                       geo::leg(latitude, longitude, b.latitude, b.longitude).meters;
            });
        }
        townFocus = townTop = 0;
    } else if (key == LV_KEY_UP) {
        if (townFocus > 0)
            townFocus--;
    } else if (key == LV_KEY_DOWN) {
        if (townFocus + 1 < count)
            townFocus++;
    } else if (key == LV_KEY_LEFT) {
        closeApp();
    } else if (key == LV_KEY_RIGHT) {
        townActivate();
    }
}

/**
 * OK on a town: the pointer page on its label point, from where OK shows it on the map.
 */
void KeyboardView_320x240::townActivate(void)
{
    const std::vector<TownIndex::Entry> rows = townRows();
    if (townFocus >= rows.size())
        return;
    const TownIndex::Entry &town = rows[townFocus];
    pointTo(town.latitude * 1e-7, town.longitude * 1e-7, town.name + ", " + town.area);
}

bool KeyboardView_320x240::townBack(void)
{
    if (app != App::Town || townSearch.empty())
        return false;
    townSearch.clear();
    townHits.clear();
    townFocus = townTop = 0;
    return true;
}

void KeyboardView_320x240::renderTown(void)
{
    char buf[64];
    if (!openTowns()) {
        const bool other = towns.state() == TownIndex::State::OtherVersion;
        label(content, 8, 40, c_width - 16, font16, c_text, other ? "Town data of another version" : "No town data on the card",
              LV_TEXT_ALIGN_CENTER);
        label(content, 8, 74, c_width - 16, font12, c_muted, "The file is towns.bin in the folder /geo", LV_TEXT_ALIGN_CENTER);
        label(content, 8, 92, c_width - 16, font12, c_muted, "of the SD card. It is built on a computer", LV_TEXT_ALIGN_CENTER);
        label(content, 8, 110, c_width - 16, font12, c_muted, "with maps/towns.py and can be sent over", LV_TEXT_ALIGN_CENTER);
        label(content, 8, 128, c_width - 16, font12, c_muted, "Wi-Fi with the upload page.", LV_TEXT_ALIGN_CENTER);
        return;
    }

    const NodeRecord *own = nodeStore.find(ownNode);
    const bool fix = own && own->position.hasCoordinates();
    const TownIndex::Place &place = ownPlace();
    if (!fix) {
        label(content, 8, 6, c_width - 16, font20, c_text, "No GPS fix");
        label(content, 8, 32, c_width - 16, font14, c_muted, "Towns can still be looked up by name");
    } else if (place.town.valid()) {
        label(content, 8, 6, c_width - 16, font20, c_text, place.town.name.c_str());
        label(content, 8, 32, c_width - 16, font14, c_muted, (place.town.area + kindNote(place.town.kind)).c_str());
        if (place.neighbor.valid())
            label(content, 8, 50, c_width - 16, font12, c_focus, ("Near the " + place.neighbor.name + " line").c_str());
    } else {
        label(content, 8, 6, c_width - 16, font20, c_text, place.covered ? "No town here" : "Outside the town data");
        label(content, 8, 32, c_width - 16, font14, c_muted, place.covered ? "Water, or land the data leaves out" : "The data ends before this place");
    }
    if (!townSearch.empty()) {
        snprintf(buf, sizeof(buf), "Towns that start with: %s", townSearch.c_str());
        lv_obj_t *search = label(content, 8, 50, c_width - 16, font12, c_focus, buf);
        lv_obj_set_style_bg_color(search, lv_color_hex(c_ground), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(search, LV_OPA_COVER, LV_PART_MAIN);
    }

    const std::vector<TownIndex::Entry> rows = townRows();
    const bool imperial = imperialUnits();
    if (rows.empty()) {
        label(content, 8, c_rowsTop + 30, c_width - 16, font14, c_muted, townSearch.empty() ? "Type the first letters of a town" : "No town starts like that",
              LV_TEXT_ALIGN_CENTER);
    }
    townFocus = std::min(townFocus, rows.empty() ? 0 : rows.size() - 1);
    townTop = scrollTop(townTop, townFocus, rows.size(), c_rows);
    for (size_t i = townTop; i < rows.size() && i < townTop + c_rows; i++) {
        const TownIndex::Entry &town = rows[i];
        std::string value;
        if (fix) {
            const geo::Leg leg = geo::leg(own->position.latitude, own->position.longitude, town.latitude, town.longitude);
            geo::distanceText(leg.meters, imperial, buf, sizeof(buf));
            value = std::string(buf) + " " + geo::cardinal(leg.bearing);
        }
        // a search finds the same name in many states: say which one
        const std::string title = townSearch.empty() ? town.name : town.name + ", " + town.area.substr(town.area.rfind(' ') + 1);
        if (!townSearch.empty())
            value = (value.empty() ? "" : value + " · ") + town.area.substr(0, town.area.rfind(','));
        settingRow(content, c_rowsTop + (int32_t)(i - townTop) * c_settingHeight, i == townFocus, title.c_str(), value.c_str(), false);
    }
    listScrollbar(content, c_rowsTop, townTop, rows.size(), c_rows, c_settingHeight);
    label(content, 8, 203, c_width - 16, font12, c_focusMuted,
          townSearch.empty() ? "Type: find a town   OK: point to it" : "OK: point to it   Back: towns around", LV_TEXT_ALIGN_CENTER);
}

/**
 * The Town tile.
 */
KeyboardView_320x240::TileView KeyboardView_320x240::townTile(void)
{
    TileView view;
    view.value = "--";
    const NodeRecord *own = nodeStore.find(ownNode);
    if (!openTowns()) {
        view.sub = "No town data";
    } else if (!own || !own->position.hasCoordinates()) {
        view.sub = "No fix";
    } else {
        const TownIndex::Place &place = ownPlace();
        if (place.town.valid()) {
            view.value = place.town.name;
            view.sub = place.neighbor.valid() ? "Near the " + place.neighbor.name + " line" : place.town.area;
        } else {
            view.sub = place.covered ? "No town here" : "Outside the data";
        }
    }
    return view;
}

#endif
