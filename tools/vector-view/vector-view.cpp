// vector-view <map.jmv[,more.jmv...]> <latitude> <longitude> border   (how far the border of a country is)
// vector-view <map.jmv[,more.jmv...]> <latitude> <longitude> <zoom> <light|dark> <out.ppm> [layers to leave out: c w r t b n]
//
// What the device would show: a 320 x 240 view of vector maps (side by side, as the device shows the files
// of a folder) around a place, drawn by device-ui's own
// VectorMap, and how long the tiles took. Build: see build.sh next to this file.
#include "util/VectorMap.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 7 && !(argc == 5 && strcmp(argv[4], "border") == 0)) {
        fprintf(stderr, "usage: vector-view map.jmv lat lon zoom light|dark out.ppm [cwrtbn]\n");
        return 2;
    }
    std::vector<std::unique_ptr<VectorMap>> maps;
    size_t reads = 0, bytes = 0;
    std::string list = argv[1];
    for (size_t from = 0; from <= list.size();) {
        size_t comma = list.find(',', from);
        if (comma == std::string::npos)
            comma = list.size();
        const std::string name = list.substr(from, comma - from);
        from = comma + 1;
        FILE *f = fopen(name.c_str(), "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", name.c_str());
            return 1;
        }
        maps.emplace_back(new VectorMap());
        if (!maps.back()->open([&reads, &bytes, f](uint32_t offset, void *buffer, uint32_t length) {
                reads++;
                bytes += length;
                return fseek(f, offset, SEEK_SET) == 0 && fread(buffer, 1, length, f) == length;
            })) {
            fprintf(stderr, "%s is no vector map\n", name.c_str());
            return 1;
        }
    }
    const double lat = atof(argv[2]), lon = atof(argv[3]);
    if (strcmp(argv[4], "border") == 0) {
        // how far the border of a country is from the place, as the device works it out for its warning
        const double x = (lon + 180.0) / 360.0, y = (1.0 - std::asinh(std::tan(lat * M_PI / 180)) / M_PI) / 2;
        std::vector<double> lines;
        bool covered = false;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                uint32_t most = 0;
                std::vector<double> best, found;
                for (auto &map : maps) {
                    found.clear();
                    const uint32_t weight = map->collect(VectorMap::NationLine, x, y, dx, dy, found);
                    if (weight > most) {
                        most = weight;
                        best.swap(found);
                    }
                }
                covered |= most && !dx && !dy;
                lines.insert(lines.end(), best.begin(), best.end());
            }
        const double scale = 40075016.686 * std::cos(lat * M_PI / 180);
        double nearest = 1e30, nearX = 0, nearY = 0;
        for (size_t i = 0; i + 3 < lines.size(); i += 4) {
            const double ax = lines[i], ay = lines[i + 1], ex = lines[i + 2] - ax, ey = lines[i + 3] - ay, length = ex * ex + ey * ey;
            double t = length > 0 ? ((x - ax) * ex + (y - ay) * ey) / length : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const double px = ax + t * ex, py = ay + t * ey, d = (px - x) * (px - x) + (py - y) * (py - y);
            if (d < nearest) {
                nearest = d;
                nearX = px;
                nearY = py;
            }
        }
        double degrees = std::atan2(nearX - x, -(nearY - y)) * 180 / M_PI;
        if (!covered)
            printf("no map covers %.5f %.5f\n", lat, lon);
        else if (nearest > 1e29)
            printf("%.5f %.5f: no border within the cells around, %zu stretches\n", lat, lon, lines.size() / 4);
        else
            printf("%.5f %.5f: border %.0f m at %.0f degrees, %zu stretches around\n", lat, lon, std::sqrt(nearest) * scale,
                   degrees < 0 ? degrees + 360 : degrees, lines.size() / 4);
        return 0;
    }
    const int z = atoi(argv[4]);
    const bool dark = strcmp(argv[5], "dark") == 0;
    VectorMap::Layers layers;
    if (argc > 7) {
        layers.contours = !strchr(argv[7], 'c');
        layers.water = !strchr(argv[7], 'w');
        layers.roads = !strchr(argv[7], 'r');
        layers.trails = !strchr(argv[7], 't');
        layers.boundaries = !strchr(argv[7], 'b');
        layers.names = !strchr(argv[7], 'n');
    }
    const int W = 320, H = 240;
    const double n = std::ldexp(1.0, z);
    const double px = (lon + 180.0) / 360.0 * n * 256, py = (1.0 - std::asinh(std::tan(lat * M_PI / 180)) / M_PI) / 2 * n * 256;
    const long left = std::lround(px) - W / 2, top = std::lround(py) - H / 2;
    std::vector<uint16_t> view((size_t)W * H, 0x8410), tile(256 * 256);
    int tiles = 0, empty = 0;
    const auto start = std::chrono::steady_clock::now();
    for (long ty = top >> 8; ty <= (top + H - 1) >> 8; ty++)
        for (long tx = left >> 8; tx <= (left + W - 1) >> 8; tx++) {
            tiles++;
            // as the device does: each map its lines, the first the ground under them; then each its names
            // and where maps hold the same cell, the one with the most for it draws it
            // (among the pieces of one map that was cut up, in the coarse levels too: group)
            auto ownerOf = [&](int dx, int dy, uint16_t group) {
                int owner = -1;
                uint32_t most = 0;
                for (size_t i = 0; i < maps.size(); i++) {
                    if (group && maps[i]->group() != group)
                        continue;
                    const uint32_t weight = maps[i]->weight((uint8_t)z, (uint32_t)tx, (uint32_t)ty, dx, dy);
                    if (weight > most) {
                        most = weight;
                        owner = (int)i;
                    }
                }
                return owner;
            };
            bool drawn = false;
            for (uint8_t part : {(uint8_t)VectorMap::Lines, (uint8_t)VectorMap::Names})
                for (size_t i = 0; i < maps.size(); i++) {
                    const uint8_t parts = part | (part == VectorMap::Lines && !drawn ? VectorMap::Ground : 0);
                    const uint16_t among = maps[i]->whole((uint8_t)z) ? 0 : maps[i]->group();
                    const VectorMap::Mine mine = [&, i, among](int dx, int dy) { return ownerOf(dx, dy, among) == (int)i; };
                    const bool shared = maps.size() > 1 && (maps[i]->whole((uint8_t)z) || among);
                    if (maps[i]->render((uint8_t)z, (uint32_t)tx, (uint32_t)ty, tile.data(), dark, layers, parts, shared ? &mine : nullptr))
                        drawn = true;
                }
            if (!drawn) {
                empty++;
                continue;
            }
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++) {
                    const long vx = tx * 256 + x - left, vy = ty * 256 + y - top;
                    if (vx >= 0 && vy >= 0 && vx < W && vy < H)
                        view[(size_t)vy * W + vx] = tile[y * 256 + x];
                }
        }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    FILE *out = fopen(argv[6], "wb");
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (uint16_t p : view) {
        const uint8_t c[3] = {(uint8_t)((p >> 11) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
        fwrite(c, 1, 3, out);
    }
    fclose(out);
    printf("zoom %d: %d tiles (%d without data) in %.1f ms here, %zu reads, %zu bytes read\n", z, tiles, empty, ms, reads, bytes);
    return 0;
}
