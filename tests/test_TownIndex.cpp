// TownIndex against a file of two towns built here. The reader was also compared with the reference lookup of
// kbview-harness/maps/towns.py on 5,000 points of the national file (2026-11-19): no difference.
#include "util/TownIndex.h"
#include <cstring>
#include <doctest/doctest.h>
#include <vector>

namespace
{

struct File {
    std::vector<uint8_t> bytes;
    void u16(uint16_t v)
    {
        bytes.push_back((uint8_t)v);
        bytes.push_back((uint8_t)(v >> 8));
    }
    void u32(uint32_t v)
    {
        u16((uint16_t)v);
        u16((uint16_t)(v >> 16));
    }
    void i32(int32_t v) { u32((uint32_t)v); }
    void text(const char *s) { bytes.insert(bytes.end(), s, s + strlen(s) + 1); }
};

/**
 * Two squares of 0.1 degrees side by side at 44 N: "Alder" west of -72.5, "birch" east of it, in one grid cell.
 */
std::vector<uint8_t> twoTowns(uint16_t version = 1)
{
    const int32_t west = -726000000, south = 440000000, east = -724000000, north = 441000000, middle = -725000000;
    File grid, records, names, strings, rings;
    // one cell that lists both
    grid.u32(0);
    grid.u32(2);
    grid.u32(0);
    grid.u32(1);
    const uint32_t alder = 0;
    strings.text("Alder");
    const uint32_t area = (uint32_t)strings.bytes.size();
    strings.text("Maple County, VT");
    const uint32_t birch = (uint32_t)strings.bytes.size();
    strings.text("birch");
    uint32_t ringAt[2];
    for (int i = 0; i < 2; i++) {
        ringAt[i] = (uint32_t)rings.bytes.size();
        const int32_t x = (i ? middle : west) / 100, y = south / 100;
        rings.u32(4);
        rings.i32(x);
        rings.i32(y);
        const int16_t steps[3][2] = {{10000, 0}, {0, 10000}, {-10000, 0}};
        for (const auto &step : steps) {
            rings.u16((uint16_t)step[0]);
            rings.u16((uint16_t)step[1]);
        }
    }
    for (int i = 0; i < 2; i++) {
        const int32_t left = i ? middle : west;
        records.i32(left);
        records.i32(south);
        records.i32(left + 1000000);
        records.i32(north);
        records.i32(left + 500000);
        records.i32(south + 500000);
        records.u32(i ? birch : alder);
        records.u32(area);
        records.u32(ringAt[i]);
        records.u16(1);
        records.bytes.push_back(0);
        records.bytes.push_back(0);
    }
    names.u32(0);
    names.u32(1);

    File file;
    file.bytes = {'K', 'B', 'T', 'N'};
    file.u16(version);
    file.u16(150);
    file.i32(west);
    file.i32(south);
    file.i32(east);
    file.i32(north);
    file.u32(2000000);
    file.u16(1);
    file.u16(1);
    file.u32(2);
    const uint32_t gridAt = 64, recordsAt = gridAt + (uint32_t)grid.bytes.size();
    const uint32_t namesAt = recordsAt + (uint32_t)records.bytes.size(), stringsAt = namesAt + (uint32_t)names.bytes.size();
    const uint32_t ringsAt = stringsAt + (uint32_t)strings.bytes.size();
    file.u32(gridAt);
    file.u32(recordsAt);
    file.u32(namesAt);
    file.u32(stringsAt);
    file.u32(ringsAt);
    file.u32(ringsAt + (uint32_t)rings.bytes.size());
    file.u32(20261119);
    for (const File *part : {&grid, &records, &names, &strings, &rings})
        file.bytes.insert(file.bytes.end(), part->bytes.begin(), part->bytes.end());
    return file.bytes;
}

TownIndex::Read reader(const std::vector<uint8_t> &bytes)
{
    return [&bytes](uint32_t at, void *buffer, uint32_t length) {
        if ((size_t)at + length > bytes.size())
            return false;
        memcpy(buffer, bytes.data() + at, length);
        return true;
    };
}

} // namespace

TEST_CASE("TownIndex finds the town of a point")
{
    const std::vector<uint8_t> bytes = twoTowns();
    TownIndex towns;
    REQUIRE(towns.open(reader(bytes)) == TownIndex::State::Ok);
    CHECK(towns.count() == 2);
    CHECK(towns.accuracy() == 150);

    TownIndex::Place place = towns.find(440500000, -725500000);
    CHECK(place.covered);
    REQUIRE(place.town.valid());
    CHECK(place.town.name == "Alder");
    CHECK(place.town.area == "Maple County, VT");
    CHECK_FALSE(place.neighbor.valid());

    place = towns.find(440500000, -724500000);
    CHECK(place.town.name == "birch");

    // 80 m west of the line between them
    place = towns.find(440500000, -725010000);
    CHECK(place.town.name == "Alder");
    REQUIRE(place.neighbor.valid());
    CHECK(place.neighbor.name == "birch");

    place = towns.find(450000000, -725000000);
    CHECK_FALSE(place.covered);
    CHECK_FALSE(place.town.valid());
}

TEST_CASE("TownIndex lists and searches")
{
    const std::vector<uint8_t> bytes = twoTowns();
    TownIndex towns;
    REQUIRE(towns.open(reader(bytes)) == TownIndex::State::Ok);
    const auto near = towns.around(440500000, -724600000, 5);
    REQUIRE(near.size() == 2);
    CHECK(near[0].name == "birch");
    CHECK(towns.around(440500000, -724600000, 5, near[0].number).size() == 1);
    REQUIRE(towns.search("BI", 5).size() == 1);
    CHECK(towns.search("al", 5)[0].name == "Alder");
    CHECK(towns.search("c", 5).empty());
    CHECK(towns.search("", 5).empty());
}

TEST_CASE("TownIndex refuses what it cannot read")
{
    TownIndex towns;
    const std::vector<uint8_t> other = twoTowns(2);
    CHECK(towns.open(reader(other)) == TownIndex::State::OtherVersion);
    CHECK_FALSE(towns.find(440500000, -725500000).covered);
    const std::vector<uint8_t> junk(100, 7);
    CHECK(towns.open(reader(junk)) == TownIndex::State::NotTownData);
    std::vector<uint8_t> cut = twoTowns();
    cut.resize(cut.size() - 8); // a file that is not all there (an upload still running) is not opened
    CHECK(towns.open(reader(cut)) == TownIndex::State::NotTownData);
    // a ring that claims more points than the file holds: no crash, no answer from it, the other town still found
    std::vector<uint8_t> lying = twoTowns();
    const size_t secondRing = lying.size() - 24;
    lying[secondRing] = 0xFF;
    lying[secondRing + 1] = 0xFF;
    lying[secondRing + 2] = 0x3F;
    REQUIRE(towns.open(reader(lying)) == TownIndex::State::Ok);
    CHECK(towns.find(440500000, -725500000).town.name == "Alder");
    CHECK_FALSE(towns.find(440500000, -724500000).town.valid());
}
