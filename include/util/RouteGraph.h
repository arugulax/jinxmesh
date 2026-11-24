#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

/**
 * Routes over roads and trails from the routing file on the card, /geo/roads.bin. The format and its builder
 * (kbview-harness/maps/roads.py) are described in kbview-harness/docs/MAP-PACKS.md, section 6.
 *
 * The file is tens of megabytes and never loaded whole: it is read in blocks of 4 kB through a function, and a
 * few hundred of them are kept. A search runs in slices, so that the screen stays alive: begin(), then work()
 * until it is no longer Running, then result(). Its memory is taken at begin() and given back when it ends.
 *
 * No display code in here; the unit tests build a small file in memory.
 */
class RouteGraph
{
  public:
    // reads length bytes at offset into buffer; false when that is not possible
    using Read = std::function<bool(uint32_t offset, void *buffer, uint32_t length)>;

    // where the big buffers come from: external RAM on the device
    static void setAllocator(void *(*allocate)(size_t), void (*release)(void *));
    template <class T> struct Big {
        using value_type = T;
        Big() = default;
        template <class U> Big(const Big<U> &) {}
        T *allocate(size_t n) { return static_cast<T *>(RouteGraph::bigAllocate(n * sizeof(T))); }
        void deallocate(T *p, size_t) { RouteGraph::bigRelease(p); }
        template <class U> bool operator==(const Big<U> &) const { return true; }
        template <class U> bool operator!=(const Big<U> &) const { return false; }
    };

    enum class State : uint8_t { Closed, Ok, NotRoadData, OtherVersion };
    enum class Mode : uint8_t { Car, Foot };
    enum class Progress : uint8_t { Idle, Running, Done, NoWayNearStart, NoWayNearEnd, NoRoute, TooFar, NoMemory, ReadError };
    enum Turn : uint8_t { Start, Continue, SlightLeft, Left, SharpLeft, SlightRight, Right, SharpRight, UTurn, Arrive };
    enum Class : uint8_t {
        Motorway, Trunk, Primary, Secondary, Tertiary, Unclassified, Residential, Service, Track, Path, Cycleway, Steps, Ferry
    };

    struct Point {
        int32_t latitude = 0; // degrees times 1e7
        int32_t longitude = 0;
    };

    struct Step {
        uint8_t turn = Start;
        uint8_t kind = Unclassified; // class of the way that follows
        std::string name;            // of the way that follows; empty when it has none
        uint32_t point = 0;          // index into Route::points of where this happens
        uint32_t meters = 0;         // from here to the next step
        float bearing = 0;           // of the way that follows, at its start
    };

    struct Route {
        std::vector<Point, Big<Point>> points; // from the way nearest the start to the way nearest the end
        std::vector<uint32_t, Big<uint32_t>> along; // meters from the start, per point
        std::vector<Step> steps;               // the first is Start, the last Arrive
        uint32_t meters = 0;
        uint32_t seconds = 0;     // at the speeds the search assumes
        uint32_t startOff = 0;    // meters from the position asked for to the first point
        uint32_t endOff = 0;      // and from the last point to the destination asked for
    };

    ~RouteGraph() { close(); }
    State open(Read read);
    void close(void);
    State state(void) const { return status; }
    uint32_t vertices(void) const { return header.vertices; }
    bool covers(Point at) const;

    /**
     * Starts a search. Running, or why there will be none.
     */
    Progress begin(Point from, Point to, Mode mode);
    /**
     * Searches on for about that many milliseconds (at least one step). The clock is a function so that tests
     * need none.
     */
    Progress work(uint32_t milliseconds, uint32_t (*clock)(void));
    Progress progress(void) const { return search.progress; }
    void cancel(void);
    uint32_t settled(void) const { return search.settled; }
    // how far the search has come towards the destination, 0 to 100
    uint8_t percent(void) const;
    /**
     * The route of a search that is Done. The search's memory is given back.
     */
    bool result(Route &out);

    static const char *turnText(uint8_t turn);
    static float speed(uint8_t kind, Mode mode); // m/s
    static void *bigAllocate(size_t bytes);
    static void bigRelease(void *memory);

  private:
    static constexpr uint32_t c_block = 4096;
    static constexpr uint32_t c_ways = 4; // blocks per set of the cache
    static constexpr uint32_t c_seed = 0x7FFFFFFF;

    struct Header {
        int32_t minLat = 0, minLon = 0, maxLat = 0, maxLon = 0;
        uint32_t cell = 0, columns = 0, rows = 0, vertices = 0, edges = 0;
        uint32_t offCells = 0, offVertices = 0, offEdges = 0, offShapes = 0, offNames = 0, size = 0;
    };
    struct Vertex {
        int32_t latitude, longitude;
        uint32_t firstEdge;
    };
    struct Edge {
        uint32_t target, shape, meta, name;
        uint32_t length(void) const { return meta & 0xFFFFF; }
        uint8_t kind(void) const { return (meta >> 20) & 15; }
        bool car(void) const { return (meta >> 24) & 1; }
        bool foot(void) const { return (meta >> 25) & 1; }
        bool reversed(void) const { return (meta >> 26) & 1; }
    };
    // a position on a way
    struct Snap {
        bool found = false;
        uint32_t from = 0;  // source vertex of the edge record it lies on
        Edge edge{};        // that record
        Point at;           // the point on the way
        uint32_t along = 0; // meters from the source vertex to it
        uint32_t segment = 0; // the point lies behind this point of the shape (0: the source vertex)
        double off = 0;     // meters from the position asked for
    };
    struct Visit {
        uint32_t vertex; // plus one; 0: free
        uint32_t cost;
        uint32_t parent; // bit 31: settled
    };
    struct Open {
        uint32_t f;
        uint32_t vertex;
    };
    struct Slot {
        uint32_t block = UINT32_MAX;
        uint32_t used = 0;
    };
    struct Search {
        Progress progress = Progress::Idle;
        Mode mode = Mode::Car;
        Point from, to;
        Snap start, end;
        bool sameWay = false; // start and end lie on one edge and the way between them is open
        Visit *visits = nullptr;
        uint32_t capacity = 0, count = 0;
        Open *heap = nullptr;
        uint32_t heapCapacity = 0, heapCount = 0;
        uint32_t settled = 0;
        uint32_t best = UINT32_MAX; // cost of the best way to the destination so far
        uint32_t bestVertex = 0;
        uint32_t goal[2] = {UINT32_MAX, UINT32_MAX}; // vertices from which the end is reached, and what that costs
        uint32_t goalCost[2] = {0, 0};
        double startDistance = 0, closest = 0; // straight line to the destination: at the start, and the least so far
    };

    bool read(uint32_t offset, void *buffer, uint32_t length);
    bool vertex(uint32_t number, Vertex &out);
    bool edge(uint32_t number, Edge &out);
    bool usable(const Edge &e, Mode mode) const;
    uint32_t cost(const Edge &e, Mode mode) const;
    bool shape(uint32_t from, const Edge &e, std::vector<Point> &out);
    bool reverseOf(uint32_t from, const Edge &e, Edge &out);
    bool snap(Point at, Mode mode, Snap &out);
    std::string name(uint32_t offset);
    Visit *visit(uint32_t vertex, bool add);
    bool push(uint32_t f, uint32_t vertex);
    bool relax(uint32_t vertex, uint32_t cost, uint32_t parent);
    uint32_t bound(const Vertex &v) const;
    void release(void);
    bool allocate(void);

    State status = State::Closed;
    Read reader;
    Header header;
    Search search;
    uint8_t *cache = nullptr;
    Slot *slots = nullptr;
    uint32_t sets = 0, tick = 0;
    bool readFailed = false;
};
