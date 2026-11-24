#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/common/ViewController.h"
#include "graphics/map/MapPanel.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/PMTileService.h"
#include "graphics/map/VectorTileService.h"
#include "util/GeoMath.h"
#include "graphics/map/StorageTileService.h"
#include "graphics/map/TileProvider.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include "storage/MapStorage.h"
#include "util/ILog.h"
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#if defined(ARCH_PORTDUINO)
#include "graphics/map/AsyncTileService.h"
#include "graphics/map/CURLService.h"
#elif defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32P4)
#include "graphics/map/AsyncTileService.h"
#include "graphics/map/URLService.h"
#endif

using namespace kbview;

/**
 * The stock map panel, opened up as far as the heads-up display needs.
 */
class KeyboardMapPanel : public MapPanel
{
  public:
    using MapPanel::MapPanel;
    float centerLatitude(void) const { return scrolled.latitude; }
    float centerLongitude(void) const { return scrolled.longitude; }
    bool isLocked(void) const { return locked; }
};

namespace
{

constexpr uint8_t c_zoomOwn = 13;    // first view around this device
constexpr uint8_t c_zoomNode = 11;   // first view around some other node
constexpr uint8_t c_zoomWorld = 3;   // nothing to center on yet
constexpr uint8_t c_zoomMax = 18;    // tile servers and tile sets end around here
constexpr uint8_t c_stretch = 2;     // zoom levels past a style's deepest tiles, drawn from those at 2x and 4x
constexpr uint8_t c_zoomShow = 14;   // "Show on map" zooms in at least this far
constexpr int32_t c_scaleWidth = 80; // longest scale bar in px

constexpr uint32_t c_crumbBase = 0x100;    // map object ids of the trail, below any node number
constexpr uint32_t c_waypointBase = 0x200; // and of the waypoints
constexpr double c_crumbSpacing = 25.0;    // m between two points of the trail
constexpr uint32_t c_keepLocal = UINT32_MAX;
constexpr size_t c_maxHeard = MAX_NUM_NODES_VIEW + 64; // nodes whose channels are remembered
constexpr uint32_t c_addOsm = UINT32_MAX;
constexpr uint32_t c_noExpiry = INT32_MAX; // what the phone apps send for a waypoint that stays
const char *const c_mapFolder = "/maps";
const char *const c_osmStyle = "osm";
const char *const c_osmTemplate = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";

StorageTileService *tileStore = nullptr; // owned by the tile service of the map
std::string styleUrl;                    // where the style in use downloads from; empty: nowhere
bool downloading = false;                // missing tiles are being fetched
std::vector<lv_obj_t *> crumbMarks;
std::vector<lv_obj_t *> waypointMarks;

// where a tile set that covers one region says its middle is
struct {
    bool known;
    float latitude, longitude;
    uint8_t zoom;
} region;

// what the map panel calls to put a mark where its position is on screen
void showMark(const std::vector<lv_obj_t *> &marks, size_t index, uint16_t x, uint16_t y, uint8_t zoom)
{
    if (index >= marks.size() || !marks[index])
        return;
    if (x == 0 && y == 0 && zoom == 0) {
        lv_obj_set_hidden(marks[index], true);
        return;
    }
    lv_obj_set_pos(marks[index], x, y);
    lv_obj_set_hidden(marks[index], false);
    lv_obj_move_foreground(marks[index]);
}

lv_obj_t *chip(lv_obj_t *parent, int32_t x, int32_t y, const char *text, uint32_t color)
{
    lv_obj_t *obj = box(parent, x, y, LV_SIZE_CONTENT, 16, c_ground);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(obj, 5, LV_PART_MAIN);
    lv_obj_center(label(obj, 0, 0, 0, font12, color, text));
    return obj;
}

} // namespace

/**
 * The map lives in its own layer below the heads-up display and is only fed while it is on screen.
 */
void KeyboardView_320x240::showMap(bool show)
{
    if (show && !map)
        loadMap();
    if (show == mapShown)
        return;
    mapShown = show;
    if (show) {
        lv_obj_set_hidden(mapLayer, false);
        lv_obj_set_hidden(mapHud, false);
    } else {
        lv_obj_set_hidden(mapLayer, true);
        lv_obj_set_hidden(mapHud, true);
    }
    if (show && map)
        map->forceRedraw();
}

void KeyboardView_320x240::mapTask(void)
{
    if (!map || !mapShown || screenOff)
        return; // tiles are read and decoded for somebody who looks
    if (ownMoved) {
        ownMoved = false;
        map->setGpsPosition(pendingLat, pendingLon);
    }
    map->task_handler();
    // the heads-up display says when the tiles on screen are stretched
    if (map->redrawComplete() && map->getStretch() != mapStretch) {
        mapStretch = map->getStretch();
        dirty = true;
    }
}

/**
 * Switches to a tile set below /maps. One that is nothing but a packed archive narrows the zoom range to what it holds,
 * plus the levels that are drawn stretched.
 */
void KeyboardView_320x240::selectMapStyle(const std::string &style)
{
    char tileDir[MapTileSettings::TILE_STYLE_SIZE];
    MapTileSettings::styleToDir(style.c_str(), tileDir, sizeof(tileDir));
    const bool archive = MapStorage::hasMapArchive(*storage, MapTileSettings::getPrefix(), tileDir);
    styleUrl = MapStorage::getUrlProvider(*storage, MapTileSettings::getPrefix(), tileDir);
    if (!styleUrl.empty())
        TileProvider::selectTemplate(TileProvider::addTemplate("URL: " + style, styleUrl));
    MapTileSettings::setTileStyle(style.c_str());
    MapTileSettings::setPMTiles(archive);
    MapTile::dropStretchSource();
    mapTiles = true;
    mapZoomMin = 2;
    mapZoomMax = c_zoomMax;
    region = {};
    ILOG_INFO("map style %s%s%s", style.c_str(), archive ? " (archive)" : "", styleUrl.empty() ? "" : " (online)");
    // files *.jmv in the folder of the style: maps of lines, drawn at any zoom (util/VectorMap.h), a state each
    if (VectorTileService::use(std::string(MapTileSettings::getPrefix()) + "/" + tileDir)) {
        double latitude = 0, longitude = 0;
        VectorTileService::center(latitude, longitude);
        MapTileSettings::setPMTiles(false);
        mapZoomMin = std::max<uint8_t>(VectorTileService::minZoom(), 2);
        mapZoomMax = std::max(mapZoomMin, std::min(VectorTileService::maxZoom(), c_zoomMax));
        region.known = true;
        region.latitude = (float)latitude;
        region.longitude = (float)longitude;
        region.zoom = std::clamp<uint8_t>(13, mapZoomMin, mapZoomMax);
    } else {
        VectorTileService::use("");
    }
    if (!VectorTileService::inUse() && archive && styleUrl.empty()) {
        // nothing but the archive: zooming past what it holds would only show an empty map
        const std::string path =
            std::string(MapTileSettings::getPrefix()) + "/" + tileDir + "/" + tileDir + MapTileSettings::PMTILES_EXTENSION;
        uint8_t header[pmtiles::HEADER_BYTES];
        pmtiles::headerv3 parsed{};
        auto file = storage->open(path.c_str(), OpenMode::Read);
        if (file && file->readAt(0, header, sizeof(header)) && pmtiles::deserialize_header(header, sizeof(header), parsed) &&
            parsed.min_zoom <= parsed.max_zoom && parsed.min_zoom <= c_zoomMax) {
            // in int before it is cut to a byte, and never the wrong way around, whatever the header says
            mapZoomMin = (uint8_t)std::max<int>(parsed.min_zoom, 2);
            mapZoomMax = (uint8_t)std::max<int>(mapZoomMin, std::min<int>((int)parsed.max_zoom + c_stretch, c_zoomMax));
            region.known = parsed.center_lat_e7 != 0 || parsed.center_lon_e7 != 0;
            region.latitude = parsed.center_lat_e7 * 1e-7f;
            region.longitude = parsed.center_lon_e7 * 1e-7f;
            region.zoom = std::clamp(parsed.center_zoom, mapZoomMin, mapZoomMax);
        }
    }
    applyTileDownload();
}

/**
 * Tiles come from the SD card: /maps/<style>/z/x/y.png or /maps/<style>/<style>.pmtiles. A style with a .url file
 * can fetch what is missing over Wi-Fi.
 */
void KeyboardView_320x240::loadMap(void)
{
    if (map)
        return;
    ensureStorage();
    MapTileSettings::setStretch(c_stretch);
    tileStore = new StorageTileService(*storage);
    // a style that is a vector map is drawn on the spot; everything else are pictures from the card
    map = new KeyboardMapPanel(mapLayer, new VectorTileService(*storage, new PMTileService(*storage, tileStore)));
    VectorTileService::setDark(!prefs.light);
    applyMapLayers();
    mapTiles = false;
    downloading = false;
    region = {};

    if (mounted) {
        MapTileSettings::setPrefix("/maps");
        const std::set<std::string> styles = MapStorage::loadMapStyles(*storage, MapTileSettings::getPrefix());
        if (styles.count("/map")) {
            // tiles of one style directly below /map
            MapTileSettings::setPrefix("/map");
            MapTileSettings::setTileStyle("");
            MapTileSettings::setPMTiles(false);
            mapTiles = true;
        } else if (!styles.empty()) {
            // the style picked last; else a packed tile set, which was put there on purpose; else the first name
            std::string style = *styles.begin();
            if (styles.count(prefs.mapStyle)) {
                style = prefs.mapStyle;
            } else {
                char tileDir[MapTileSettings::TILE_STYLE_SIZE];
                for (const std::string &candidate : styles) {
                    MapTileSettings::styleToDir(candidate.c_str(), tileDir, sizeof(tileDir));
                    if (MapStorage::hasMapArchive(*storage, MapTileSettings::getPrefix(), tileDir)) {
                        style = candidate;
                        break;
                    }
                }
            }
            selectMapStyle(style);
        }
    }
    MapTileSettings::setSaveOK(mounted);
    MapTileSettings::setUniqueId(ownNode);
    map->updateDimensions();

    const NodeRecord *own = nodeStore.find(ownNode);
    if (own && own->position.hasCoordinates()) {
        map->setHomeLocation(own->position.latitude * 1e-7f, own->position.longitude * 1e-7f);
        map->setZoom(std::clamp(c_zoomOwn, mapZoomMin, mapZoomMax));
        map->setGpsPosition(own->position.latitude * 1e-7f, own->position.longitude * 1e-7f);
        map->setLocked(true);
    } else {
        const NodeRecord *first = nullptr;
        for (const auto &[nodeNum, node] : nodeStore.records()) {
            if (node.position.hasCoordinates() && (!first || node.lastHeard > first->lastHeard))
                first = &node;
        }
        if (first) {
            map->setHomeLocation(first->position.latitude * 1e-7f, first->position.longitude * 1e-7f);
            map->setZoom(std::clamp(c_zoomNode, mapZoomMin, mapZoomMax));
            map->setScrolledPosition(first->position.latitude * 1e-7f, first->position.longitude * 1e-7f);
        } else if (region.known) {
            // no position to show yet: the part of the world the tiles are for, not an empty one
            map->setHomeLocation(region.latitude, region.longitude);
            map->setZoom(region.zoom);
            map->setScrolledPosition(region.latitude, region.longitude);
        } else {
            map->setZoom(std::clamp(c_zoomWorld, mapZoomMin, mapZoomMax));
        }
    }

    for (const auto &[nodeNum, node] : nodeStore.records())
        placeMarker(nodeNum);
    placeWaypoints();
    placeBreadcrumbs();
    applyTileDownload();
}

/**
 * Puts a node on the map or moves it; a node without a position loses its marker.
 */
void KeyboardView_320x240::placeMarker(uint32_t nodeNum)
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    // the trail is kept from the first fix on, the map may be opened much later
    if (nodeNum == ownNode && node && node->position.hasCoordinates())
        addBreadcrumb(node->position.latitude, node->position.longitude);
    if (!map)
        return;
    if (!node || !node->position.hasCoordinates() || !inMapView(nodeNum, mapView)) {
        dropMarker(nodeNum);
        return;
    }
    const float lat = node->position.latitude * 1e-7f, lon = node->position.longitude * 1e-7f;

    lv_obj_t *&marker = markers[nodeNum];
    if (!marker) {
        if (nodeNum == ownNode) {
            // this device: a blue dot in a white ring, which no line and no stop of a route looks like
            marker = box(mapLayer, 0, 0, 16, 16, 0x1A73E8);
            outline(marker, 0xFFFFFF, 3, 8);
        } else {
            auto [bg, fg] = colorOf(nodeNum);
            const bool owned = ownedNodes.count(nodeNum);
            marker = box(mapLayer, 0, 0, LV_SIZE_CONTENT, owned ? 18 : 16, bg);
            outline(marker, owned ? c_owned : c_ground, owned ? 2 : 1, 4);
            lv_obj_set_style_pad_hor(marker, 4, LV_PART_MAIN);
            lv_obj_center(label(marker, 0, 0, 0, font12, fg, shortName(nodeNum).c_str()));
        }
        // the map reports where the position is; the marker sits centered on it
        lv_obj_set_style_translate_x(marker, LV_PCT(-50), LV_PART_MAIN);
        lv_obj_set_style_translate_y(marker, LV_PCT(-50), LV_PART_MAIN);
        lv_obj_set_hidden(marker, true);
    } else if (nodeNum != ownNode && lv_obj_get_child_count(marker)) {
        lv_label_set_text(lv_obj_get_child(marker, 0), shortName(nodeNum).c_str());
    }

    map->add(nodeNum, lat, lon, [this](uint32_t id, uint16_t x, uint16_t y, uint8_t zoom) {
        const auto it = markers.find(id);
        if (it == markers.end() || !it->second)
            return;
        if (x == 0 && y == 0 && zoom == 0) {
            lv_obj_set_hidden(it->second, true);
            return;
        }
        lv_obj_set_pos(it->second, x, y);
        lv_obj_set_hidden(it->second, false);
        lv_obj_move_foreground(it->second);
    });
    if (nodeNum == ownNode) {
        // a map that follows the own position loads its tiles anew with every fix: not behind a dark screen
        if (screenOff || !mapShown) {
            pendingLat = lat;
            pendingLon = lon;
            ownMoved = true;
        } else {
            ownMoved = false;
            map->setGpsPosition(lat, lon);
        }
    }
    if (mapShown)
        dirty = true;
}

void KeyboardView_320x240::dropMarker(uint32_t nodeNum)
{
    const auto it = markers.find(nodeNum);
    if (it == markers.end())
        return;
    if (map)
        map->remove(nodeNum);
    if (it->second)
        lv_obj_delete(it->second);
    markers.erase(it);
    if (mapTarget == nodeNum)
        mapTarget = 0;
}

/**
 * The map on a place that is not a node, such as the position in an emergency message.
 */
void KeyboardView_320x240::showPlace(double latitude, double longitude)
{
    switchTab(Tab::Map);
    if (!map)
        loadMap();
    if (!map)
        return;
    map->setLocked(false);
    if (MapTileSettings::getZoomLevel() < std::min(c_zoomShow, mapZoomMax))
        map->setZoom(std::min(c_zoomShow, mapZoomMax));
    map->setScrolledPosition((float)latitude, (float)longitude);
    mapTarget = 0;
}

void KeyboardView_320x240::showOnMap(uint32_t nodeNum)
{
    const NodeRecord *node = nodeStore.find(nodeNum);
    if (!node || !node->position.hasCoordinates()) {
        toast("No position shared");
        return;
    }
    switchTab(Tab::Map);
    if (!map)
        loadMap();
    if (!inMapView(nodeNum, mapView)) {
        setMapView(c_viewAll);
        toast("Map shows all nodes again");
    }
    map->setLocked(false);
    if (MapTileSettings::getZoomLevel() < std::min(c_zoomShow, mapZoomMax))
        map->setZoom(std::min(c_zoomShow, mapZoomMax));
    map->setScrolledPosition(node->position.latitude * 1e-7f, node->position.longitude * 1e-7f);
    mapTarget = nodeNum;
}

void KeyboardView_320x240::handleMapKey(uint32_t key)
{
    if (!map)
        return;
    auto pan = [this](int16_t dx, int16_t dy) {
        map->setLocked(false);
        if (!map->scroll(dx, dy))
            map->forceRedraw();
    };
    auto zoom = [this](int delta) {
        const int level = MapTileSettings::getZoomLevel() + delta;
        if (level >= mapZoomMin && level <= mapZoomMax)
            map->setZoom((uint8_t)level);
    };

    switch (key) {
    case LV_KEY_LEFT:
        pan(1, 0);
        break;
    case LV_KEY_RIGHT:
        pan(-1, 0);
        break;
    case LV_KEY_UP:
        pan(0, 1);
        break;
    case LV_KEY_DOWN:
        pan(0, -1);
        break;
    case '+':
    case '=':
    case 'i':
    case 'I':
        zoom(1);
        break;
    case '-':
    case 'o':
    case 'O':
        zoom(-1);
        break;
    case LV_KEY_ENTER: {
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates()) {
            map->setLocked(true);
            mapTarget = 0;
        } else {
            toast("No GPS fix yet");
        }
        break;
    }
    case 'n':
    case 'N': {
        // nodes with a position, one after the other
        uint32_t first = 0, next = 0;
        for (const auto &[nodeNum, marker] : markers) {
            if (nodeNum == ownNode)
                continue;
            if (!first)
                first = nodeNum;
            if (!next && nodeNum > mapTarget)
                next = nodeNum;
        }
        if (!next)
            next = first;
        if (next)
            showOnMap(next);
        else
            toast(mapView == c_viewAll ? "No node has shared a position" : "No node of this view has shared a position");
        break;
    }
    case 'v':
    case 'V': {
        // the views one after the other: all nodes, the favorites, each channel
        std::vector<uint8_t> views{c_viewAll};
        if (!favorites.empty())
            views.push_back(c_viewFavorites);
        for (uint8_t i = 0; i < c_max_channels; i++) {
            if (channelInfo[i].enabled)
                views.push_back(i);
        }
        const auto at = std::find(views.begin(), views.end(), mapView);
        setMapView(at == views.end() || at + 1 == views.end() ? c_viewAll : *(at + 1));
        char buf[48];
        snprintf(buf, sizeof(buf), "%s · %u with a position", mapViewName(mapView).c_str(), (unsigned)mapViewCount(mapView));
        toast(buf);
        break;
    }
    default:
        break;
    }
}

/**
 * Heads-up display over the map: zoom, follow state, scale and the keys that work here.
 */
void KeyboardView_320x240::renderMap(void)
{
    if (!map)
        return;
    const int32_t w = c_width, h = lv_obj_get_height(mapHud);
    const uint8_t zoom = MapTileSettings::getZoomLevel();
    renderRouteLine();
    const bool imperial = hasDisplay && display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
    char buf[64];

    char stretched[24] = "";
    if (mapStretch)
        snprintf(stretched, sizeof(stretched), " · stretched %ux", 1u << mapStretch);
    snprintf(buf, sizeof(buf), "Z%u%s · %s%s", (unsigned)zoom, stretched, map->isLocked() ? "Follow" : "Free",
             downloading ? " · Online" : "");
    chip(mapHud, 4, 4, buf, map->isLocked() ? c_focus : c_text);

    const NodeRecord *target = mapTarget ? nodeStore.find(mapTarget) : nullptr;
    if (target) {
        const std::string distance = distanceTo(*target);
        snprintf(buf, sizeof(buf), "%s%s%s", shortName(mapTarget).c_str(), distance.empty() ? "" : " · ", distance.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%u on the map", (unsigned)markers.size());
    }
    // a view that leaves nodes out says so in place of the count, or below the node that is looked at
    const bool filtered = mapView != c_viewAll;
    char view[48];
    snprintf(view, sizeof(view), "%s · %u", mapViewName(mapView).c_str(), (unsigned)(markers.size() - markers.count(ownNode)));
    lv_obj_align(chip(mapHud, 0, 0, filtered && !target ? view : buf, filtered && !target ? c_focus : c_text), LV_ALIGN_TOP_RIGHT,
                 -4, 4);
    if (filtered && target)
        lv_obj_align(chip(mapHud, 0, 0, view, c_focus), LV_ALIGN_TOP_RIGHT, -4, 24);

    if (!mapTiles)
        lv_obj_align(chip(mapHud, 0, 0, mounted ? "No map yet: hold OK, Map style" : "No SD card, no map tiles", c_focusMuted),
                     LV_ALIGN_TOP_MID, 0, filtered && target ? 46 : 26);
    if (styleUrl.find("openstreetmap") != std::string::npos)
        lv_obj_align(chip(mapHud, 0, 0, "(c) OpenStreetMap", c_muted), LV_ALIGN_TOP_RIGHT, -4, h - 40);

    if (!barFocus && !map->isLocked()) {
        // crosshair: what "N" and the zoom are centered on
        box(mapHud, w / 2 - 6, h / 2, 13, 1, c_text);
        box(mapHud, w / 2, h / 2 - 6, 1, 13, c_text);
    }

    // scale bar in a round number of the chosen units
    const double metersPerPixel = 156543.03392 * cos(map->centerLatitude() * M_PI / 180.0) / (double)(1u << zoom) * 256.0 /
                                  std::max<int>(1, MapTileSettings::getTileSize());
    if (metersPerPixel > 0.0) {
        double unit = imperial ? 0.3048 : 1.0;
        const char *name = imperial ? "ft" : "m";
        if (metersPerPixel * c_scaleWidth / unit >= (imperial ? 5280.0 : 1000.0)) {
            unit = imperial ? 1609.344 : 1000.0;
            name = imperial ? "mi" : "km";
        }
        const double longest = metersPerPixel * c_scaleWidth / unit;
        double length = pow(10.0, floor(log10(longest)));
        if (length * 5 <= longest)
            length *= 5;
        else if (length * 2 <= longest)
            length *= 2;
        const int32_t px = std::max<int32_t>(4, (int32_t)lround(length * unit / metersPerPixel));
        snprintf(buf, sizeof(buf), "%g %s", length, name);
        chip(mapHud, 4, h - 40, buf, c_text);
        box(mapHud, 3, h - 23, px + 2, 5, c_ground);
        box(mapHud, 4, h - 22, px, 3, c_text);
    }

    // while a route is followed: the turn that comes up, so that the map is the guide
    const std::string guide = guideText();
    if (!guide.empty())
        lv_obj_align(chip(mapHud, 0, 0, guide.c_str(), c_focus), LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *hint = box(mapHud, 0, h - 16, w, 16, c_ground);
    label(hint, 0, 1, w, font12, c_focusMuted,
          barFocus ? "OK or Up: move the map" : "I/O zoom · OK follow · N node · V view · Hold OK",
          LV_TEXT_ALIGN_CENTER);
}

// -------- tile download --------

/**
 * Tiles that are missing on the card are fetched while the map is looked at and kept there, when that is wanted,
 * Wi-Fi is up and the style says where its tiles come from.
 */
void KeyboardView_320x240::applyTileDownload(void)
{
    if (!map)
        return;
    const bool wanted = prefs.tileDownload && wifiUp && mounted && !styleUrl.empty();
    if (wanted == downloading)
        return;
    downloading = wanted;
    if (wanted) {
        auto saveTile = [](const char *name, void *img, size_t len) { return tileStore->save(name, img, len); };
#if defined(ARCH_PORTDUINO)
        map->setBackupService(new AsyncTileService(new CURLService(saveTile)));
#elif defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32P4)
        map->setBackupService(new AsyncTileService(new URLService(saveTile)));
#else
        (void)saveTile;
        downloading = false;
#endif
    } else {
        map->setBackupService(nullptr);
    }
    // tiles that were asked for from the old source never arrive
    map->forceRedraw();
    if (mapShown)
        dirty = true;
}

// -------- map menu --------

/**
 * Hold OK on the map.
 */
void KeyboardView_320x240::openMapMenu(void)
{
    if (!map)
        return;
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    auto add = [&items, &values](const std::string &name, uint32_t value) {
        items.push_back(name);
        values.push_back(value);
    };
    char buf[32];
    if (mapTarget && nodeStore.find(mapTarget))
        add(chatName(mapTarget), 10);
    add("Show: " + mapViewName(mapView), 7);
    add("Map style", 1);
    if (VectorTileService::inUse())
        add("Layers", 8);
    add("Drop a waypoint here", 2);
    snprintf(buf, sizeof(buf), "Waypoints (%u)", (unsigned)waypoints.size());
    add(buf, 3);
    add(prefs.breadcrumbs ? "Hide my trail" : "Show my trail", 4);
    if (!breadcrumbs.empty())
        add("Clear my trail", 5);
    add(prefs.tileDownload ? "Stop fetching missing tiles" : "Fetch missing tiles over Wi-Fi", 6);
    openPicker(Setting::MapMenu, "Map", std::move(items), std::move(values), -1, "", mapTarget);
}

/**
 * The tile sets on the card. It is read each time: an upload may have added one.
 */
void KeyboardView_320x240::openMapStyles(void)
{
    if (!ensureStorage()) {
        toast("No SD card");
        return;
    }
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    int32_t current = -1;
    char tileDir[MapTileSettings::TILE_STYLE_SIZE];
    const bool styled = mapTiles && !strcmp(MapTileSettings::getPrefix(), c_mapFolder);
    mapStyles.clear();
    const std::set<std::string> styles = MapStorage::loadMapStyles(*storage, c_mapFolder);
    for (const std::string &style : styles) {
        if (style == "/map")
            continue;
        MapTileSettings::styleToDir(style.c_str(), tileDir, sizeof(tileDir));
        const bool archive = MapStorage::hasMapArchive(*storage, c_mapFolder, tileDir);
        const bool online = !MapStorage::getUrlProvider(*storage, c_mapFolder, tileDir).empty();
        if (styled && style == MapTileSettings::getTileStyle())
            current = (int32_t)items.size();
        values.push_back((uint32_t)mapStyles.size());
        mapStyles.push_back(style);
        items.push_back(style + (archive ? " · packed" : "") + (online ? " · online" : ""));
    }
    if (!styles.count(c_osmStyle)) {
        items.push_back("OpenStreetMap (online)");
        values.push_back(c_addOsm);
    }
    openPicker(Setting::MapStyle, "Map style", std::move(items), std::move(values), current,
               "Folders below /maps on the SD card");
}

// -------- map views --------

/**
 * The radio keeps no list of who is on a channel. A node counts as being on one when something of it was decoded
 * with that channel's key: a packet since power-on, a message in the log, or the node info the radio stored.
 */
void KeyboardView_320x240::markHeard(uint32_t nodeNum, uint8_t channel)
{
    static_assert(c_max_channels <= 8, "one bit per channel");
    if (!nodeNum || nodeNum == UINT32_MAX || nodeNum == ownNode || channel >= c_max_channels)
        return;
    const uint8_t bit = (uint8_t)(1u << channel);
    const auto known = heardOn.find(nodeNum);
    if (known != heardOn.end()) {
        if (known->second & bit)
            return;
        known->second |= bit;
    } else {
        if (heardOn.size() >= c_maxHeard) {
            // nodes the list has dropped since
            for (auto it = heardOn.begin(); it != heardOn.end();)
                it = nodeStore.find(it->first) ? std::next(it) : heardOn.erase(it);
        }
        if (heardOn.size() >= c_maxHeard)
            return;
        heardOn[nodeNum] = bit;
    }
    if (mapView == channel)
        placeMarker(nodeNum);
}

bool KeyboardView_320x240::inMapView(uint32_t nodeNum, uint8_t view) const
{
    if (view == c_viewAll || nodeNum == ownNode)
        return true;
    if (view == c_viewFavorites)
        return favorites.count(nodeNum) > 0;
    const auto heard = heardOn.find(nodeNum);
    return view < c_max_channels && heard != heardOn.end() && (heard->second & (1u << view));
}

std::string KeyboardView_320x240::mapViewName(uint8_t view) const
{
    if (view == c_viewFavorites)
        return "Favorites";
    return view < c_max_channels ? "#" + chatName(view) : "All nodes";
}

/**
 * Nodes of a view that have a position, this device not counted.
 */
size_t KeyboardView_320x240::mapViewCount(uint8_t view) const
{
    size_t count = 0;
    for (const auto &[nodeNum, node] : nodeStore.records())
        count += nodeNum != ownNode && node.position.hasCoordinates() && inMapView(nodeNum, view);
    return count;
}

/**
 * The view is stored by channel name, the slot of a channel may change. Until the channels are known, and when
 * the channel is gone, the map shows all nodes; the stored choice stays for when it comes back.
 */
void KeyboardView_320x240::resolveMapView(void)
{
    uint8_t view = c_viewAll;
    if (prefs.mapView == "fav") {
        view = c_viewFavorites;
    } else if (!prefs.mapView.empty() && prefs.mapView[0] == '#') {
        for (uint8_t i = 0; i < c_max_channels && view == c_viewAll; i++) {
            if (channelInfo[i].enabled && !prefs.mapView.compare(1, std::string::npos, channelInfo[i].name))
                view = i;
        }
    }
    if (view == mapView)
        return;
    mapView = view;
    refreshMarkers();
}

void KeyboardView_320x240::setMapView(uint8_t view)
{
    if (view != c_viewFavorites && (view >= c_max_channels || !channelInfo[view].enabled))
        view = c_viewAll;
    const std::string name = view == c_viewAll ? "all" : view == c_viewFavorites ? "fav" : "#" + channelInfo[view].name;
    if (name != prefs.mapView) {
        prefs.mapView = name;
        savePrefs();
    }
    if (view == mapView)
        return;
    mapView = view;
    refreshMarkers();
}

void KeyboardView_320x240::refreshMarkers(void)
{
    if (!map)
        return;
    for (const auto &[nodeNum, node] : nodeStore.records())
        placeMarker(nodeNum);
    dirty = true;
}

void KeyboardView_320x240::openMapViews(void)
{
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    int32_t current = 0;
    auto add = [&](uint8_t view) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%u)", mapViewName(view).c_str(), (unsigned)mapViewCount(view));
        if (view == mapView)
            current = (int32_t)items.size();
        items.push_back(buf);
        values.push_back(view);
    };
    add(c_viewAll);
    add(c_viewFavorites);
    for (uint8_t i = 0; i < c_max_channels; i++) {
        if (channelInfo[i].enabled)
            add(i);
    }
    openPicker(Setting::MapView, "Show on the map", std::move(items), std::move(values), current,
               "A channel: the nodes heard on it");
}

// -------- waypoints --------

void KeyboardView_320x240::openWaypoints(void)
{
    if (waypoints.empty()) {
        toast("No waypoints yet");
        return;
    }
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    for (size_t i = 0; i < waypoints.size(); i++) {
        const Waypoint &point = waypoints[i];
        items.push_back(point.from == ownNode ? point.name : point.name + " · " + shortName(point.from));
        values.push_back((uint32_t)i);
    }
    openPicker(Setting::Waypoints, "Waypoints", std::move(items), std::move(values));
}

/**
 * Marks for all waypoints; they are few, so a change places them all again.
 */
void KeyboardView_320x240::placeWaypoints(void)
{
    if (!map)
        return;
    for (size_t i = 0; i < waypointMarks.size(); i++) {
        map->remove(c_waypointBase + i);
        if (waypointMarks[i])
            lv_obj_delete(waypointMarks[i]);
    }
    waypointMarks.clear();
    for (size_t i = 0; i < waypoints.size(); i++) {
        // square and light, where a node is round and colored
        lv_obj_t *mark = box(mapLayer, 0, 0, LV_SIZE_CONTENT, 16, c_text);
        outline(mark, c_ground, 1, 0);
        lv_obj_set_style_pad_hor(mark, 4, LV_PART_MAIN);
        lv_obj_center(label(mark, 0, 0, 0, font12, c_ground, waypoints[i].name.c_str()));
        lv_obj_set_style_translate_x(mark, LV_PCT(-50), LV_PART_MAIN);
        lv_obj_set_style_translate_y(mark, LV_PCT(-50), LV_PART_MAIN);
        lv_obj_set_hidden(mark, true);
        waypointMarks.push_back(mark);
        map->add(c_waypointBase + i, waypoints[i].latitude * 1e-7f, waypoints[i].longitude * 1e-7f,
                 [](uint32_t id, uint16_t x, uint16_t y, uint8_t zoom) { showMark(waypointMarks, id - c_waypointBase, x, y, zoom); });
    }
    if (mapShown)
        dirty = true;
}

/**
 * A waypoint someone shared, changed or took back.
 */
void KeyboardView_320x240::newWaypoint(uint32_t from, uint8_t ch, const meshtastic_Waypoint &waypoint)
{
    (void)ch;
    const uint32_t time = now();
    const auto old =
        std::find_if(waypoints.begin(), waypoints.end(), [&waypoint](const Waypoint &point) { return point.id == waypoint.id; });
    // the apps take a waypoint back by sending it again with an expiry in the past
    const bool gone = !waypoint.has_latitude_i || !waypoint.has_longitude_i ||
                      (waypoint.expire && (!validTime(waypoint.expire) || (validTime(time) && waypoint.expire <= time)));
    if (gone) {
        if (old == waypoints.end())
            return;
        toast(("Waypoint removed: " + old->name).c_str());
        waypoints.erase(old);
    } else {
        Waypoint point{waypoint.id, from, waypoint.latitude_i, waypoint.longitude_i, waypoint.expire, waypoint.name};
        if (point.name.empty())
            point.name = "Waypoint";
        toast(("Waypoint from " + shortName(from) + ": " + point.name).c_str());
        if (old != waypoints.end()) {
            *old = point;
        } else {
            if (waypoints.size() >= c_maxWaypoints)
                waypoints.erase(waypoints.begin());
            waypoints.push_back(point);
        }
    }
    savePrefs();
    placeWaypoints();
}

// -------- trail --------

/**
 * Own positions of this session, one every few steps.
 */
void KeyboardView_320x240::addBreadcrumb(int32_t latitude, int32_t longitude)
{
    if (!prefs.breadcrumbs)
        return;
    if (!breadcrumbs.empty()) {
        const double north = (latitude - breadcrumbs.back().first) * 1e-7 * 111320.0;
        const double east = (longitude - breadcrumbs.back().second) * 1e-7 * 111320.0 * cos(latitude * 1e-7 * M_PI / 180.0);
        if (north * north + east * east < c_crumbSpacing * c_crumbSpacing)
            return;
    }
    breadcrumbs.emplace_back(latitude, longitude);
    if (breadcrumbs.size() > c_maxBreadcrumbs)
        breadcrumbs.erase(breadcrumbs.begin());
    placeBreadcrumbs();
}

void KeyboardView_320x240::placeBreadcrumbs(void)
{
    if (!map)
        return;
    const size_t wanted = prefs.breadcrumbs ? breadcrumbs.size() : 0;
    while (crumbMarks.size() > wanted) {
        map->remove(c_crumbBase + crumbMarks.size() - 1);
        if (crumbMarks.back())
            lv_obj_delete(crumbMarks.back());
        crumbMarks.pop_back();
    }
    for (size_t i = 0; i < wanted; i++) {
        if (i >= crumbMarks.size()) {
            lv_obj_t *mark = box(mapLayer, 0, 0, 5, 5, c_blue);
            outline(mark, c_ground, 1, 3);
            lv_obj_set_style_translate_x(mark, LV_PCT(-50), LV_PART_MAIN);
            lv_obj_set_style_translate_y(mark, LV_PCT(-50), LV_PART_MAIN);
            lv_obj_set_hidden(mark, true);
            crumbMarks.push_back(mark);
        }
        map->add(c_crumbBase + i, breadcrumbs[i].first * 1e-7f, breadcrumbs[i].second * 1e-7f,
                 [this](uint32_t id, uint16_t x, uint16_t y, uint8_t zoom) {
                     showMark(crumbMarks, id - c_crumbBase, x, y, zoom);
                     // the newest point is where this device is: its dot stays on top
                     const auto own = markers.find(ownNode);
                     if (own != markers.end() && own->second)
                         lv_obj_move_foreground(own->second);
                 });
    }
    if (mapShown)
        dirty = true;
}

// -------- what the pickers and editors of the map lead to --------

bool KeyboardView_320x240::mapPick(Setting what, uint32_t value, uint32_t target)
{
    switch (what) {
    case Setting::MapMenu:
        if (!map)
            return true;
        switch (value) {
        case 1:
            openMapStyles();
            break;
        case 2: {
            const NodeRecord *own = nodeStore.find(ownNode);
            if (map->isLocked() && own && own->position.hasCoordinates()) {
                dropLatitude = own->position.latitude;
                dropLongitude = own->position.longitude;
            } else {
                dropLatitude = (int32_t)lround(map->centerLatitude() * 1e7);
                dropLongitude = (int32_t)lround(map->centerLongitude() * 1e7);
            }
            char note[40];
            snprintf(note, sizeof(note), "%.5f, %.5f", dropLatitude * 1e-7, dropLongitude * 1e-7);
            openEditor(Setting::WaypointName, "Name the waypoint", "", sizeof(((meshtastic_Waypoint *)0)->name) - 1, "Name", note);
            break;
        }
        case 3:
            openWaypoints();
            break;
        case 4: {
            prefs.breadcrumbs = !prefs.breadcrumbs;
            savePrefs();
            const NodeRecord *own = nodeStore.find(ownNode);
            if (prefs.breadcrumbs && own && own->position.hasCoordinates())
                addBreadcrumb(own->position.latitude, own->position.longitude);
            placeBreadcrumbs();
            toast(prefs.breadcrumbs ? "Trail on" : "Trail hidden and paused");
            break;
        }
        case 5:
            breadcrumbs.clear();
            placeBreadcrumbs();
            toast("Trail cleared");
            break;
        case 6:
            prefs.tileDownload = !prefs.tileDownload;
            savePrefs();
            applyTileDownload();
            toast(!prefs.tileDownload    ? "Tiles come from the SD card only"
                  : !wifiUp              ? "On: starts once Wi-Fi is connected"
                  : styleUrl.empty()     ? "On: this map style has no online source"
                                         : "Fetching missing tiles",
                  3000);
            break;
        case 7:
            openMapViews();
            break;
        case 8:
            openMapLayers();
            break;
        case 10:
            openNodeMenu(target);
            break;
        default:
            break;
        }
        return true;
    case Setting::MapLayers:
        prefs.mapLayersOff ^= (uint8_t)(1u << value);
        savePrefs();
        applyMapLayers();
        if (map)
            map->forceRedraw();
        openMapLayers((int32_t)value); // stays open, for the next one
        return true;
    case Setting::MapView:
        setMapView((uint8_t)value);
        if (mapView != c_viewAll && !mapViewCount(mapView))
            toast(mapView == c_viewFavorites ? "No favorite has shared a position" : "Nobody heard on this channel has shared a position",
                  3000);
        return true;
    case Setting::MapStyle: {
        std::string style;
        if (!map || !ensureStorage())
            return true;
        if (value == c_addOsm) {
            if (!MapStorage::setUrlProvider(*storage, c_mapFolder, c_osmStyle, c_osmTemplate)) {
                toast("Could not write to the SD card");
                return true;
            }
            style = c_osmStyle;
        } else if (value < mapStyles.size()) {
            style = mapStyles[value];
        } else {
            return true;
        }
        MapTileSettings::setPrefix(c_mapFolder);
        MapTileSettings::setSaveOK(true);
        selectMapStyle(style);
        prefs.mapStyle = style;
        savePrefs();
        const uint8_t zoom = MapTileSettings::getZoomLevel();
        if (zoom < mapZoomMin || zoom > mapZoomMax)
            map->setZoom(std::clamp(zoom, mapZoomMin, mapZoomMax));
        map->forceRedraw();
        if (!styleUrl.empty() && !downloading)
            toast(prefs.tileDownload ? "New tiles arrive once Wi-Fi is connected" : "Fetching missing tiles is turned off", 3000);
        dirty = true;
        return true;
    }
    case Setting::WaypointShare: {
        uint32_t id = (uint32_t)lv_rand(1, INT32_MAX) ^ now() ^ (uint32_t)lv_tick_get();
        if (!id)
            id = 1;
        if (waypoints.size() >= c_maxWaypoints)
            waypoints.erase(waypoints.begin());
        waypoints.push_back(Waypoint{id, ownNode, dropLatitude, dropLongitude, 0, dropName});
        savePrefs();
        placeWaypoints();
        if (value < c_max_channels) {
            meshtastic_Waypoint waypoint = meshtastic_Waypoint_init_default;
            waypoint.id = id;
            waypoint.has_latitude_i = true;
            waypoint.latitude_i = dropLatitude;
            waypoint.has_longitude_i = true;
            waypoint.longitude_i = dropLongitude;
            waypoint.expire = c_noExpiry;
            snprintf(waypoint.name, sizeof(waypoint.name), "%s", dropName.c_str());
            const uint32_t requestId = requests.addRequest(value, ResponseHandler::noRequest);
            const bool sent =
                controller->sendWaypoint(UINT32_MAX, (uint8_t)value, lora.hop_limit ? lora.hop_limit : 3, requestId, waypoint);
            toast(sent ? ("Shared on " + chatName(value)).c_str() : "Saved, but it could not be sent");
        } else {
            toast("Waypoint saved");
        }
        return true;
    }
    case Setting::Waypoints:
        if (value < waypoints.size()) {
            const Waypoint &point = waypoints[value];
            char note[40];
            snprintf(note, sizeof(note), "%.5f, %.5f", point.latitude * 1e-7, point.longitude * 1e-7);
            openPicker(Setting::WaypointMenu, point.name.c_str(),
                       {"Show on the map", "Navigate: by road, driving", "Navigate: on foot, roads and trails", "Delete"}, {1, 3, 4, 2}, -1,
                       note, value);
        }
        return true;
    case Setting::WaypointMenu:
        if (target >= waypoints.size())
            return true;
        if (value == 1) {
            const Waypoint point = waypoints[target];
            switchTab(Tab::Map);
            if (!map)
                loadMap();
            map->setLocked(false);
            if (MapTileSettings::getZoomLevel() < std::min(c_zoomShow, mapZoomMax))
                map->setZoom(std::min(c_zoomShow, mapZoomMax));
            map->setScrolledPosition(point.latitude * 1e-7f, point.longitude * 1e-7f);
            mapTarget = 0;
        } else if (value == 3 || value == 4) {
            const Waypoint point = waypoints[target];
            navigateTo(point.latitude, point.longitude, point.name, value == 4 ? 1 : 0, 0);
        } else if (value == 2) {
            waypoints.erase(waypoints.begin() + target);
            savePrefs();
            placeWaypoints();
            toast("Waypoint deleted");
        }
        return true;
    default:
        return false;
    }
}

bool KeyboardView_320x240::mapEdit(Setting what, const std::string &text, uint32_t target)
{
    (void)target;
    if (what != Setting::WaypointName)
        return false;
    const size_t first = text.find_first_not_of(' ');
    dropName = first == std::string::npos ? "Waypoint" : text.substr(first, text.find_last_not_of(' ') - first + 1);

    std::vector<std::string> items{"Keep it on this device"};
    std::vector<uint32_t> values{c_keepLocal};
    for (uint32_t i = 0; i < c_max_channels; i++) {
        if (channels[i].role == meshtastic_Channel_Role_DISABLED)
            continue;
        items.push_back("Share on " + chatName(i));
        values.push_back(i);
    }
    openPicker(Setting::WaypointShare, dropName.c_str(), std::move(items), std::move(values), -1, "Everyone on that channel sees it");
    return true;
}

/**
 * The map goes along with this device from now on, as after OK on the map.
 */
void KeyboardView_320x240::followOwn(void)
{
    if (map)
        map->setLocked(true);
}

/**
 * The route on the map: a line over the tiles, from the positions of its points on the screen. Only what is on
 * the screen is drawn, and points closer together than a few pixels are left out.
 */
void KeyboardView_320x240::renderRouteLine(void)
{
    routeLine.clear();
    planLine.clear();
    if (!map)
        return;
    const bool leg = routeActive && route.points.size() >= 2, whole = planRunning && planStops.size() >= 2;
    if (!leg && !whole)
        return;
    const int32_t width = c_width, height = lv_obj_get_height(mapHud);
    const double world = (double)MapTileSettings::getTileSize() * (double)(1u << MapTileSettings::getZoomLevel());
    auto project = [world](double latitude, double longitude, double &x, double &y) {
        const double rad = latitude * M_PI / 180.0;
        x = (longitude + 180.0) / 360.0 * world;
        y = (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0 * world;
    };
    double centerX, centerY;
    project(map->centerLatitude(), map->centerLongitude(), centerX, centerY);
    auto onScreen = [&](int32_t latitude, int32_t longitude, double &x, double &y) {
        project(latitude * 1e-7, longitude * 1e-7, x, y);
        x += width / 2.0 - centerX;
        y += height / 2.0 - centerY;
    };
    // a line through count places: the stretches that touch the screen, as runs of points; a run ends where the
    // line leaves
    int32_t lines = 0; // drawn so far: they are the lowest of what lies over the map
    auto path = [&](std::vector<lv_point_precise_t> &store, size_t count, const std::function<void(size_t, int32_t &, int32_t &)> &at,
                    uint32_t color) {
        std::vector<std::pair<size_t, size_t>> runs; // first point and count in the store
        const double margin = 40;
        bool open = false;
        double lastX = 0, lastY = 0;
        for (size_t i = 0; i < count; i++) {
            int32_t latitude, longitude;
            at(i, latitude, longitude);
            double x, y;
            onScreen(latitude, longitude, x, y);
            const bool inside = x >= -margin && x <= width + margin && y >= -margin && y <= height + margin;
            // a stretch can cross the screen with both of its ends outside: far ends are pulled in, which is close enough
            const bool near = x >= -2000 && x <= width + 2000 && y >= -2000 && y <= height + 2000;
            if (!near) {
                open = false;
                continue;
            }
            if (open && !inside && (lastX < -margin || lastX > width + margin || lastY < -margin || lastY > height + margin)) {
                // from outside to outside: only worth a stretch when it may cross
                const bool crosses = !((x < 0 && lastX < 0) || (x > width && lastX > width) || (y < 0 && lastY < 0) ||
                                       (y > height && lastY > height));
                if (!crosses) {
                    store.back() = lv_point_precise_t{(lv_value_precise_t)lround(x), (lv_value_precise_t)lround(y)};
                    lastX = x;
                    lastY = y;
                    continue;
                }
            }
            if (open && i + 1 < count && fabs(x - lastX) < 3 && fabs(y - lastY) < 3)
                continue;
            if (!open) {
                runs.emplace_back(store.size(), 0);
                open = true;
            }
            store.push_back(lv_point_precise_t{(lv_value_precise_t)lround(x), (lv_value_precise_t)lround(y)});
            runs.back().second++;
            lastX = x;
            lastY = y;
        }
        for (const auto &[first, length] : runs) {
            if (length < 2)
                continue;
            for (int pass = 0; pass < 2; pass++) {
                // a bright line on a dark casing, so that it shows on any map; each goes below what is there
                lv_obj_t *line = lv_line_create(mapHud);
                lv_obj_remove_style_all(line);
                lv_line_set_points(line, store.data() + first, (uint32_t)length);
                lv_obj_set_style_line_width(line, pass ? 6 : 3, LV_PART_MAIN);
                lv_obj_set_style_line_color(line, lv_color_hex(pass ? c_ground : color), LV_PART_MAIN);
                lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
                lv_obj_move_to_index(line, 0); // below the chips of the map
                lines++;
            }
        }
    };
    // the way to the next stop or to the destination; what is drawn later lies below
    if (leg)
        path(routeLine, route.points.size(), [this](size_t i, int32_t &latitude, int32_t &longitude) {
            latitude = route.points[i].latitude;
            longitude = route.points[i].longitude;
        }, c_focus);
    // a route of several stops: the whole of it as it was given, from stop to stop
    if (whole)
        path(planLine, planStops.size(), [this](size_t i, int32_t &latitude, int32_t &longitude) {
            latitude = planStops[i].latitude;
            longitude = planStops[i].longitude;
        }, c_planLine);
    double x, y;
    if (whole) {
        // its first and its last stop
        for (const size_t i : {(size_t)0, planStops.size() - 1}) {
            onScreen(planStops[i].latitude, planStops[i].longitude, x, y);
            if (x >= 0 && x <= width && y >= 0 && y <= height) {
                lv_obj_t *end = box(mapHud, (int32_t)lround(x) - 4, (int32_t)lround(y) - 4, 9, 9, i ? c_planLine : c_ground);
                outline(end, i ? c_ground : c_planLine, 2, i ? 5 : 2);
            }
        }
    }
    // this device, over the lines (which hide its mark on the map below them) and under the chips
    const NodeRecord *own = nodeStore.find(ownNode);
    if (own && own->position.hasCoordinates()) {
        onScreen(own->position.latitude, own->position.longitude, x, y);
        if (x >= 0 && x <= width && y >= 0 && y <= height) {
            lv_obj_t *here = box(mapHud, (int32_t)lround(x) - 8, (int32_t)lround(y) - 8, 16, 16, 0x1A73E8);
            outline(here, 0xFFFFFF, 3, 8);
            lv_obj_move_to_index(here, lines);
        }
    }
    // where it leads now
    const int32_t toLatitude = leg ? route.points.back().latitude : planStops[std::min(planAt, planStops.size() - 1)].latitude;
    const int32_t toLongitude = leg ? route.points.back().longitude : planStops[std::min(planAt, planStops.size() - 1)].longitude;
    onScreen(toLatitude, toLongitude, x, y);
    if (x >= 0 && x <= width && y >= 0 && y <= height) {
        lv_obj_t *end = box(mapHud, (int32_t)lround(x) - 5, (int32_t)lround(y) - 5, 11, 11, c_focus);
        outline(end, c_ground, 2, 6);
    }
}

#endif

/**
 * A fix of the own position: says so when the border of a country has come near, at 500, 250 and 100 m,
 * and when it was crossed. The maps in use tell where the border is (the maps of states and provinces);
 * where none of them covers the place nothing is said.
 */
void KeyboardView_320x240::borderFix(int32_t latitude, int32_t longitude)
{
    if (!prefs.borderWarn || !VectorTileService::inUse())
        return;
    const uint32_t tick = lv_tick_get();
    if (borderChecked && tick - borderChecked < 4000)
        return;
    borderChecked = tick ? tick : 1;
    float meters = 0, bearing = 0;
    bool crossed = false;
    if (!VectorTileService::border(latitude * 1e-7, longitude * 1e-7, meters, bearing, crossed)) {
        borderStep = 0;
        return;
    }
    if (crossed) {
        toast("You have crossed the border of a country", 12000);
        playAlarm();
        borderStep = 3;
        return;
    }
    static const float c_within[] = {1e9f, 500, 250, 100};
    // further away again by half: the next time it comes near is news again
    while (borderStep > 0 && meters > c_within[borderStep] * 1.5f)
        borderStep--;
    const uint8_t step = meters < c_within[3] ? 3 : meters < c_within[2] ? 2 : meters < c_within[1] ? 1 : 0;
    if (step <= borderStep)
        return;
    borderStep = step;
    char text[72];
    if (imperialUnits())
        snprintf(text, sizeof(text), "Border of a country %u ft to the %s", (unsigned)lround(meters / 0.3048 / 10) * 10, geo::cardinal(bearing));
    else
        snprintf(text, sizeof(text), "Border of a country %u m to the %s", (unsigned)lround(meters / 10) * 10, geo::cardinal(bearing));
    toast(text, 10000);
    playAlarm();
}

/**
 * What a map drawn from lines shows: each part on or off. (The border warning does not need the boundaries shown.)
 */
void KeyboardView_320x240::openMapLayers(int32_t focus)
{
    static const char *const names[] = {"Contours", "Water", "Roads", "Trails", "Boundaries", "Names"};
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    for (uint32_t i = 0; i < 6; i++) {
        items.push_back(std::string(names[i]) + (prefs.mapLayersOff & (1u << i) ? ": off" : ": on"));
        values.push_back(i);
    }
    openPicker(Setting::MapLayers, "Map layers", std::move(items), std::move(values), focus, "OK turns one on or off; Back when done");
}

void KeyboardView_320x240::applyMapLayers(void)
{
    VectorMap::Layers &layers = VectorTileService::layers();
    layers.contours = !(prefs.mapLayersOff & 1);
    layers.water = !(prefs.mapLayersOff & 2);
    layers.roads = !(prefs.mapLayersOff & 4);
    layers.trails = !(prefs.mapLayersOff & 8);
    layers.boundaries = !(prefs.mapLayersOff & 16);
    layers.names = !(prefs.mapLayersOff & 32);
}

void KeyboardView_320x240::mapThemeChanged(void)
{
    VectorTileService::setDark(!prefs.light);
    if (map && VectorTileService::inUse())
        map->forceRedraw();
}
