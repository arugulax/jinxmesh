#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * Reader for the town data, /geo/towns.bin on the card: which town a point is in, which towns are around, and
 * towns by the first letters of their name. The format and its builder (kbview-harness/maps/towns.py) are
 * described in kbview-harness/docs/MAP-PACKS.md, section 5.
 *
 * The file is never loaded whole: everything goes through a function that reads a stretch of it. No display
 * code in here; the unit tests build a small file in memory.
 */
class TownIndex
{
  public:
    // reads length bytes at offset into buffer; false when that is not possible
    using Read = std::function<bool(uint32_t offset, void *buffer, uint32_t length)>;

    enum class State : uint8_t { Closed, Ok, NotTownData, OtherVersion };
    enum Kind : uint8_t { Town = 0, Statistical = 1, Water = 2, Unorganized = 3 };

    struct Entry {
        uint32_t number = UINT32_MAX; // of the record
        std::string name;             // "Montpelier"
        std::string area;             // "Washington County, VT"
        int32_t latitude = 0;         // of the label point, degrees times 1e7
        int32_t longitude = 0;
        uint8_t kind = Town;
        bool valid(void) const { return number != UINT32_MAX; }
    };

    struct Place {
        bool covered = false; // the point lies in the box of the data
        Entry town;           // not valid: no town there (water, another country)
        Entry neighbor;       // valid: the point is within the data's accuracy of the line to this town
    };

    State open(Read read);
    void close(void);
    State state(void) const { return status; }
    uint32_t count(void) const { return header.count; }
    uint16_t accuracy(void) const { return header.accuracy; } // meters
    uint32_t buildDate(void) const { return header.date; }    // yyyymmdd

    /**
     * The town of a position given in degrees times 1e7.
     */
    Place find(int32_t latitude, int32_t longitude);

    /**
     * The towns whose label points are closest to a position, nearest first; without the one numbered except.
     */
    std::vector<Entry> around(int32_t latitude, int32_t longitude, size_t most, uint32_t except = UINT32_MAX);

    /**
     * Towns whose name starts with these letters, case ignored (ASCII), in the order of the names.
     */
    std::vector<Entry> search(const std::string &start, size_t most);

  private:
    struct Header {
        uint16_t accuracy = 0;
        int32_t west = 0, south = 0, east = 0, north = 0;
        uint32_t cell = 0;
        uint16_t columns = 0, rows = 0;
        uint32_t count = 0, grid = 0, records = 0, names = 0, strings = 0, rings = 0, length = 0, date = 0;
    };
    struct Record {
        int32_t west, south, east, north, labelLon, labelLat;
        uint32_t name, area, ring;
        uint16_t ringCount;
        uint8_t kind;
    };

    bool record(uint32_t number, Record &out);
    std::string text(uint32_t offset);
    Entry entry(uint32_t number, const Record &record);
    bool cellList(int32_t column, int32_t row, std::vector<uint32_t> &out);
    bool walk(const Record &record, int32_t x, int32_t y, float scale, bool &inside, float &nearest);

    Read read;
    uint32_t walkLeft = 0; // points the lookup that is running may still read
    State status = State::Closed;
    Header header;
};
