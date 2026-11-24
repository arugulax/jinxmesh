#include "graphics/map/VectorTileService.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
// one map for the one map panel there is; the service object comes and goes with the panel
IStorage *store = nullptr;

// the maps of the folder in use: one a state or region. They are opened as tiles ask for them, a few at a time.
struct Entry {
    std::string path;
    int32_t south, west, north, east;
    uint16_t group; // see VectorMap::group()
};
struct Slot {
    int entry = -1;
    std::unique_ptr<IFile> file;
    VectorMap map;
    uint32_t used = 0;
};
constexpr size_t c_maps = 240, c_open = 3;
std::vector<Entry> catalog;
Slot slots[c_open];
uint32_t useTick = 0;
// the borders around the place last asked about: of its cell and the eight around it
std::vector<double> borderLines;
int64_t borderCell = -1;
bool borderCovered = false;
bool borderHasBefore = false;
double borderBeforeX = 0, borderBeforeY = 0;
uint8_t lowZoom = 0, highZoom = 0;
int32_t allSouth = 0, allWest = 0, allNorth = 0, allEast = 0;

bool openInto(Slot &slot, const std::string &path)
{
    slot.map.close();
    slot.entry = -1;
    slot.file = store ? store->open(path.c_str(), OpenMode::Read) : nullptr;
    if (!slot.file)
        return false;
    Slot *self = &slot;
    if (!slot.map.open([self](uint32_t offset, void *buffer, uint32_t length) { return self->file && self->file->readAt(offset, buffer, length); })) {
        slot.file.reset();
        return false;
    }
    return true;
}

void closeAll(void)
{
    for (Slot &slot : slots) {
        slot.map.close();
        slot.file.reset();
        slot.entry = -1;
    }
    catalog.clear();
    borderCell = -1;
    borderHasBefore = false;
}

// the open map of that entry of the catalog; the one unused longest makes room
VectorMap *mapOf(int entry)
{
    Slot *oldest = &slots[0];
    for (Slot &slot : slots) {
        if (slot.entry == entry) {
            slot.used = ++useTick;
            return &slot.map;
        }
        if (slot.entry < 0 || (oldest->entry >= 0 && slot.used < oldest->used))
            oldest = &slot;
    }
    if (!openInto(*oldest, catalog[entry].path))
        return nullptr;
    oldest->entry = entry;
    oldest->used = ++useTick;
    return &oldest->map;
}
VectorMap::Layers shownLayers;
bool darkGround = true;
uint32_t tileMs = 0;
uint16_t serial = 0;

// ".../<z>/<x>/<y>.png"
bool parseZXY(const char *name, uint32_t &z, uint32_t &x, uint32_t &y)
{
    if (!name)
        return false;
    const char *slash[3] = {nullptr, nullptr, nullptr};
    for (const char *p = name; *p; ++p) {
        if (*p == '/') {
            slash[0] = slash[1];
            slash[1] = slash[2];
            slash[2] = p;
        }
    }
    return slash[0] && sscanf(slash[0], "/%u/%u/%u", &z, &x, &y) == 3;
}
} // namespace

VectorTileService::VectorTileService(IStorage &storage, ITileService *fallback) : ITileService("V:"), fallback(fallback)
{
    store = &storage;
}

VectorTileService::~VectorTileService()
{
    closeAll();
    delete fallback;
}

bool VectorTileService::use(const std::string &folder)
{
    closeAll();
    if (folder.empty() || !store)
        return false;
    // The maps of the states and those of the provinces are one map, whichever of the two is picked: they
    // lie in folders of their own next to each other, "state" and "province", and meet at the border.
    const size_t cut = folder.rfind('/');
    const std::string parent = cut == std::string::npos ? "" : folder.substr(0, cut), last = folder.substr(cut == std::string::npos ? 0 : cut + 1);
    std::vector<std::string> folders{folder};
    if (last == "state")
        folders.push_back(parent + "/province");
    else if (last == "province")
        folders.push_back(parent + "/state");
    std::vector<std::string> names;
    for (const std::string &from : folders) {
        store->list(from.c_str(), [&](const char *name, bool isDir) {
            const size_t length = strlen(name);
            if (!isDir && length > 4 && strcmp(name + length - 4, ".jmv") == 0 && name[0] != '.' && names.size() < c_maps)
                names.push_back(from + "/" + name);
        });
    }
    for (const std::string &name : names) {
        Entry entry;
        entry.path = name;
        if (!openInto(slots[0], entry.path)) {
            ILOG_WARN("%s is no vector map of this version", entry.path.c_str());
            continue;
        }
        const VectorMap &map = slots[0].map;
        map.bounds(entry.south, entry.west, entry.north, entry.east);
        entry.group = map.group();
        if (catalog.empty()) {
            lowZoom = map.minZoom();
            highZoom = map.maxZoom();
            allSouth = entry.south;
            allWest = entry.west;
            allNorth = entry.north;
            allEast = entry.east;
        } else {
            lowZoom = std::min(lowZoom, map.minZoom());
            highZoom = std::max(highZoom, map.maxZoom());
            allSouth = std::min(allSouth, entry.south);
            allWest = std::min(allWest, entry.west);
            allNorth = std::max(allNorth, entry.north);
            allEast = std::max(allEast, entry.east);
        }
        ILOG_INFO("vector map %s, zoom %u to %u", entry.path.c_str(), map.minZoom(), map.maxZoom());
        catalog.push_back(entry);
    }
    slots[0].map.close();
    slots[0].file.reset();
    if (catalog.empty())
        return false;
    // colors of one's own: <folder>.colors in it, lines like "light.ground=E9DEC3" or "dark.contour=444B53"
    const size_t slash = folder.rfind('/');
    const std::string colorsPath = folder + "/" + folder.substr(slash == std::string::npos ? 0 : slash + 1) + ".colors";
    auto colors = store->open(colorsPath.c_str(), OpenMode::Read);
    if (colors) {
        char text[1500];
        const int32_t got = colors->read(text, sizeof(text) - 1);
        text[got > 0 ? got : 0] = 0;
        for (char *line = strtok(text, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
            char *equals = strchr(line, '=');
            const bool dark = strncmp(line, "dark.", 5) == 0;
            if (!equals || (!dark && strncmp(line, "light.", 6) != 0))
                continue;
            *equals = 0;
            if (!VectorMap::setColor(dark, line + (dark ? 5 : 6), (uint32_t)strtoul(equals + 1, nullptr, 16)))
                ILOG_WARN("%s: no color named %s", colorsPath.c_str(), line);
        }
    }
    return true;
}

namespace
{
bool crosses(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy)
{
    auto side = [](double px, double py, double qx, double qy, double rx, double ry) { return (qx - px) * (ry - py) - (qy - py) * (rx - px); };
    const double d1 = side(cx, cy, dx, dy, ax, ay), d2 = side(cx, cy, dx, dy, bx, by);
    const double d3 = side(ax, ay, bx, by, cx, cy), d4 = side(ax, ay, bx, by, dx, dy);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}
} // namespace

bool VectorTileService::border(double latitude, double longitude, float &meters, float &bearing, bool &crossed)
{
    crossed = false;
    if (catalog.empty() || latitude > 85 || latitude < -85)
        return false;
    const double x = (longitude + 180.0) / 360.0, y = (1.0 - asinh(tan(latitude * M_PI / 180.0)) / M_PI) / 2.0;
    // by cells of zoom 13, the finest the maps have
    const int64_t here = (int64_t)(y * 8192.0) * 8192 + (int64_t)(x * 8192.0);
    if (here != borderCell) {
        borderCell = here;
        borderLines.clear();
        borderCovered = false;
        std::vector<double> found;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                // of the maps that have this cell, the one with the most for it, as for drawing
                uint32_t most = 0;
                std::vector<double> best;
                for (size_t i = 0; i < catalog.size(); i++) {
                    const Entry &entry = catalog[i];
                    if (entry.west * 1e-7 > longitude + 0.2 || entry.east * 1e-7 < longitude - 0.2 || entry.south * 1e-7 > latitude + 0.2 ||
                        entry.north * 1e-7 < latitude - 0.2)
                        continue;
                    VectorMap *map = mapOf((int)i);
                    if (!map)
                        continue;
                    found.clear();
                    const uint32_t weight = map->collect(VectorMap::NationLine, x, y, dx, dy, found);
                    if (weight > most) {
                        most = weight;
                        best.swap(found);
                    }
                }
                if (most && dx == 0 && dy == 0)
                    borderCovered = true;
                borderLines.insert(borderLines.end(), best.begin(), best.end());
            }
        }
    }
    if (!borderCovered) {
        borderHasBefore = false;
        return false;
    }
    // near a place the square is as good as flat: this many meters to one of it, both ways
    const double scale = 40075016.686 * cos(latitude * M_PI / 180.0);
    double nearest = 1e30, nearX = 0, nearY = 0;
    for (size_t i = 0; i + 3 < borderLines.size(); i += 4) {
        const double ax = borderLines[i], ay = borderLines[i + 1], bx = borderLines[i + 2], by = borderLines[i + 3];
        const double ex = bx - ax, ey = by - ay, length = ex * ex + ey * ey;
        double t = length > 0 ? ((x - ax) * ex + (y - ay) * ey) / length : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const double px = ax + t * ex, py = ay + t * ey, d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < nearest) {
            nearest = d;
            nearX = px;
            nearY = py;
        }
        if (borderHasBefore && crosses(borderBeforeX, borderBeforeY, x, y, ax, ay, bx, by))
            crossed = true;
    }
    // a way of a few steps says nothing (the fix wanders), one of many kilometers is no way (a new start)
    const double moved = sqrt((x - borderBeforeX) * (x - borderBeforeX) + (y - borderBeforeY) * (y - borderBeforeY)) * scale;
    if (borderHasBefore && moved > 20000)
        crossed = false;
    if (!borderHasBefore || moved > 15 || crossed) {
        borderBeforeX = x;
        borderBeforeY = y;
        borderHasBefore = true;
    }
    meters = nearest > 1e29 ? 1e9f : (float)(sqrt(nearest) * scale);
    double degrees = atan2(nearX - x, -(nearY - y)) * 180.0 / M_PI;
    bearing = (float)(degrees < 0 ? degrees + 360.0 : degrees);
    return true;
}

bool VectorTileService::inUse(void)
{
    return !catalog.empty();
}

uint8_t VectorTileService::minZoom(void)
{
    return lowZoom;
}

uint8_t VectorTileService::maxZoom(void)
{
    return highZoom;
}

void VectorTileService::center(double &latitude, double &longitude)
{
    latitude = (allSouth * 0.5 + allNorth * 0.5) * 1e-7;
    longitude = (allWest * 0.5 + allEast * 0.5) * 1e-7;
}

void VectorTileService::setDark(bool dark)
{
    darkGround = dark;
}

VectorMap::Layers &VectorTileService::layers(void)
{
    return shownLayers;
}

uint32_t VectorTileService::lastTileMs(void)
{
    return tileMs;
}

bool VectorTileService::load(const char *name, void *img)
{
    if (catalog.empty())
        return fallback ? fallback->load(name, img) : false;
    uint32_t z, x, y;
    if (!parseZXY(name, z, x, y) || z > 24)
        return false;
    const uint32_t start = lv_tick_get();
    const uint32_t bytes = VectorMap::c_tile * VectorMap::c_tile * 2;
    lv_image_dsc_t *dsc = (lv_image_dsc_t *)lv_malloc_zeroed(sizeof(lv_image_dsc_t));
    uint8_t *data = (uint8_t *)lv_malloc(bytes);
    if (!dsc || !data) {
        if (dsc)
            lv_free(dsc);
        if (data)
            lv_free(data);
        ILOG_WARN("no memory for a map tile");
        return false;
    }
    // the maps that reach into this tile each draw their lines, the first the ground under them; then their names
    const double span = 360.0 / (double)(1u << z);
    const double tileWest = x * span - 180.0, tileEast = tileWest + span;
    auto latitudeOf = [&](uint32_t row) { return atan(sinh(M_PI * (1.0 - 2.0 * row / (double)(1u << z)))) * 180.0 / M_PI; };
    const double tileNorth = latitudeOf(y), tileSouth = latitudeOf(y + 1);
    std::vector<int> here;
    for (size_t i = 0; i < catalog.size(); i++) {
        const Entry &entry = catalog[i];
        if (!(entry.west * 1e-7 > tileEast || entry.east * 1e-7 < tileWest || entry.south * 1e-7 > tileNorth || entry.north * 1e-7 < tileSouth))
            here.push_back((int)i);
    }
    // Along a border two maps hold the same cells, each of them whole: such a cell is drawn from the map
    // that has the most for it (the first of equals). Asked per cell, as names reach in from the cells around.
    // In the coarse levels a map holds its state alone and all maps are laid over each other, but the pieces
    // of one map that was cut up share cells there too: among those of a group the same rule holds.
    int16_t owner[3][3];
    int32_t ownerGroup = -1;
    auto ownerOf = [&](int dx, int dy, uint16_t group) { // group 0: among all maps
        if (ownerGroup != group) {
            for (auto &row : owner)
                for (int16_t &known : row)
                    known = -2; // not asked yet
            ownerGroup = group;
        }
        int16_t &known = owner[dy + 1][dx + 1];
        if (known == -2) {
            known = -1;
            uint32_t most = 0;
            for (int i : here) {
                if (group && catalog[i].group != group)
                    continue;
                VectorMap *map = mapOf(i);
                const uint32_t weight = map ? map->weight((uint8_t)z, x, y, dx, dy) : 0;
                if (weight > most) {
                    most = weight;
                    known = (int16_t)i;
                }
            }
        }
        return (int)known;
    };
    bool drawn = false;
    for (uint8_t part : {(uint8_t)VectorMap::Lines, (uint8_t)VectorMap::Names}) {
        for (int i : here) {
            VectorMap *map = mapOf(i);
            if (!map)
                continue;
            const uint8_t parts = part | (part == VectorMap::Lines && !drawn ? VectorMap::Ground : 0);
            const uint16_t among = map->whole((uint8_t)z) ? 0 : catalog[i].group;
            const VectorMap::Mine mine = [&, i, among](int dx, int dy) { return ownerOf(dx, dy, among) == i; };
            const bool shared = here.size() > 1 && (map->whole((uint8_t)z) || among);
            if (shared) {
                // settle who draws what before drawing: asking opens the other maps, and this one has to be
                // the open one while it draws
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        ownerOf(dx, dy, among);
                map = mapOf(i);
                if (!map)
                    continue;
            }
            if (map->render((uint8_t)z, x, y, (uint16_t *)data, darkGround, shownLayers, parts, shared ? &mine : nullptr))
                drawn = true;
        }
        if (!drawn)
            break;
    }
    if (!drawn) {
        // outside of what the map covers
        lv_free(data);
        lv_free(dsc);
        return false;
    }
    // the same shape the PNG decoder hands over, so that the map tile frees it the same way
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.w = VectorMap::c_tile;
    dsc->header.h = VectorMap::c_tile;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = VectorMap::c_tile * 2;
    dsc->header.flags = LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1;
    dsc->header.reserved_2 = (uint16_t)(0x8000 | ++serial); // tells images apart that land at the same address
    dsc->data = data;
    dsc->data_size = bytes;
    lv_obj_t *obj = (lv_obj_t *)img;
    lv_image_set_src(obj, dsc);
    if (lv_image_get_src(obj) != dsc) {
        lv_free(data);
        lv_free(dsc);
        return false;
    }
    tileMs = lv_tick_get() - start;
    ILOG_DEBUG("vector tile %u/%u/%u drawn in %u ms", (unsigned)z, (unsigned)x, (unsigned)y, (unsigned)tileMs);
    return true;
}

bool VectorTileService::save(const char *name, void *img, size_t len)
{
    return fallback ? fallback->save(name, img, len) : false;
}
