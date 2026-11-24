// contours <grid.f32 | grid.rgb> <width> <height> <interval m> <tolerance px> <out.bin>
//
// Contour lines of an elevation grid, by marching squares. The grid is meters as float32, rows from the north,
// or (a name ending in .rgb) the pixels of "terrarium" elevation tiles, three bytes each: red * 256 + green +
// blue / 256 - 32768 meters; black pixels are places without data, and no line runs through those. The grid is
// smoothed a little first, the segments are joined to lines, and the lines are thinned (Douglas-Peucker) to
// the tolerance. Output, little endian: per line float32 elevation (m), uint32 count, then count pairs of
// float32 x y in grid pixels. No dependencies: g++ -O2 -std=c++17 -o contours contours.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

struct P {
    float x, y;
};

static void thin(const std::vector<P> &in, size_t a, size_t b, float tolerance, std::vector<char> &keep)
{
    // iterative: contour lines get long
    std::vector<std::pair<size_t, size_t>> todo{{a, b}};
    while (!todo.empty()) {
        auto [from, to] = todo.back();
        todo.pop_back();
        if (to <= from + 1)
            continue;
        const float dx = in[to].x - in[from].x, dy = in[to].y - in[from].y;
        const float length = std::sqrt(dx * dx + dy * dy);
        float worst = 0;
        size_t at = from;
        for (size_t i = from + 1; i < to; i++) {
            const float ex = in[i].x - in[from].x, ey = in[i].y - in[from].y;
            const float d = length > 0 ? std::fabs(ex * dy - ey * dx) / length : std::sqrt(ex * ex + ey * ey);
            if (d > worst) {
                worst = d;
                at = i;
            }
        }
        if (worst > tolerance) {
            keep[at] = 1;
            todo.push_back({from, at});
            todo.push_back({at, to});
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: contours grid.f32 width height interval tolerance out.bin\n");
        return 2;
    }
    const int W = atoi(argv[2]), H = atoi(argv[3]);
    const double interval = atof(argv[4]);
    const float tolerance = (float)atof(argv[5]);
    std::vector<float> raw((size_t)W * H), g((size_t)W * H);
    FILE *f = fopen(argv[1], "rb");
    const size_t nameLength = strlen(argv[1]);
    if (nameLength > 4 && strcmp(argv[1] + nameLength - 4, ".rgb") == 0) {
        std::vector<uint8_t> row((size_t)W * 3);
        for (int y = 0; y < H; y++) {
            if (!f || fread(row.data(), 1, row.size(), f) != row.size()) {
                fprintf(stderr, "cannot read the grid\n");
                return 1;
            }
            for (int x = 0; x < W; x++) {
                const uint8_t *p = &row[(size_t)x * 3];
                raw[(size_t)y * W + x] = (p[0] | p[1] | p[2]) ? p[0] * 256.0f + p[1] + p[2] / 256.0f - 32768.0f : NAN;
            }
        }
    } else if (!f || fread(raw.data(), 4, raw.size(), f) != raw.size()) {
        fprintf(stderr, "cannot read the grid\n");
        return 1;
    }
    fclose(f);
    // two passes of a 3x3 binomial filter: the model has steps of its own that would show as wiggles
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double sum = 0, weight = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                            continue;
                        const float v = raw[(size_t)yy * W + xx];
                        if (std::isnan(v))
                            continue;
                        const double w = (dx ? 1 : 2) * (dy ? 1 : 2);
                        sum += w * v;
                        weight += w;
                    }
                g[(size_t)y * W + x] = std::isnan(raw[(size_t)y * W + x]) || weight == 0 ? NAN : (float)(sum / weight);
            }
        raw = g;
    }
    float low = 1e9f, high = -1e9f;
    for (float v : g) {
        if (std::isnan(v))
            continue;
        low = std::fmin(low, v);
        high = std::fmax(high, v);
    }
    if (low > high)
        low = high = 0;
    // nothing below the sea: its floor is in the tiles too
    low = std::fmax(low, 0.0f);
    FILE *out = fopen(argv[6], "wb");
    size_t lines = 0, points = 0, kept = 0;
    // a point of a contour lies on a grid edge: edge id = cell index * 2 + (0 horizontal edge, 1 vertical edge)
    for (long step = (long)std::ceil(low / interval); step * interval <= high; step++) {
        const float level = (float)(step * interval) + 1e-4f; // never exactly on a grid value
        std::unordered_map<uint64_t, std::vector<uint64_t>> links; // edge -> the edges it is joined to
        auto edgeH = [&](int x, int y) { return ((uint64_t)y * W + x) * 2; };     // between (x,y) and (x+1,y)
        auto edgeV = [&](int x, int y) { return ((uint64_t)y * W + x) * 2 + 1; }; // between (x,y) and (x,y+1)
        for (int y = 0; y + 1 < H; y++)
            for (int x = 0; x + 1 < W; x++) {
                const float a = g[(size_t)y * W + x], b = g[(size_t)y * W + x + 1], c = g[(size_t)(y + 1) * W + x + 1],
                            d = g[(size_t)(y + 1) * W + x];
                if (std::isnan(a + b + c + d))
                    continue;
                const int code = (a > level) | (b > level) << 1 | (c > level) << 2 | (d > level) << 3;
                if (code == 0 || code == 15)
                    continue;
                const uint64_t top = edgeH(x, y), right = edgeV(x + 1, y), bottom = edgeH(x, y + 1), left = edgeV(x, y);
                auto join = [&](uint64_t p, uint64_t q) {
                    links[p].push_back(q);
                    links[q].push_back(p);
                };
                switch (code) {
                case 1: case 14: join(left, top); break;
                case 2: case 13: join(top, right); break;
                case 3: case 12: join(left, right); break;
                case 4: case 11: join(right, bottom); break;
                case 6: case 9: join(top, bottom); break;
                case 7: case 8: join(left, bottom); break;
                case 5: // saddle: by the middle
                case 10: {
                    const bool mid = (a + b + c + d) / 4 > level;
                    if ((code == 5) == mid) {
                        join(left, bottom);
                        join(top, right);
                    } else {
                        join(left, top);
                        join(right, bottom);
                    }
                    break;
                }
                }
            }
        auto where = [&](uint64_t e) {
            const uint64_t cell = e / 2;
            const int x = (int)(cell % W), y = (int)(cell / W);
            const float v0 = g[(size_t)y * W + x];
            if (e & 1) {
                const float v1 = g[(size_t)(y + 1) * W + x];
                return P{(float)x, y + (level - v0) / (v1 - v0)};
            }
            const float v1 = g[(size_t)y * W + x + 1];
            return P{x + (level - v0) / (v1 - v0), (float)y};
        };
        std::unordered_map<uint64_t, char> used;
        auto walk = [&](uint64_t start, std::vector<P> &line) {
            uint64_t at = start, before = UINT64_MAX;
            for (;;) {
                used[at] = 1;
                line.push_back(where(at));
                uint64_t next = UINT64_MAX;
                for (uint64_t n : links[at])
                    if (n != before && !used.count(n)) {
                        next = n;
                        break;
                    }
                if (next == UINT64_MAX) {
                    // a closed line: back to where it began
                    for (uint64_t n : links[at])
                        if (n == start && n != before && line.size() > 2) {
                            line.push_back(where(start));
                            break;
                        }
                    return;
                }
                before = at;
                at = next;
            }
        };
        auto emit = [&](std::vector<P> &line) {
            if (line.size() < 2)
                return;
            std::vector<char> keep(line.size(), 0);
            keep.front() = keep.back() = 1;
            thin(line, 0, line.size() - 1, tolerance, keep);
            std::vector<P> thinned;
            for (size_t i = 0; i < line.size(); i++)
                if (keep[i])
                    thinned.push_back(line[i]);
            // a speck: a closed line a few pixels around
            if (thinned.size() < 4 && line.size() < 12)
                return;
            const float elevation = (float)(step * interval);
            const uint32_t count = (uint32_t)thinned.size();
            fwrite(&elevation, 4, 1, out);
            fwrite(&count, 4, 1, out);
            fwrite(thinned.data(), 8, count, out);
            lines++;
            points += line.size();
            kept += count;
        };
        // open lines first, from their ends (an edge with one link), then the closed ones
        for (auto &[e, to] : links)
            if (to.size() == 1 && !used.count(e)) {
                std::vector<P> line;
                walk(e, line);
                emit(line);
            }
        for (auto &[e, to] : links)
            if (!used.count(e)) {
                std::vector<P> line;
                walk(e, line);
                emit(line);
            }
    }
    fclose(out);
    fprintf(stderr, "elevation %.0f to %.0f m, %zu lines, %zu points, %zu kept\n", low, high, lines, points, kept);
    return 0;
}
