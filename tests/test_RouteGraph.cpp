// RouteGraph against a file of four vertices built here. The search was also compared with the reference router
// of kbview-harness/maps/roads.py on the file of Vermont, New Hampshire and northern New York (2026-11-20): the
// same routes to within a few meters on six pairs up to 365 km.
#include "util/RouteGraph.h"
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
};

constexpr uint32_t c_car = 1u << 24, c_foot = 1u << 25, c_reversed = 1u << 26;
constexpr uint32_t c_residential = 6u << 20, c_path = 9u << 20;

/**
 * A at 44.000 N 73.000 W, B 400 m east of it, C another 400 m east, D 445 m north of B.
 * A-B "Elm" is a street, B-C "Elm" a one-way street closed to feet, B-D "Trail" a path.
 */
std::vector<uint8_t> village(void)
{
    File f;
    const uint32_t cells = 2, vertices = 4, edges = 5, shapes = 6, names = 11;
    const uint32_t offCells = 68, offVertices = offCells + 8 * cells, offEdges = offVertices + 12 * (vertices + 1),
                   offShapes = offEdges + 16 * edges, offNames = offShapes + shapes;
    f.bytes = {'J', 'M', 'R', 'D'};
    f.u16(1);
    f.u16(0);
    f.i32(440000000);
    f.i32(-730000000);
    f.i32(440040000);
    f.i32(-729900000);
    f.u32(100000);
    f.u32(2);
    f.u32(1);
    f.u32(vertices);
    f.u32(edges);
    f.u32(offCells);
    f.u32(offVertices);
    f.u32(offEdges);
    f.u32(offShapes);
    f.u32(offNames);
    f.u32(offNames + names);
    // cells: A, B and D in the first, C in the second
    f.u32(0);
    f.u32(3);
    f.u32(3);
    f.u32(1);
    // vertices A, B, D, C and the closing record
    const int32_t at[4][2] = {{440000000, -730000000}, {440000000, -729950000}, {440040000, -729950000}, {440000000, -729900000}};
    const uint32_t first[5] = {0, 1, 4, 5, 5};
    for (int i = 0; i < 4; i++) {
        f.i32(at[i][0]);
        f.i32(at[i][1]);
        f.u32(first[i]);
    }
    f.i32(0);
    f.i32(0);
    f.u32(first[4]);
    // edges: target, shape, meta, name
    const uint32_t list[5][4] = {{1, 0, 400 | c_residential | c_car | c_foot, 1},
                                 {0, 0, 400 | c_residential | c_car | c_foot | c_reversed, 1},
                                 {3, 2, 400 | c_residential | c_car, 1},
                                 {2, 4, 445 | c_path | c_foot, 5},
                                 {1, 4, 445 | c_path | c_foot | c_reversed, 5}};
    for (const auto &e : list)
        for (uint32_t v : e)
            f.u32(v);
    for (int i = 0; i < 3; i++)
        f.u16(0); // no points between the vertices
    const char text[] = "\0Elm\0Trail";
    f.bytes.insert(f.bytes.end(), text, text + sizeof(text));
    return f.bytes;
}

RouteGraph::Read reader(const std::vector<uint8_t> &bytes)
{
    return [&bytes](uint32_t offset, void *buffer, uint32_t length) {
        if ((uint64_t)offset + length > bytes.size())
            return false;
        memcpy(buffer, bytes.data() + offset, length);
        return true;
    };
}

RouteGraph::Progress run(RouteGraph &graph, RouteGraph::Point from, RouteGraph::Point to, RouteGraph::Mode mode)
{
    RouteGraph::Progress progress = graph.begin(from, to, mode);
    while (progress == RouteGraph::Progress::Running)
        progress = graph.work(0, nullptr);
    return progress;
}

const RouteGraph::Point A{440000000, -730000000}, B{440000000, -729950000}, C{440000000, -729900000}, D{440040000, -729950000};

} // namespace

TEST_CASE("RouteGraph finds the way along a street")
{
    const std::vector<uint8_t> bytes = village();
    RouteGraph graph;
    REQUIRE(graph.open(reader(bytes)) == RouteGraph::State::Ok);
    REQUIRE(run(graph, A, C, RouteGraph::Mode::Car) == RouteGraph::Progress::Done);
    RouteGraph::Route route;
    REQUIRE(graph.result(route));
    CHECK(route.meters == 800);
    CHECK(route.points.size() == 3);
    CHECK(route.along.back() == 800);
    REQUIRE(route.steps.size() == 2);
    CHECK(route.steps[0].turn == RouteGraph::Start);
    CHECK(route.steps[0].name == "Elm");
    CHECK(route.steps[0].meters == 800);
    CHECK(route.steps[1].turn == RouteGraph::Arrive);
}

TEST_CASE("RouteGraph keeps to one-way streets and to what feet may use")
{
    const std::vector<uint8_t> bytes = village();
    RouteGraph graph;
    REQUIRE(graph.open(reader(bytes)) == RouteGraph::State::Ok);
    // against the one-way street there is no way back
    CHECK(run(graph, C, A, RouteGraph::Mode::Car) == RouteGraph::Progress::NoRoute);
    // on foot the path is open, and the turn onto it is a left one
    REQUIRE(run(graph, A, D, RouteGraph::Mode::Foot) == RouteGraph::Progress::Done);
    RouteGraph::Route route;
    REQUIRE(graph.result(route));
    CHECK(route.meters == 845);
    REQUIRE(route.steps.size() == 3);
    CHECK(route.steps[1].turn == RouteGraph::Left);
    CHECK(route.steps[1].name == "Trail");
    CHECK(route.steps[1].point == 1);
    CHECK(route.steps[1].meters == 445);
    // a car is not sent up the path: its way ends on the street below it
    REQUIRE(run(graph, A, D, RouteGraph::Mode::Car) == RouteGraph::Progress::Done);
    REQUIRE(graph.result(route));
    CHECK(route.meters == 400);
    CHECK(route.endOff > 400);
}

TEST_CASE("RouteGraph routes between two points of one edge")
{
    const std::vector<uint8_t> bytes = village();
    RouteGraph graph;
    REQUIRE(graph.open(reader(bytes)) == RouteGraph::State::Ok);
    const RouteGraph::Point near{440000000, -729990000}, far{440000000, -729960000};
    REQUIRE(run(graph, far, near, RouteGraph::Mode::Car) == RouteGraph::Progress::Done);
    RouteGraph::Route route;
    REQUIRE(graph.result(route));
    CHECK(route.meters > 200);
    CHECK(route.meters < 280);
    CHECK(route.points.size() == 2);
}

TEST_CASE("RouteGraph refuses what is not road data and positions far from any way")
{
    std::vector<uint8_t> bytes = village();
    RouteGraph graph;
    REQUIRE(graph.open(reader(bytes)) == RouteGraph::State::Ok);
    CHECK(graph.begin(RouteGraph::Point{450000000, -730000000}, A, RouteGraph::Mode::Car) == RouteGraph::Progress::NoWayNearStart);
    CHECK(graph.begin(A, RouteGraph::Point{440000000, -720000000}, RouteGraph::Mode::Car) == RouteGraph::Progress::NoWayNearEnd);
    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 5);
    CHECK(graph.open(reader(cut)) == RouteGraph::State::NotRoadData);
    bytes[4] = 9;
    CHECK(graph.open(reader(bytes)) == RouteGraph::State::OtherVersion);
    bytes[0] = 'X';
    CHECK(graph.open(reader(bytes)) == RouteGraph::State::NotRoadData);
}
