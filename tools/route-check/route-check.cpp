// route-check: the device's router on a desktop, to compare with the reference router of maps/roads.py.
//   route-check FILE.bin lat,lon lat,lon [car|foot] [-v]
// Build: g++ -O2 -std=c++17 -I$UI/include route-check.cpp $UI/source/util/RouteGraph.cpp -o route-check
#include "util/RouteGraph.h"
#include <chrono>
#include <cstdio>
#include <cstring>

static uint32_t reads = 0;
static uint32_t ms(void)
{
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "route-check FILE lat,lon lat,lon [car|foot] [-v]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    RouteGraph graph;
    if (graph.open([f](uint32_t offset, void *buffer, uint32_t length) {
            reads++;
            return fseek(f, offset, SEEK_SET) == 0 && fread(buffer, 1, length, f) == length;
        }) != RouteGraph::State::Ok) {
        printf("not road data\n");
        return 1;
    }
    double a, b, c, d;
    sscanf(argv[2], "%lf,%lf", &a, &b);
    sscanf(argv[3], "%lf,%lf", &c, &d);
    const bool foot = argc > 4 && !strcmp(argv[4], "foot");
    const bool verbose = argc > 5;
    const uint32_t t0 = ms();
    reads = 0;
    RouteGraph::Progress p = graph.begin({(int32_t)(a * 1e7), (int32_t)(b * 1e7)}, {(int32_t)(c * 1e7), (int32_t)(d * 1e7)},
                                         foot ? RouteGraph::Mode::Foot : RouteGraph::Mode::Car);
    uint32_t slices = 0;
    while (p == RouteGraph::Progress::Running) {
        p = graph.work(20, ms);
        slices++;
    }
    const uint32_t settled = graph.settled();
    RouteGraph::Route route;
    if (p != RouteGraph::Progress::Done || !graph.result(route)) {
        printf("no route: %d\n", (int)p);
        return 1;
    }
    printf("length %u m, %u s, %zu points, %zu steps, %u settled, %u block reads, %u ms, off %u/%u m\n", route.meters,
           route.seconds, route.points.size(), route.steps.size(), settled, reads, ms() - t0, route.startOff, route.endOff);
    if (verbose)
        for (const auto &s : route.steps)
            printf("  %-12s %-30s %6u m  (%.0f deg)\n", RouteGraph::turnText(s.turn), s.name.c_str(), s.meters, s.bearing);
    return 0;
}
