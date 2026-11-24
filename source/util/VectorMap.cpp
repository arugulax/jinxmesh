#include "util/VectorMap.h"
#include "util/VectorMapFont.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr int c_size = VectorMap::c_tile;
constexpr int c_extent = 4096;       // units across a cell
constexpr uint8_t c_overZoom = 2;    // zooms past the last one of the finest level that are still drawn
constexpr uint32_t c_maxCell = 3 << 19; // bytes; a cell larger than this is not one of ours
constexpr int c_nameReach = 110;     // pixels a name may reach from its place

uint16_t rgb(uint32_t color)
{
    return (uint16_t)(((color >> 19) & 0x1F) << 11 | ((color >> 10) & 0x3F) << 5 | ((color >> 3) & 0x1F));
}

struct Look {
    uint16_t color = 0;
    uint8_t width = 0; // 0: not drawn at this zoom
    uint8_t on = 0;    // dashes: pixels drawn, then pixels left out; 0: a solid line
    uint8_t off = 0;
};

struct Palette {
    uint32_t ground, contour, contourIndex, contourMain, water, waterLine, motorway, primary, secondary, minor, service, track, path,
        rail, boundary, text, halo;
};

// The colors: a tan ground with grey contour lines, so that roads, water and trails stand out on a small
// screen. A file next to the map can change any of them (VectorMap::setColor), to try a look without a build.
Palette c_light = {0xE9DEC3, 0xC2BAA8, 0x9F9888, 0x7B756A, 0xA9CFF0, 0x2F78BF, 0xC0392B, 0xD35400, 0x1E1E1E,
                   0x444444, 0x6E6E6E, 0x8D6E3F, 0x2E7D32, 0x111111, 0x8E44AD, 0x111111, 0xE9DEC3};
// the same tan map under the dark theme: the paper look reads best on this screen. A dark one can be had
// through the colors file of the map ("dark.ground=0B0F14" and so on).
Palette c_dark = c_light;

const char *const c_colorNames[] = {"ground", "contour", "contourIndex", "contourMain", "water",   "waterLine", "motorway", "primary", "secondary",
                                    "minor",  "service", "track",        "path",        "rail",    "boundary",  "text",     "halo"};

Look lookOf(uint8_t kind, uint8_t z, const Palette &p, const VectorMap::Layers &layers)
{
    Look look;
    switch (kind) {
    case VectorMap::Contour20:
        if (layers.contours && z >= 15)
            look = {rgb(p.contour), 1};
        break;
    case VectorMap::Contour100:
        if (layers.contours && z >= 13)
            look = {rgb(z >= 15 ? p.contourIndex : p.contour), 1};
        break;
    case VectorMap::Contour500:
        if (layers.contours && z >= 11)
            look = {rgb(z >= 13 ? p.contourMain : p.contourIndex), (uint8_t)(z >= 15 ? 2 : 1)};
        break;
    case VectorMap::Stream:
        if (layers.water && z >= 13)
            look = {rgb(p.waterLine), 1};
        break;
    case VectorMap::River:
        if (layers.water)
            look = {rgb(p.waterLine), (uint8_t)(z >= 12 ? 2 : 1)};
        break;
    case VectorMap::Water:
        if (layers.water)
            look = {rgb(p.water), 1};
        break;
    case VectorMap::Coast:
        if (layers.water)
            look = {rgb(p.waterLine), (uint8_t)(z >= 9 ? 2 : 1)};
        break;
    case VectorMap::StateLine:
        if (layers.boundaries)
            look = {rgb(p.boundary), (uint8_t)(z >= 9 ? 2 : 1), 8, 3};
        break;
    case VectorMap::NationLine:
        if (layers.boundaries)
            look = {rgb(p.boundary), (uint8_t)(z >= 9 ? 3 : 2), 10, 3};
        break;
    case VectorMap::Motorway:
        if (layers.roads)
            look = {rgb(p.motorway), (uint8_t)(z >= 11 ? 3 : 2)};
        break;
    case VectorMap::Primary:
        if (layers.roads)
            look = {rgb(p.primary), (uint8_t)(z >= 13 ? 3 : z >= 10 ? 2 : 1)};
        break;
    case VectorMap::Secondary:
        if (layers.roads)
            look = {rgb(p.secondary), (uint8_t)(z >= 13 ? 2 : 1)};
        break;
    case VectorMap::Minor:
        if (layers.roads && z >= 12)
            look = {rgb(p.minor), (uint8_t)(z >= 16 ? 2 : 1)};
        break;
    case VectorMap::Service:
        if (layers.roads && z >= 15)
            look = {rgb(p.service), 1};
        break;
    case VectorMap::Track:
        if (layers.trails && z >= 13)
            look = {rgb(p.track), 1, 5, 2};
        break;
    case VectorMap::Path:
        if (layers.trails && z >= 13)
            look = {rgb(p.path), (uint8_t)(z >= 15 ? 2 : 1), 3, 2};
        break;
    case VectorMap::Rail:
        if (layers.roads && z >= 10)
            look = {rgb(p.rail), 1, 6, 3};
        break;
    case VectorMap::Boundary:
        if (layers.boundaries && z >= 10)
            look = {rgb(p.boundary), 1, 6, 2, };
        break;
    default:
        break;
    }
    return look;
}

class Reader
{
  public:
    Reader(const uint8_t *data, size_t size) : at(data), end(data + size) {}
    bool more(void) const { return ok && at < end; }
    bool good(void) const { return ok; }
    uint8_t byte(void)
    {
        if (at >= end) {
            ok = false;
            return 0;
        }
        return *at++;
    }
    uint32_t varint(void)
    {
        uint32_t value = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const uint8_t b = byte();
            value |= (uint32_t)(b & 0x7F) << shift;
            if (!(b & 0x80))
                return value;
        }
        ok = false;
        return 0;
    }
    int32_t zigzag(void)
    {
        const uint32_t v = varint();
        return (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
    }
    const uint8_t *bytes(size_t count)
    {
        if ((size_t)(end - at) < count) {
            ok = false;
            return nullptr;
        }
        const uint8_t *p = at;
        at += count;
        return p;
    }

  private:
    const uint8_t *at, *end;
    bool ok = true;
};

struct Point {
    int x, y; // sixteenths of a pixel
};

class Canvas
{
  public:
    explicit Canvas(uint16_t *pixels) : px(pixels) {}

    void fill(uint16_t color) { std::fill(px, px + c_size * c_size, color); }

    void dot(int x, int y, uint16_t color)
    {
        if ((unsigned)x < (unsigned)c_size && (unsigned)y < (unsigned)c_size)
            px[y * c_size + x] = color;
    }

    // that color over what is there, at a strength of 0 to 32
    void blend(int x, int y, uint16_t color, int strength)
    {
        if ((unsigned)x >= (unsigned)c_size || (unsigned)y >= (unsigned)c_size || strength <= 0)
            return;
        uint16_t &at = px[y * c_size + x];
        if (strength >= 32) {
            at = color;
            return;
        }
        // red and blue in the low word, green in the high one: room for the product of each
        const uint32_t over = (color | (uint32_t)color << 16) & 0x07E0F81F, under = (at | (uint32_t)at << 16) & 0x07E0F81F;
        const uint32_t mixed = ((over * (uint32_t)strength + under * (uint32_t)(32 - strength)) >> 5) & 0x07E0F81F;
        at = (uint16_t)(mixed | mixed >> 16);
    }

    /**
     * A line of that width with soft edges, in dashes when on is set. phase carries the place in the dash
     * pattern from one piece of a line to the next. Points far outside are fine: the line is cut to the tile
     * first.
     *
     * It walks the longer axis of the line a pixel at a time and colors, across it, each pixel by how much
     * of it the line covers.
     */
    void line(Point a, Point b, const Look &look, float &phase)
    {
        const int lo = -4 * 16, hi = (c_size + 4) * 16;
        // cut to the tile (Cohen-Sutherland); what lies outside costs no steps
        auto code = [&](const Point &p) { return (p.x < lo) | (p.x > hi) << 1 | (p.y < lo) << 2 | (p.y > hi) << 3; };
        int ca = code(a), cb = code(b);
        for (int guard = 0; guard < 8 && (ca | cb); guard++) {
            if (ca & cb)
                return;
            const int out = ca ? ca : cb;
            const int64_t dx = (int64_t)b.x - a.x, dy = (int64_t)b.y - a.y;
            Point p;
            if (out & 8)
                p = {(int)(a.x + dx * (hi - a.y) / dy), hi};
            else if (out & 4)
                p = {(int)(a.x + dx * (lo - a.y) / dy), lo};
            else if (out & 2)
                p = {hi, (int)(a.y + dy * (hi - a.x) / dx)};
            else
                p = {lo, (int)(a.y + dy * (lo - a.x) / dx)};
            if (out == ca) {
                a = p;
                ca = code(a);
            } else {
                b = p;
                cb = code(b);
            }
        }
        if (ca | cb)
            return;
        // the middle of pixel n is at n
        float x0 = a.x / 16.0f - 0.5f, y0 = a.y / 16.0f - 0.5f, x1 = b.x / 16.0f - 0.5f, y1 = b.y / 16.0f - 0.5f;
        const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
        if (steep) {
            std::swap(x0, y0);
            std::swap(x1, y1);
        }
        const bool backwards = x0 > x1;
        if (backwards) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        const float run = x1 - x0;
        const float slope = run > 0.001f ? (y1 - y0) / run : 0;
        // length per step along the longer axis: the root of 1 + slope^2, near enough (slope is at most 1)
        const float stretch = 1.0f + 0.4142f * slope * slope;
        const float half = look.width * 0.5f + 0.5f, reach = half * stretch, shrink = 1.0f / stretch;
        const int first = (int)floorf(x0 + 0.5f), last = (int)floorf(x1 + 0.5f);
        const uint32_t period = look.on + look.off;
        for (int u = first; u <= last; u++) {
            if (period) {
                const float along = phase + (backwards ? x1 - u : u - x0) * stretch;
                if ((uint32_t)(along > 0 ? along : 0) % period >= look.on)
                    continue;
            }
            const float middle = y0 + (u - x0) * slope;
            const int from = (int)ceilf(middle - reach), to = (int)floorf(middle + reach);
            for (int v = from; v <= to; v++) {
                const float cover = half - fabsf(v - middle) * shrink;
                const int strength = cover >= 1.0f ? 32 : (int)(cover * 32.0f);
                if (steep)
                    blend(v, u, look.color, strength);
                else
                    blend(u, v, look.color, strength);
            }
        }
        phase += run * stretch;
    }

    // an area, filled by the even-odd rule
    void area(const std::vector<Point> &ring, uint16_t color)
    {
        if (ring.size() < 3)
            return;
        int top = INT32_MAX, bottom = INT32_MIN;
        for (const Point &p : ring) {
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        top = std::max(top >> 4, 0);
        bottom = std::min(bottom >> 4, c_size - 1);
        std::vector<int> crossings;
        for (int y = top; y <= bottom; y++) {
            crossings.clear();
            const int middle = y * 16 + 8;
            for (size_t i = 0; i < ring.size(); i++) {
                const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
                if ((a.y <= middle) == (b.y <= middle))
                    continue;
                // the pixel whose middle is the first right of the crossing
                const float cross = (a.x + (float)(middle - a.y) * (b.x - a.x) / (float)(b.y - a.y)) / 16.0f;
                crossings.push_back((int)ceilf(std::max(-8.0f, std::min(cross, c_size + 8.0f)) - 0.5f));
            }
            std::sort(crossings.begin(), crossings.end());
            for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const int from = std::max(crossings[i], 0), to = std::min(crossings[i + 1], c_size);
                for (int x = from; x < to; x++)
                    px[y * c_size + x] = color;
            }
        }
    }

    template <size_t Rows> static int textWidth(const uint16_t font[95][Rows], const char *text, size_t length)
    {
        int width = 0;
        for (size_t i = 0; i < length; i++) {
            const uint8_t c = (uint8_t)text[i];
            width += font[(c >= 32 && c < 127 ? c : '?') - 32][0];
        }
        return width;
    }

    /**
     * Text turned about its middle, which is at that place (sixteenths of a pixel); the angle in 256ths of
     * a turn, clockwise on the screen. The text is first set upright in a small buffer, its rim next to it,
     * and each pixel of the map under it then takes its share of the letters that fall on it.
     */
    template <size_t Rows>
    void turned(const uint16_t font[95][Rows], Point middle, int8_t angle, const char *text, size_t length, uint16_t color, uint16_t halo)
    {
        constexpr int pad = 2;
        const int wide = textWidth(font, text, length) + 2 * pad, high = (int)Rows - 1 + 2 * pad;
        if (wide > 320)
            return;
        std::vector<uint8_t> ink((size_t)wide * high, 0), rim((size_t)wide * high, 0);
        int cx = pad;
        for (size_t i = 0; i < length; i++) {
            const uint8_t c = (uint8_t)text[i];
            const uint16_t *glyph = font[(c >= 32 && c < 127 ? c : '?') - 32];
            for (size_t row = 1; row < Rows; row++) {
                for (int col = 0; col < 16; col++) {
                    if (!(glyph[row] & (0x8000 >> col)) || cx + col >= wide - pad)
                        continue;
                    const int gx = cx + col, gy = pad + (int)row - 1;
                    ink[(size_t)gy * wide + gx] = 1;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            rim[(size_t)(gy + dy) * wide + gx + dx] = 1;
                }
            }
            cx += glyph[0];
        }
        const float turn = angle * (3.14159265f / 128.0f), co = cosf(turn), si = sinf(turn);
        const float mx = middle.x / 16.0f, my = middle.y / 16.0f;
        const float reachX = (fabsf(co) * wide + fabsf(si) * high) * 0.5f + 1, reachY = (fabsf(si) * wide + fabsf(co) * high) * 0.5f + 1;
        const int left = std::max(0, (int)floorf(mx - reachX)), right = std::min(c_size - 1, (int)ceilf(mx + reachX));
        const int top = std::max(0, (int)floorf(my - reachY)), bottom = std::min(c_size - 1, (int)ceilf(my + reachY));
        // how much of a buffer lies at a place between its cells (0 to 1)
        auto share = [&](const std::vector<uint8_t> &from, float u, float v) {
            const int iu = (int)floorf(u), iv = (int)floorf(v);
            if (iu < 0 || iv < 0 || iu >= wide - 1 || iv >= high - 1)
                return 0.0f;
            const float fu = u - iu, fv = v - iv;
            const uint8_t *at = &from[(size_t)iv * wide + iu];
            return (at[0] * (1 - fu) + at[1] * fu) * (1 - fv) + (at[wide] * (1 - fu) + at[wide + 1] * fu) * fv;
        };
        for (int y = top; y <= bottom; y++) {
            for (int x = left; x <= right; x++) {
                const float dx = x + 0.5f - mx, dy = y + 0.5f - my;
                // the place in the buffer, counted between the middles of its cells
                const float u = dx * co + dy * si + wide * 0.5f - 0.5f, v = -dx * si + dy * co + high * 0.5f - 0.5f;
                const float around = share(rim, u, v);
                if (around <= 0)
                    continue;
                blend(x, y, halo, (int)(std::min(1.0f, around * 1.5f) * 32.0f));
                // a little firmer than its share: thin strokes would grey out otherwise
                blend(x, y, color, (int)(std::min(1.0f, share(ink, u, v) * 1.4f) * 32.0f));
            }
        }
    }

    // text with its left top corner there; halo: a rim in that color so that it reads on top of lines
    template <size_t Rows> void text(const uint16_t font[95][Rows], int x, int y, const char *text, size_t length, uint16_t color, uint16_t halo)
    {
        for (int pass = 0; pass < 2; pass++) {
            int cx = x;
            for (size_t i = 0; i < length; i++) {
                const uint8_t c = (uint8_t)text[i];
                const uint16_t *glyph = font[(c >= 32 && c < 127 ? c : '?') - 32];
                for (size_t row = 1; row < Rows; row++) {
                    const uint16_t bits = glyph[row];
                    if (!bits)
                        continue;
                    for (int col = 0; col < 16; col++) {
                        if (!(bits & (0x8000 >> col)))
                            continue;
                        const int gx = cx + col, gy = y + (int)row - 1;
                        if (pass == 0) {
                            for (int dy = -1; dy <= 1; dy++)
                                for (int dx = -1; dx <= 1; dx++)
                                    dot(gx + dx, gy + dy, halo);
                        } else {
                            dot(gx, gy, color);
                        }
                    }
                }
                cx += glyph[0];
            }
        }
    }

  private:
    uint16_t *px;
};

uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

// past a feature that is not drawn
void skip(Reader &in, uint8_t kind)
{
    if (kind >= 64) {
        in.zigzag();
        in.zigzag();
        in.bytes(3);
        in.varint();
        in.bytes(in.byte());
        return;
    }
    if (kind >= VectorMap::Contour20 && kind <= VectorMap::Contour500)
        in.varint();
    const uint32_t count = in.varint();
    for (uint32_t i = 0; i < count && in.good(); i++) {
        in.varint();
        in.varint();
    }
}
} // namespace

bool VectorMap::setColor(bool dark, const char *name, uint32_t color)
{
    uint32_t *values = &(dark ? c_dark : c_light).ground;
    for (size_t i = 0; i < sizeof(c_colorNames) / sizeof(c_colorNames[0]); i++) {
        if (strcmp(name, c_colorNames[i]) == 0) {
            values[i] = color & 0xFFFFFF;
            return true;
        }
    }
    return false;
}

bool VectorMap::open(Read read)
{
    close();
    reader = std::move(read);
    uint8_t head[28];
    if (!reader || !reader(0, head, sizeof(head)) || memcmp(head, "JMVM", 4) != 0 || get16(head + 4) != 4)
        return false;
    const uint16_t count = get16(head + 6);
    if (count == 0 || count > 8)
        return false;
    south = (int32_t)get32(head + 8);
    west = (int32_t)get32(head + 12);
    north = (int32_t)get32(head + 16);
    east = (int32_t)get32(head + 20);
    lowZoom = head[24];
    pieceOf = get16(head + 26);
    highZoom = std::min<uint8_t>(head[25] + c_overZoom, 19);
    for (uint16_t i = 0; i < count; i++) {
        uint8_t raw[20];
        if (!reader(28 + 20 * i, raw, sizeof(raw)))
            return false;
        Level level;
        level.cellZoom = raw[0];
        level.firstZoom = raw[1];
        level.lastZoom = raw[2];
        level.x0 = get32(raw + 4);
        level.y0 = get32(raw + 8);
        level.columns = get16(raw + 12);
        level.rows = get16(raw + 14);
        level.directory = get32(raw + 16);
        if (level.cellZoom > 16 || level.firstZoom < level.cellZoom || level.lastZoom < level.firstZoom || !level.columns || !level.rows)
            return false;
        levels.push_back(level);
    }
    opened = true;
    return true;
}

void VectorMap::close(void)
{
    opened = false;
    levels.clear();
    for (Cell &c : cells) {
        c.level = 0xFF;
        std::vector<uint8_t>().swap(c.data);
    }
    for (Cell &c : names) {
        c.level = 0xFF;
        std::vector<uint8_t>().swap(c.data);
    }
    for (Entry &e : entries)
        e.level = 0xFF;
}

void VectorMap::center(double &latitude, double &longitude) const
{
    latitude = (south * 0.5 + north * 0.5) * 1e-7;
    longitude = (west * 0.5 + east * 0.5) * 1e-7;
}

/**
 * What the directory says about a cell: where it is and how long. The last few are kept: maps that share a
 * border ask each other for every tile there.
 */
bool VectorMap::entryOf(uint8_t level, uint32_t x, uint32_t y, uint8_t *entry)
{
    const Level &l = levels[level];
    if (x < l.x0 || y < l.y0 || x >= l.x0 + l.columns || y >= l.y0 + l.rows)
        return false;
    for (const Entry &e : entries) {
        if (e.level == level && e.x == x && e.y == y) {
            memcpy(entry, e.raw, 12);
            return true;
        }
    }
    const uint32_t index = (y - l.y0) * l.columns + (x - l.x0);
    if (!reader(l.directory + 12 * index, entry, 12))
        return false;
    Entry &e = entries[nextEntry++ % c_entries];
    e.level = level;
    e.x = x;
    e.y = y;
    memcpy(e.raw, entry, 12);
    return true;
}

int VectorMap::levelOf(uint8_t z) const
{
    // the level that serves this zoom; past the last zoom of the finest one, that one
    int which = -1;
    for (size_t i = 0; i < levels.size(); i++) {
        if (z >= levels[i].firstZoom && z <= levels[i].lastZoom)
            which = (int)i;
    }
    if (which < 0) {
        for (size_t i = 0; i < levels.size(); i++) {
            if (z > levels[i].lastZoom && (which < 0 || levels[i].cellZoom > levels[which].cellZoom))
                which = (int)i;
        }
    }
    return which;
}

bool VectorMap::whole(uint8_t z) const
{
    const int which = opened ? levelOf(z) : -1;
    return which >= 0 && levels[which].cellZoom >= c_wholeCells;
}

uint32_t VectorMap::weight(uint8_t z, uint32_t x, uint32_t y, int dx, int dy)
{
    const int which = opened ? levelOf(z) : -1;
    if (which < 0 || z < levels[which].cellZoom)
        return 0;
    const int shift = z - levels[which].cellZoom;
    // a map draws nothing into a tile whose own cell lies outside of its rows and columns (see render()),
    // names of the cells next to it neither: it must not be the one to draw those
    const Level &level = levels[which];
    const uint32_t cx = x >> shift, cy = y >> shift;
    if (cx < level.x0 || cy < level.y0 || cx >= level.x0 + level.columns || cy >= level.y0 + level.rows)
        return 0;
    uint8_t entry[12];
    if (!entryOf((uint8_t)which, cx + dx, cy + dy, entry))
        return 0;
    return get32(entry + 4) + get32(entry + 8);
}

uint32_t VectorMap::collect(uint8_t kind, double mx, double my, int dx, int dy, std::vector<double> &out)
{
    if (!opened || kind >= 64)
        return 0;
    size_t which = 0;
    for (size_t i = 1; i < levels.size(); i++) {
        if (levels[i].cellZoom > levels[which].cellZoom)
            which = i;
    }
    const double across = (double)(1u << levels[which].cellZoom);
    if (mx < 0 || my < 0 || mx >= 1 || my >= 1)
        return 0;
    const uint32_t cx = (uint32_t)(mx * across) + dx, cy = (uint32_t)(my * across) + dy;
    uint8_t entry[12];
    if (!entryOf((uint8_t)which, cx, cy, entry))
        return 0;
    const uint32_t weight = get32(entry + 4) + get32(entry + 8);
    const std::vector<uint8_t> *data = weight ? cell((uint8_t)which, cx, cy, false) : nullptr;
    if (!data)
        return weight;
    Reader in(data->data(), data->size());
    while (in.more()) {
        const uint8_t found = in.byte();
        if (found != kind) {
            skip(in, found);
            continue;
        }
        const uint32_t count = in.varint();
        if (count > 100000)
            break;
        int32_t ux = 0, uy = 0;
        double beforeX = 0, beforeY = 0;
        for (uint32_t i = 0; i < count && in.good(); i++) {
            ux += in.zigzag();
            uy += in.zigzag();
            const double x = (cx + ux / (double)c_extent) / across, y = (cy + uy / (double)c_extent) / across;
            if (i) {
                out.push_back(beforeX);
                out.push_back(beforeY);
                out.push_back(x);
                out.push_back(y);
            }
            beforeX = x;
            beforeY = y;
        }
    }
    return weight;
}

/**
 * The features of a cell, from the few that are kept or from the file. Null when the file has none there.
 */
const std::vector<uint8_t> *VectorMap::cell(uint8_t level, uint32_t x, uint32_t y, bool wantNames)
{
    const Level &l = levels[level];
    if (x < l.x0 || y < l.y0 || x >= l.x0 + l.columns || y >= l.y0 + l.rows)
        return nullptr;
    Cell *kept = wantNames ? names : cells;
    const size_t count = wantNames ? c_names : c_cells;
    tick++;
    Cell *oldest = &kept[0];
    for (size_t i = 0; i < count; i++) {
        Cell &c = kept[i];
        if (c.level == level && c.x == x && c.y == y) {
            c.used = tick;
            return c.data.empty() ? nullptr : &c.data;
        }
        if (c.used < oldest->used)
            oldest = &c;
    }
    uint8_t entry[12];
    if (!entryOf(level, x, y, entry))
        return nullptr;
    const uint32_t lines = get32(entry + 4);
    const uint32_t offset = get32(entry) + (wantNames ? lines : 0), length = wantNames ? get32(entry + 8) : lines;
    oldest->level = level;
    oldest->x = x;
    oldest->y = y;
    oldest->used = tick;
    oldest->data.clear();
    if (length == 0 || length > c_maxCell)
        return nullptr;
    oldest->data.resize(length);
    if (!reader(offset, oldest->data.data(), length)) {
        oldest->data.clear();
        return nullptr;
    }
    return &oldest->data;
}

bool VectorMap::render(uint8_t z, uint32_t x, uint32_t y, uint16_t *pixels, bool dark, const Layers &layers, uint8_t parts,
                       const Mine *mine)
{
    if (!opened || !pixels || z < lowZoom || z > highZoom)
        return false;
    const int which = levelOf(z);
    if (which < 0)
        return false;
    const Level &level = levels[which];
    const int shift = z - level.cellZoom; // the tile is this many halvings of its cell
    if (shift < 0 || shift > 8)
        return false;
    const uint32_t cx = x >> shift, cy = y >> shift;
    if (cx < level.x0 || cy < level.y0 || cx >= level.x0 + level.columns || cy >= level.y0 + level.rows)
        return false;

    const Palette &palette = dark ? c_dark : c_light;
    Canvas canvas(pixels);
    if (parts & Ground)
        canvas.fill(rgb(palette.ground));

    // where the tile begins in its cell, in units; a unit is 2^shift / 16 pixels
    const int32_t originX = (int32_t)((x - (cx << shift)) * (uint32_t)(c_extent >> shift));
    const int32_t originY = (int32_t)((y - (cy << shift)) * (uint32_t)(c_extent >> shift));
    const int64_t scale = (int64_t)1 << shift;
    // units from the tile's corner times 2^shift: sixteenths of a pixel
    auto toFine = [&](int32_t ux, int32_t uy) {
        return Point{(int)(((int64_t)ux - originX) * scale), (int)(((int64_t)uy - originY) * scale)};
    };
    auto floor16 = [](int v) { return v >= 0 ? v / 16 : -((15 - v) / 16); };

    const std::vector<uint8_t> *data = (parts & Lines) && (!mine || (*mine)(0, 0)) ? cell((uint8_t)which, cx, cy, false) : nullptr;
    std::vector<Point> points;
    if (data) {
        // the builder has put the features of a cell in the order they are drawn in, the last on top
        Look looks[64];
        for (uint8_t kind = 0; kind < 64; kind++)
            looks[kind] = lookOf(kind, z, palette, layers);
        Reader in(data->data(), data->size());
        while (in.more()) {
            const uint8_t kind = in.byte();
            if (kind >= 64 || !looks[kind].width) {
                skip(in, kind);
                continue;
            }
            const Look &look = looks[kind];
            if (kind >= Contour20 && kind <= Contour500)
                in.varint();
            const uint32_t count = in.varint();
            if (count > 100000)
                break;
            int32_t ux = 0, uy = 0;
            if (kind == Water) {
                points.clear();
                for (uint32_t i = 0; i < count && in.good(); i++) {
                    ux += in.zigzag();
                    uy += in.zigzag();
                    points.push_back(toFine(ux, uy));
                }
                if (in.good()) {
                    canvas.area(points, look.color);
                    // and a soft rim
                    float phase = 0;
                    for (size_t i = 0; i < points.size(); i++)
                        canvas.line(points[i], points[(i + 1) % points.size()], look, phase);
                }
                continue;
            }
            float phase = 0;
            Point before{0, 0};
            for (uint32_t i = 0; i < count && in.good(); i++) {
                ux += in.zigzag();
                uy += in.zigzag();
                const Point now = toFine(ux, uy);
                if (i)
                    canvas.line(before, now, look, phase);
                before = now;
            }
        }
    }

    if (layers.names && (parts & Names)) {
        // names: those of the cell, and those of the cells next to it that reach into this tile
        const int32_t reach = (int32_t)((c_nameReach * 16) / scale) + 1; // in units
        const int32_t span = c_extent >> shift;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx < 0 && originX > reach)
                    continue;
                if (dx > 0 && originX + span < c_extent - reach)
                    continue;
                if (dy < 0 && originY > reach)
                    continue;
                if (dy > 0 && originY + span < c_extent - reach)
                    continue;
                if (mine && !(*mine)(dx, dy))
                    continue;
                const std::vector<uint8_t> *from = cell((uint8_t)which, cx + dx, cy + dy, true);
                if (!from)
                    continue;
                Reader in(from->data(), from->size());
                while (in.more()) {
                    const uint8_t kind = in.byte();
                    if (kind < 64) {
                        skip(in, kind);
                        continue;
                    }
                    const int32_t ux = in.zigzag() + dx * c_extent, uy = in.zigzag() + dy * c_extent;
                    const uint8_t fromZoom = in.byte(), toZoom = in.byte();
                    const int8_t angle = (int8_t)in.byte();
                    const uint32_t number = in.varint();
                    const uint8_t length = in.byte();
                    const char *text = (const char *)in.bytes(length);
                    if (!in.good() || !text)
                        break;
                    // the builder has settled at which zooms a name has room, and how it lies there
                    if (z < fromZoom || z > toZoom || (kind == ContourName && !layers.contours))
                        continue;
                    const Point fine = toFine(ux, uy);
                    const Point at{floor16(fine.x), floor16(fine.y)};
                    if (at.x < -c_nameReach || at.y < -c_nameReach || at.x > c_size + c_nameReach || at.y > c_size + c_nameReach)
                        continue;
                    const uint16_t ink = rgb(kind == WaterName ? palette.waterLine : palette.text), rim = rgb(palette.halo);
                    const bool large = (kind == City || kind == Town) && z >= 10;
                    if (kind == ContourName) {
                        // the height of a contour line, lying in the line
                        char line[12];
                        const int n = snprintf(line, sizeof(line), "%u", (unsigned)number);
                        canvas.turned(vectormapfont::small, fine, angle, line, (size_t)n, rgb(palette.contourMain), rim);
                    } else if (angle != 0 && !large) {
                        canvas.turned(vectormapfont::small, fine, angle, text, length, ink, rim);
                    } else if (kind == Peak) {
                        // a small triangle where the top is, the name under it
                        for (int row = 0; row < 4; row++)
                            for (int col = -row; col <= row; col++)
                                canvas.dot(at.x + col, at.y - 2 + row, ink);
                        char line[64];
                        int n = snprintf(line, sizeof(line), "%.*s", (int)length, text);
                        if (number && z >= 14)
                            n += snprintf(line + n, sizeof(line) - n, " %u'", (unsigned)number);
                        const int w = Canvas::textWidth(vectormapfont::small, line, (size_t)n);
                        canvas.text(vectormapfont::small, at.x - w / 2, at.y + 4, line, (size_t)n, ink, rim);
                    } else if (large) {
                        const int w = Canvas::textWidth(vectormapfont::large, text, length);
                        canvas.text(vectormapfont::large, at.x - w / 2, at.y - vectormapfont::largeHeight / 2, text, length, ink, rim);
                    } else {
                        const int w = Canvas::textWidth(vectormapfont::small, text, length);
                        canvas.text(vectormapfont::small, at.x - w / 2, at.y - vectormapfont::smallHeight / 2, text, length, ink, rim);
                    }
                }
            }
        }
    }
    return true;
}
