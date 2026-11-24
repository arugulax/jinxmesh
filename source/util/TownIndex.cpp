#include "util/TownIndex.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{

constexpr uint32_t c_headerSize = 64;
constexpr uint32_t c_recordSize = 40;
constexpr uint16_t c_version = 1;
constexpr size_t c_stepBuffer = 256;     // steps of a ring read at a time
constexpr size_t c_maxList = 4096;       // records of one cell; more than that is a broken file
constexpr size_t c_maxAround = 1500;     // records looked at for the towns around a place
constexpr uint32_t c_maxWalk = 600000;   // points one lookup may read; the national file needs a few thousand
constexpr float c_metersPerUnit = 1.11319f; // 1e-5 degrees of latitude
constexpr int32_t c_marginPerMeter = 200;   // 1e-7 degrees a box grows per meter of accuracy, generous in longitude

uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t i32(const uint8_t *p)
{
    return (int32_t)u32(p);
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/**
 * How name starts compared with start, case ignored: negative when it sorts before, 0 when it starts with it.
 */
int compareStart(const std::string &name, const std::string &start)
{
    for (size_t i = 0; i < start.size(); i++) {
        if (i >= name.size())
            return -1;
        const unsigned char a = (unsigned char)lower(name[i]), b = (unsigned char)lower(start[i]);
        if (a != b)
            return a < b ? -1 : 1;
    }
    return 0;
}

} // namespace

TownIndex::State TownIndex::open(Read reader)
{
    close();
    read = std::move(reader);
    uint8_t raw[c_headerSize];
    if (!read || !read(0, raw, sizeof(raw)) || memcmp(raw, "KBTN", 4) != 0) {
        status = State::NotTownData;
        return status;
    }
    if (u16(raw + 4) != c_version) {
        status = State::OtherVersion;
        return status;
    }
    header.accuracy = u16(raw + 6);
    header.west = i32(raw + 8);
    header.south = i32(raw + 12);
    header.east = i32(raw + 16);
    header.north = i32(raw + 20);
    header.cell = u32(raw + 24);
    header.columns = u16(raw + 28);
    header.rows = u16(raw + 30);
    header.count = u32(raw + 32);
    header.grid = u32(raw + 36);
    header.records = u32(raw + 40);
    header.names = u32(raw + 44);
    header.strings = u32(raw + 48);
    header.rings = u32(raw + 52);
    header.length = u32(raw + 56);
    header.date = u32(raw + 60);
    // The parts in their order and inside the file, and the file all there (an upload that is still running has
    // the header and not yet the end): everything later relies on that.
    uint8_t last;
    const bool sound = header.cell && header.columns && header.rows && header.count && header.count < 10000000 &&
                       header.grid >= c_headerSize && header.grid < header.records && header.records < header.names &&
                       header.names < header.strings && header.strings < header.rings && header.rings < header.length &&
                       (header.names - header.records) / c_recordSize >= header.count && read(header.length - 1, &last, 1);
    status = sound ? State::Ok : State::NotTownData;
    return status;
}

void TownIndex::close(void)
{
    status = State::Closed;
    header = Header();
    read = nullptr;
}

bool TownIndex::record(uint32_t number, Record &out)
{
    uint8_t raw[c_recordSize];
    if (number >= header.count || !read(header.records + number * c_recordSize, raw, sizeof(raw)))
        return false;
    out.west = i32(raw);
    out.south = i32(raw + 4);
    out.east = i32(raw + 8);
    out.north = i32(raw + 12);
    out.labelLon = i32(raw + 16);
    out.labelLat = i32(raw + 20);
    out.name = u32(raw + 24);
    out.area = u32(raw + 28);
    out.ring = u32(raw + 32);
    out.ringCount = u16(raw + 36);
    out.kind = raw[38];
    return true;
}

std::string TownIndex::text(uint32_t offset)
{
    char raw[97] = {0};
    const uint32_t at = header.strings + offset;
    if (at >= header.rings)
        return "";
    // the strings end where the rings start: never read past them
    const uint32_t length = std::min<uint32_t>(sizeof(raw) - 1, header.rings - at);
    if (!read(at, raw, length))
        return "";
    return raw;
}

TownIndex::Entry TownIndex::entry(uint32_t number, const Record &rec)
{
    Entry out;
    out.number = number;
    out.name = text(rec.name);
    out.area = text(rec.area);
    out.latitude = rec.labelLat;
    out.longitude = rec.labelLon;
    out.kind = rec.kind;
    return out;
}

bool TownIndex::cellList(int32_t column, int32_t row, std::vector<uint32_t> &out)
{
    if (column < 0 || row < 0 || column >= header.columns || row >= header.rows)
        return true; // nothing there
    uint8_t raw[8];
    const uint32_t cell = (uint32_t)row * header.columns + (uint32_t)column;
    if (!read(header.grid + 4 * cell, raw, sizeof(raw)))
        return false;
    const uint32_t from = u32(raw), to = u32(raw + 4);
    if (to < from || to - from > c_maxList)
        return false;
    const uint32_t lists = header.grid + 4 * ((uint32_t)header.columns * header.rows + 1);
    std::vector<uint8_t> bytes(4 * (to - from));
    if (!bytes.empty() && !read(lists + 4 * from, bytes.data(), (uint32_t)bytes.size()))
        return false;
    for (uint32_t i = 0; i < to - from; i++)
        out.push_back(u32(bytes.data() + 4 * i));
    return true;
}

/**
 * Goes around all rings of a record once: is the point (in 1e-5 degrees) inside, by counting the edges a line
 * running east from it crosses, and how close does an edge come, in meters. scale is the cosine of the latitude.
 */
bool TownIndex::walk(const Record &rec, int32_t x, int32_t y, float scale, bool &inside, float &nearest)
{
    if (rec.ring >= header.length - header.rings)
        return false;
    uint32_t at = header.rings + rec.ring;
    bool odd = false;
    float best = nearest * nearest / (c_metersPerUnit * c_metersPerUnit);
    uint8_t raw[4 * c_stepBuffer];
    for (uint16_t ring = 0; ring < rec.ringCount; ring++) {
        if (header.length - at < 12 || !read(at, raw, 12))
            return false;
        const uint32_t points = u32(raw);
        const int32_t firstX = i32(raw + 4), firstY = i32(raw + 8);
        at += 12;
        // no more points than the rest of the file can hold, and no more work than a lookup may cost
        if (points < 3 || points - 1 > (header.length - at) / 4 || points > walkLeft)
            return false;
        walkLeft -= points;
        int32_t x1 = firstX, y1 = firstY;
        uint32_t buffered = 0, used = 0;
        for (uint32_t i = 0; i < points; i++) {
            int32_t x2, y2;
            if (i + 1 < points) {
                if (used == buffered) {
                    buffered = std::min<uint32_t>(c_stepBuffer, points - 1 - i);
                    if (!read(at, raw, 4 * buffered))
                        return false;
                    at += 4 * buffered;
                    used = 0;
                }
                x2 = (int32_t)((uint32_t)x1 + (uint32_t)(int32_t)(int16_t)u16(raw + 4 * used)); // wraps, never overflows
                y2 = (int32_t)((uint32_t)y1 + (uint32_t)(int32_t)(int16_t)u16(raw + 4 * used + 2));
                used++;
            } else {
                x2 = firstX; // the ring closes without repeating its first point
                y2 = firstY;
            }
            if ((y1 > y) != (y2 > y)) {
                const int64_t side = ((int64_t)x2 - x1) * ((int64_t)y - y1) - ((int64_t)x - x1) * ((int64_t)y2 - y1);
                if ((side > 0) == (y2 > y1))
                    odd = !odd;
            }
            const float ax = ((float)x1 - (float)x) * scale, ay = (float)y1 - (float)y;
            const float dx = ((float)x2 - (float)x1) * scale, dy = (float)y2 - (float)y1;
            const float length = dx * dx + dy * dy;
            const float t = length > 0 ? std::clamp(-(ax * dx + ay * dy) / length, 0.0f, 1.0f) : 0.0f;
            const float ex = ax + t * dx, ey = ay + t * dy;
            best = std::min(best, ex * ex + ey * ey);
            x1 = x2;
            y1 = y2;
        }
    }
    inside = odd;
    nearest = sqrtf(best) * c_metersPerUnit;
    return true;
}

TownIndex::Place TownIndex::find(int32_t latitude, int32_t longitude)
{
    Place place;
    if (status != State::Ok || longitude < header.west || longitude > header.east || latitude < header.south ||
        latitude > header.north)
        return place;
    place.covered = true;

    std::vector<uint32_t> list;
    if (!cellList((int32_t)((uint32_t)(longitude - header.west) / header.cell),
                  (int32_t)((uint32_t)(latitude - header.south) / header.cell), list))
        return place;

    // floor, as the builder's reference does it
    const int32_t x = (int32_t)floor(longitude / 100.0), y = (int32_t)floor(latitude / 100.0);
    const float scale = cosf((float)(latitude * 1e-7 * M_PI / 180.0));
    const int32_t margin = (int32_t)header.accuracy * c_marginPerMeter;

    walkLeft = c_maxWalk;
    uint32_t in = UINT32_MAX, closest = UINT32_MAX, second = UINT32_MAX;
    float closestMeters = 1e9f, secondMeters = 1e9f; // the two nearest lines of different records
    Record inRecord{}, closestRecord{}, secondRecord{};
    for (const uint32_t number : list) {
        Record rec;
        if (!record(number, rec))
            continue;
        if (longitude < (int64_t)rec.west - margin || longitude > (int64_t)rec.east + margin ||
            latitude < (int64_t)rec.south - margin || latitude > (int64_t)rec.north + margin)
            continue;
        if (!walkLeft)
            break; // a file that makes a lookup this much work is not read any further
        bool inside = false;
        float meters = 1e9f;
        if (!walk(rec, x, y, scale, inside, meters))
            continue;
        if (inside && in == UINT32_MAX) {
            in = number;
            inRecord = rec;
        }
        if (meters < closestMeters) {
            second = closest;
            secondMeters = closestMeters;
            secondRecord = closestRecord;
            closest = number;
            closestMeters = meters;
            closestRecord = rec;
        } else if (meters < secondMeters) {
            second = number;
            secondMeters = meters;
            secondRecord = rec;
        }
    }
    // thinning the lines leaves slivers between two towns: there the nearest one counts
    if (in == UINT32_MAX && closest != UINT32_MAX && closestMeters <= 2.0f * header.accuracy) {
        in = closest;
        inRecord = closestRecord;
    }
    if (in != UINT32_MAX)
        place.town = entry(in, inRecord);
    // the nearest line of another town
    if (closest != UINT32_MAX && closest != in && closestMeters <= header.accuracy)
        place.neighbor = entry(closest, closestRecord);
    else if (second != UINT32_MAX && second != in && closest == in && secondMeters <= header.accuracy)
        place.neighbor = entry(second, secondRecord);
    return place;
}

std::vector<TownIndex::Entry> TownIndex::around(int32_t latitude, int32_t longitude, size_t most, uint32_t except)
{
    std::vector<Entry> out;
    if (status != State::Ok || !most)
        return out;
    const int32_t column = (int32_t)floor(((double)longitude - header.west) / header.cell);
    const int32_t row = (int32_t)floor(((double)latitude - header.south) / header.cell);
    std::vector<uint32_t> list;
    for (int32_t dr = -1; dr <= 1; dr++)
        for (int32_t dc = -1; dc <= 1; dc++)
            cellList(column + dc, row + dr, list);
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    if (list.size() > c_maxAround)
        list.resize(c_maxAround);

    struct Candidate {
        float distance;
        uint32_t number;
        Record record;
    };
    std::vector<Candidate> candidates;
    const float scale = cosf((float)(latitude * 1e-7 * M_PI / 180.0));
    for (const uint32_t number : list) {
        Record rec;
        if (number == except || !record(number, rec))
            continue;
        const float dx = ((float)rec.labelLon - (float)longitude) * scale, dy = (float)rec.labelLat - (float)latitude;
        candidates.push_back({dx * dx + dy * dy, number, rec});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });
    for (size_t i = 0; i < candidates.size() && i < most; i++)
        out.push_back(entry(candidates[i].number, candidates[i].record));
    return out;
}

std::vector<TownIndex::Entry> TownIndex::search(const std::string &start, size_t most)
{
    std::vector<Entry> out;
    if (status != State::Ok || start.empty())
        return out;
    const auto numberAt = [this](uint32_t index, uint32_t &number) {
        uint8_t raw[4];
        if (!read(header.names + 4 * index, raw, sizeof(raw)))
            return false;
        number = u32(raw);
        return true;
    };
    // the first name that does not sort before the letters
    uint32_t low = 0, high = header.count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        uint32_t number;
        Record rec;
        if (!numberAt(middle, number) || !record(number, rec))
            return out;
        if (compareStart(text(rec.name), start) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    for (; low < header.count && out.size() < most; low++) {
        uint32_t number;
        Record rec;
        if (!numberAt(low, number) || !record(number, rec))
            break;
        Entry found = entry(number, rec);
        if (compareStart(found.name, start) != 0)
            break;
        out.push_back(std::move(found));
    }
    return out;
}
