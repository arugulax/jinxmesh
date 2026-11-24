#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/**
 * A map drawn from lines: contour lines, water, roads and trails, rails, town lines and names, kept on the
 * card as point lists and drawn at whatever zoom is asked for. The file is made by
 * kbview-harness/maps/vector/build.py.
 *
 * The file ("JMVM", version 4, little endian):
 *
 *   "JMVM" | u16 version | u16 levels
 *   i32 south, west, north, east (degrees times 1e7)
 *   u8 lowest tile zoom | u8 highest tile zoom | u16 group: 0, or what the pieces of one map share that was
 *      cut up (a state in files small enough to host and to send)
 *   per level: u8 cell zoom | u8 first tile zoom | u8 last tile zoom | u8 0 | u32 x of the first cell |
 *              u32 y | u16 columns | u16 rows | u32 offset of its directory
 *   directories: per cell u32 offset, u32 bytes of lines and areas, u32 bytes of names
 *   cells: the lines and areas of each, its names right behind them
 *
 * A level is the same land at one degree of detail; its cells are Web Mercator tiles of its cell zoom, and it
 * serves map tiles of its range of zooms (a tile always lies within one cell). Within a cell positions run
 * from 0 to 4096, and lines reach a little over the edge so that they meet those of the next cell.
 *
 * The names of a cell are kept apart because a tile also shows names of the cells next to it, and those
 * must not cost the reading of all their lines.
 *
 * Lines, areas and names are rows of features, the lines and areas in the order they are drawn in (the last
 * on top; the builder sees to that). Each starts with its kind (one byte):
 *   lines and areas (below 64): contours carry their height in feet (varint) first; then the number of points
 *   (varint) and per point the step from the one before as two zigzag varints (the first from 0, 0)
 *   names (64 and up): x, y (zigzag varints), the first and the last tile zoom it is shown at (two bytes),
 *   how it is turned (a byte: 256ths of a turn, clockwise, signed), a number (varint; the height in feet of
 *   a peak or a contour line), length, text. The builder settles which names have room at which zoom and
 *   how they lie along their road or stream there, so that every tile shows the same ones and none run
 *   into each other; a name on a line can be there several times, for different zooms.
 *
 * No display code in here: render() fills 256 x 256 pixels of RGB565, which the tile service hands to the
 * map as if it had been a picture on the card. The unit tests and a desktop viewer use the same call.
 */
class VectorMap
{
  public:
    using Read = std::function<bool(uint32_t offset, void *buffer, uint32_t length)>;

    static constexpr int c_tile = 256;

    enum Kind : uint8_t {
        Contour20 = 1,
        Contour100 = 2,
        Contour500 = 3,
        Stream = 10,
        River = 11,
        Water = 12,
        Coast = 13, // the line of the sea
        Motorway = 20,
        Primary = 21,
        Secondary = 22,
        Minor = 23,
        Service = 24,
        Track = 25,
        Path = 26,
        Rail = 27,
        Boundary = 30,  // of a town or county
        StateLine = 31, // of a state or province
        NationLine = 32, // of a country
        City = 64,
        Town = 65,
        Village = 66,
        Peak = 67,
        RoadName = 68,  // of a road or trail, at a place on it
        WaterName = 69, // of a stream, river, pond
        ContourName = 70 // the height of a contour line (its number), at a place on it; no text
    };

    // what to draw; all on by default
    struct Layers {
        bool contours = true;
        bool water = true;
        bool roads = true;
        bool trails = true;
        bool boundaries = true;
        bool names = true;
    };

    /**
     * Changes one color of the light or the dark look: "ground", "contour" (the 20 ft lines), "contourIndex"
     * (100 ft), "contourMain" (500 ft), "water", "waterLine", "motorway", "primary", "secondary", "minor",
     * "service", "track", "path", "rail", "boundary", "text", "halo". False: no such name.
     */
    static bool setColor(bool dark, const char *name, uint32_t color);

    ~VectorMap() { close(); }
    bool open(Read read);
    void close(void);
    bool isOpen(void) const { return opened; }
    uint8_t minZoom(void) const { return lowZoom; }
    // tiles can be asked for two zooms past what the file names: lines only get longer, never coarse
    uint8_t maxZoom(void) const { return highZoom; }
    // the middle of what the file covers, degrees
    void center(double &latitude, double &longitude) const;

    // the parts of a tile; several maps that meet in one tile each draw their lines, then each its names
    enum Part : uint8_t { Ground = 1, Lines = 2, Names = 4 };
    // whether the cell of the tile (0, 0), or the one that many cells beside it, is this map's to draw
    using Mine = std::function<bool(int dx, int dy)>;

    /**
     * Draws the map tile z/x/y into 256 x 256 pixels (RGB565, rows from the top). False when the file has
     * nothing for that tile: outside of its area or its zooms; the pixels are then left alone.
     */
    bool render(uint8_t z, uint32_t x, uint32_t y, uint16_t *pixels, bool dark, const Layers &layers, uint8_t parts = Ground | Lines | Names,
                const Mine *mine = nullptr);

    /**
     * Maps of neighboring states hold the same cells along their border, each of them whole (the builder
     * fills them from both sides), so such a cell is drawn from one map only. For that:
     * whole(): whether tiles of that zoom come from cells of that kind (the fine levels; the coarse ones hold
     * a state alone and are laid over each other);
     * weight(): how much the map has for the cell of a tile, or the cell that many cells beside it; 0: nothing.
     * The map with the most is the one to draw it.
     */
    bool whole(uint8_t z) const;
    // Not 0: a piece of a map that was cut up. Pieces of one map (the same number) hold the same cells along
    // their cuts in every level, the coarse ones too: among them a cell is always drawn from one only.
    uint16_t group(void) const { return opened ? pieceOf : 0; }

    /**
     * The lines of one kind in a cell of the finest level: the cell that holds a place (x, y: of the Web
     * Mercator square, 0 to 1) or the one that many cells beside it. They are added to out as stretches, four
     * numbers each (x, y of both ends, of that square). How much the map has for the cell; 0: it is not in it.
     */
    uint32_t collect(uint8_t kind, double x, double y, int dx, int dy, std::vector<double> &out);
    uint32_t weight(uint8_t z, uint32_t x, uint32_t y, int dx = 0, int dy = 0);
    bool render(uint8_t z, uint32_t x, uint32_t y, uint16_t *pixels, bool dark) { return render(z, x, y, pixels, dark, Layers()); }
    // what the file covers, degrees times 1e7
    void bounds(int32_t &s, int32_t &w, int32_t &n, int32_t &e) const
    {
        s = south;
        w = west;
        n = north;
        e = east;
    }

  private:
    struct Level {
        uint8_t cellZoom, firstZoom, lastZoom;
        uint32_t x0, y0;
        uint16_t columns, rows;
        uint32_t directory;
    };
    struct Cell {
        uint8_t level = 0xFF;
        uint32_t x = 0, y = 0, used = 0;
        std::vector<uint8_t> data;
    };

    struct Entry {
        uint8_t level = 0xFF;
        uint32_t x = 0, y = 0;
        uint8_t raw[12];
    };

    const std::vector<uint8_t> *cell(uint8_t level, uint32_t x, uint32_t y, bool names);
    bool entryOf(uint8_t level, uint32_t x, uint32_t y, uint8_t *entry);
    int levelOf(uint8_t z) const;

    bool opened = false;
    Read reader;
    std::vector<Level> levels;
    int32_t south = 0, west = 0, north = 0, east = 0;
    uint8_t lowZoom = 0, highZoom = 0;
    uint16_t pieceOf = 0;
    static constexpr size_t c_cells = 4;  // lines and areas kept: the cells of the tiles on screen
    static constexpr size_t c_names = 12; // names kept: those and the cells around them
    Cell cells[c_cells], names[c_names];
    uint32_t tick = 0;
    static constexpr size_t c_entries = 24;
    static constexpr uint8_t c_wholeCells = 11; // cells of this zoom and finer are whole
    Entry entries[c_entries];
    uint32_t nextEntry = 0;
};
