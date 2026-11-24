#include "util/RouteGraph.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{

constexpr uint32_t c_version = 1;
constexpr uint32_t c_headerSize = 68;
constexpr uint32_t c_maxRings = 6;          // cells around a position that are searched for a way
constexpr double c_unit = 0.0111319;        // meters in 1e-7 degrees of latitude
constexpr uint32_t c_weight = 10;           // of the straight-line bound, in eighths: 10 is 1.25
constexpr uint32_t c_maxShape = 4096;       // points of one shape; the builder's are far shorter
constexpr uint32_t c_maxName = 80;
constexpr double c_bearingBase = 12.0;      // meters of a way its direction is taken over
constexpr float c_footSpeed = 1.25f;        // m/s

// km/h of a car per class; as in the builder (CAR_SPEED), whose reference router the search is compared with
const uint32_t c_carSpeed[16] = {105, 90, 80, 70, 60, 50, 40, 20, 15, 0, 0, 0, 15, 0, 0, 0};
constexpr uint32_t c_topSpeed = 105;
// what a meter on foot counts, in hundredths (FOOT_FACTOR)
const uint32_t c_footFactor[16] = {0, 130, 120, 110, 105, 100, 100, 100, 100, 100, 100, 110, 300, 0, 0, 0};

void *(*allocator)(size_t) = nullptr;
void (*releaser)(void *) = nullptr;

uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

double metersBetween(RouteGraph::Point a, RouteGraph::Point b)
{
    const double k = cos(((double)a.latitude + (double)b.latitude) * 0.5e-7 * M_PI / 180.0);
    const double dx = ((double)b.longitude - (double)a.longitude) * k, dy = (double)b.latitude - (double)a.latitude;
    return sqrt(dx * dx + dy * dy) * c_unit;
}

float bearingOf(RouteGraph::Point a, RouteGraph::Point b)
{
    const double k = cos(((double)a.latitude + (double)b.latitude) * 0.5e-7 * M_PI / 180.0);
    const double dx = ((double)b.longitude - (double)a.longitude) * k, dy = (double)b.latitude - (double)a.latitude;
    double degrees = atan2(dx, dy) * 180.0 / M_PI;
    if (degrees < 0)
        degrees += 360.0;
    return (float)degrees;
}

// one stretch of a route on one edge
struct Leg {
    std::vector<RouteGraph::Point> points;
    uint32_t name = 0;
    uint8_t kind = 0;
    uint32_t meters = 0;
    uint32_t degree = 0; // ways that meet where the leg starts; 0: not at a vertex
};

// the direction a line of points sets out in, or arrives in
float setOut(const std::vector<RouteGraph::Point> &points, bool atEnd)
{
    const size_t count = points.size();
    if (count < 2)
        return 0;
    size_t a = atEnd ? count - 1 : 0, b = a;
    double run = 0;
    while (run < c_bearingBase && (atEnd ? b > 0 : b + 1 < count)) {
        const size_t next = atEnd ? b - 1 : b + 1;
        run += metersBetween(points[b], points[next]);
        b = next;
    }
    return atEnd ? bearingOf(points[b], points[a]) : bearingOf(points[a], points[b]);
}

} // namespace

void RouteGraph::setAllocator(void *(*allocate)(size_t), void (*release)(void *))
{
    allocator = allocate;
    releaser = release;
}

void *RouteGraph::bigAllocate(size_t bytes)
{
    void *memory = allocator ? allocator(bytes) : nullptr;
    return memory ? memory : malloc(bytes);
}

void RouteGraph::bigRelease(void *memory)
{
    // memory of either source is freed by the one free() where an allocator is set: heap_caps_malloc() and
    // malloc() share it
    if (releaser && allocator)
        releaser(memory);
    else
        free(memory);
}

RouteGraph::State RouteGraph::open(Read read)
{
    close();
    reader = std::move(read);
    uint8_t raw[c_headerSize];
    if (!reader || !reader(0, raw, sizeof(raw)) || memcmp(raw, "JMRD", 4) != 0) {
        reader = nullptr;
        return status = State::NotRoadData;
    }
    if ((raw[4] | raw[5] << 8) != c_version) {
        reader = nullptr;
        return status = State::OtherVersion;
    }
    Header h;
    h.minLat = (int32_t)u32(raw + 8);
    h.minLon = (int32_t)u32(raw + 12);
    h.maxLat = (int32_t)u32(raw + 16);
    h.maxLon = (int32_t)u32(raw + 20);
    h.cell = u32(raw + 24);
    h.columns = u32(raw + 28);
    h.rows = u32(raw + 32);
    h.vertices = u32(raw + 36);
    h.edges = u32(raw + 40);
    h.offCells = u32(raw + 44);
    h.offVertices = u32(raw + 48);
    h.offEdges = u32(raw + 52);
    h.offShapes = u32(raw + 56);
    h.offNames = u32(raw + 60);
    h.size = u32(raw + 64);
    // a file that is cut short, or whose parts do not add up, is not read
    const uint64_t cells = (uint64_t)h.columns * h.rows;
    uint8_t last;
    const bool sound = h.cell > 0 && h.columns > 0 && h.rows > 0 && cells < 4000000 && h.vertices > 1 &&
                       h.vertices < c_seed - 1 && h.offCells == c_headerSize && h.offVertices == h.offCells + 8 * cells &&
                       h.offEdges == (uint64_t)h.offVertices + 12ULL * (h.vertices + 1) &&
                       h.offShapes == (uint64_t)h.offEdges + 16ULL * h.edges && h.offNames > h.offShapes &&
                       h.size > h.offNames && reader(h.size - 1, &last, 1) && last == 0;
    if (!sound) {
        reader = nullptr;
        return status = State::NotRoadData;
    }
    header = h;
    return status = State::Ok;
}

void RouteGraph::close(void)
{
    release();
    reader = nullptr;
    status = State::Closed;
    header = Header();
}

bool RouteGraph::covers(Point at) const
{
    return status == State::Ok && at.latitude >= header.minLat && at.latitude <= header.maxLat && at.longitude >= header.minLon &&
           at.longitude <= header.maxLon;
}

/**
 * Bytes of the file, through the blocks that are kept. Without a cache they are read as they are.
 */
bool RouteGraph::read(uint32_t offset, void *buffer, uint32_t length)
{
    if (status != State::Ok || (uint64_t)offset + length > header.size)
        return false;
    if (!cache)
        return reader(offset, buffer, length);
    uint8_t *out = (uint8_t *)buffer;
    while (length) {
        const uint32_t block = offset / c_block, within = offset % c_block;
        const uint32_t take = std::min(length, c_block - within);
        Slot *set = slots + (size_t)(block % sets) * c_ways;
        Slot *slot = nullptr, *oldest = set;
        for (uint32_t i = 0; i < c_ways; i++) {
            if (set[i].block == block) {
                slot = set + i;
                break;
            }
            if (set[i].used < oldest->used)
                oldest = set + i;
        }
        if (!slot) {
            slot = oldest;
            const uint32_t start = block * c_block;
            slot->block = UINT32_MAX;
            if (!reader(start, cache + (size_t)(slot - slots) * c_block, std::min(c_block, header.size - start))) {
                readFailed = true;
                return false;
            }
            slot->block = block;
        }
        slot->used = ++tick;
        memcpy(out, cache + (size_t)(slot - slots) * c_block + within, take);
        out += take;
        offset += take;
        length -= take;
    }
    return true;
}

bool RouteGraph::vertex(uint32_t number, Vertex &out)
{
    uint8_t raw[12];
    if (number > header.vertices || !read(header.offVertices + 12 * number, raw, sizeof(raw)))
        return false;
    out.latitude = (int32_t)u32(raw);
    out.longitude = (int32_t)u32(raw + 4);
    out.firstEdge = u32(raw + 8);
    return out.firstEdge <= header.edges;
}

bool RouteGraph::edge(uint32_t number, Edge &out)
{
    uint8_t raw[16];
    if (number >= header.edges || !read(header.offEdges + 16 * number, raw, sizeof(raw)))
        return false;
    out.target = u32(raw);
    out.shape = u32(raw + 4);
    out.meta = u32(raw + 8);
    out.name = u32(raw + 12);
    return out.target < header.vertices;
}

bool RouteGraph::usable(const Edge &e, Mode mode) const
{
    return mode == Mode::Car ? e.car() && c_carSpeed[e.kind()] > 0 : e.foot() && c_footFactor[e.kind()] > 0;
}

uint32_t RouteGraph::cost(const Edge &e, Mode mode) const
{
    if (mode == Mode::Car)
        return std::max<uint32_t>(1, (e.length() * c_topSpeed + c_carSpeed[e.kind()] / 2) / c_carSpeed[e.kind()]);
    return std::max<uint32_t>(1, (e.length() * c_footFactor[e.kind()] + 50) / 100);
}

float RouteGraph::speed(uint8_t kind, Mode mode)
{
    if (mode == Mode::Foot)
        return c_footSpeed;
    const uint32_t kmh = c_carSpeed[kind & 15];
    return kmh ? kmh / 3.6f : 5.0f;
}

/**
 * All points of an edge, from the vertex it leaves to its target.
 */
bool RouteGraph::shape(uint32_t from, const Edge &e, std::vector<Point> &out)
{
    out.clear();
    Vertex a, b;
    if (!vertex(e.reversed() ? e.target : from, a) || !vertex(e.reversed() ? from : e.target, b))
        return false;
    uint8_t raw[2];
    const uint32_t at = header.offShapes + e.shape;
    if (e.shape >= header.offNames - header.offShapes || !read(at, raw, 2))
        return false;
    const uint32_t count = raw[0] | raw[1] << 8;
    if (count > c_maxShape || (uint64_t)at + 2 + 4ULL * count > header.offNames)
        return false;
    out.reserve(count + 2);
    Point p{a.latitude, a.longitude};
    out.push_back(p);
    uint8_t chunk[64];
    for (uint32_t done = 0; done < count;) {
        const uint32_t take = std::min<uint32_t>(count - done, sizeof(chunk) / 4);
        if (!read(at + 2 + 4 * done, chunk, 4 * take))
            return false;
        for (uint32_t i = 0; i < take; i++) {
            p.latitude += (int16_t)(chunk[4 * i] | chunk[4 * i + 1] << 8) * 10;
            p.longitude += (int16_t)(chunk[4 * i + 2] | chunk[4 * i + 3] << 8) * 10;
            out.push_back(p);
        }
        done += take;
    }
    out.push_back(Point{b.latitude, b.longitude});
    if (e.reversed())
        std::reverse(out.begin(), out.end());
    return true;
}

/**
 * The record of the same way in the other direction; false when there is none (a one-way road closed to feet).
 */
bool RouteGraph::reverseOf(uint32_t from, const Edge &e, Edge &out)
{
    Vertex v, next;
    if (!vertex(e.target, v) || !vertex(e.target + 1, next))
        return false;
    for (uint32_t i = v.firstEdge; i < next.firstEdge; i++) {
        if (edge(i, out) && out.target == from && out.shape == e.shape && out.reversed() != e.reversed())
            return true;
    }
    return false;
}

std::string RouteGraph::name(uint32_t offset)
{
    char buf[c_maxName + 1] = {0};
    const uint32_t room = header.size - header.offNames;
    if (!offset || offset >= room || !read(header.offNames + offset, buf, std::min<uint32_t>(c_maxName, room - offset)))
        return "";
    return buf;
}

/**
 * The point on a way that the mode may use which is closest to a position.
 */
bool RouteGraph::snap(Point at, Mode mode, Snap &out)
{
    out = Snap();
    if (at.latitude < header.minLat - (int32_t)header.cell || at.longitude < header.minLon - (int32_t)header.cell)
        return false;
    const int64_t cx = ((int64_t)at.longitude - header.minLon) / (int64_t)header.cell,
                  cy = ((int64_t)at.latitude - header.minLat) / (int64_t)header.cell;
    const double k = cos(at.latitude * 1e-7 * M_PI / 180.0);
    const double cellMeters = header.cell * c_unit * std::min(1.0, k); // the short side
    double best = 1e18;
    std::vector<Point> points;
    for (uint32_t ring = 0; ring <= c_maxRings; ring++) {
        // a way through a cell has a vertex within one cell of it: one ring more than the best find is far away
        if (out.found && best < ((double)ring - 1.0) * cellMeters)
            break;
        for (int64_t y = cy - ring; y <= cy + ring; y++) {
            for (int64_t x = cx - ring; x <= cx + ring; x++) {
                if (std::max(std::llabs(x - cx), std::llabs(y - cy)) != (int64_t)ring || x < 0 || y < 0 ||
                    x >= (int64_t)header.columns || y >= (int64_t)header.rows)
                    continue;
                uint8_t raw[8];
                if (!read(header.offCells + 8 * (uint32_t)(y * header.columns + x), raw, sizeof(raw)))
                    return false;
                const uint32_t first = u32(raw), count = u32(raw + 4);
                if (first > header.vertices || count > header.vertices - first)
                    return false;
                for (uint32_t v = first; v < first + count; v++) {
                    Vertex here, next;
                    if (!vertex(v, here) || !vertex(v + 1, next))
                        return false;
                    for (uint32_t i = here.firstEdge; i < next.firstEdge; i++) {
                        Edge e;
                        if (!edge(i, e))
                            return false;
                        if (!usable(e, mode) || !shape(v, e, points))
                            continue;
                        double run = 0;
                        for (size_t s = 0; s + 1 < points.size(); s++) {
                            // the closest point of the stretch, on a plane around the position
                            const double ax = ((double)points[s].longitude - at.longitude) * k,
                                         ay = (double)points[s].latitude - at.latitude,
                                         bx = ((double)points[s + 1].longitude - at.longitude) * k,
                                         by = (double)points[s + 1].latitude - at.latitude;
                            const double dx = bx - ax, dy = by - ay, norm = dx * dx + dy * dy;
                            const double t = norm > 0 ? std::clamp(-(ax * dx + ay * dy) / norm, 0.0, 1.0) : 0.0;
                            const double px = ax + t * dx, py = ay + t * dy;
                            const double off = sqrt(px * px + py * py) * c_unit;
                            const double stretch = sqrt(norm) * c_unit;
                            if (off < best) {
                                best = off;
                                out.found = true;
                                out.from = v;
                                out.edge = e;
                                out.segment = (uint32_t)s;
                                out.at.latitude = points[s].latitude + (int32_t)lround(t * (points[s + 1].latitude - points[s].latitude));
                                out.at.longitude =
                                    points[s].longitude + (int32_t)lround(t * (points[s + 1].longitude - points[s].longitude));
                                out.along = (uint32_t)lround(run + t * stretch);
                                out.off = off;
                            }
                            run += stretch;
                        }
                    }
                }
            }
        }
    }
    if (out.found)
        out.along = std::min(out.along, out.edge.length());
    return out.found;
}

// -------- the search --------

bool RouteGraph::allocate(void)
{
    release();
    // the cache first, since nothing goes without it; then the largest table there is room for
    for (uint32_t blocks : {256u, 128u, 64u, 32u}) {
        cache = (uint8_t *)bigAllocate((size_t)blocks * c_block);
        if (cache) {
            sets = blocks / c_ways;
            break;
        }
    }
    if (!cache)
        return false;
    slots = new (std::nothrow) Slot[(size_t)sets * c_ways];
    if (!slots)
        return false;
    tick = 0;
    for (uint32_t capacity : {1u << 18, 1u << 17, 1u << 16, 1u << 15, 1u << 14}) {
        search.visits = (Visit *)bigAllocate((size_t)capacity * sizeof(Visit));
        search.heap = search.visits ? (Open *)bigAllocate((size_t)capacity * 2 * sizeof(Open)) : nullptr;
        if (search.visits && search.heap) {
            search.capacity = capacity;
            search.heapCapacity = capacity * 2;
            memset(search.visits, 0, (size_t)capacity * sizeof(Visit));
            return true;
        }
        if (search.visits)
            bigRelease(search.visits);
        search.visits = nullptr;
    }
    return false;
}

void RouteGraph::release(void)
{
    if (search.visits)
        bigRelease(search.visits);
    if (search.heap)
        bigRelease(search.heap);
    if (cache)
        bigRelease(cache);
    delete[] slots;
    search.visits = nullptr;
    search.heap = nullptr;
    search.capacity = search.count = search.heapCapacity = search.heapCount = 0;
    cache = nullptr;
    slots = nullptr;
    sets = 0;
}

void RouteGraph::cancel(void)
{
    release();
    search.progress = Progress::Idle;
}

RouteGraph::Visit *RouteGraph::visit(uint32_t vertex, bool add)
{
    const uint32_t mask = search.capacity - 1;
    uint32_t at = (vertex * 2654435761u) >> 7 & mask;
    for (;;) {
        Visit &v = search.visits[at];
        if (v.vertex == vertex + 1)
            return &v;
        if (v.vertex == 0) {
            if (!add || search.count >= search.capacity / 4 * 3)
                return nullptr;
            search.count++;
            v.vertex = vertex + 1;
            v.cost = UINT32_MAX;
            v.parent = 0;
            return &v;
        }
        at = (at + 1) & mask;
    }
}

bool RouteGraph::push(uint32_t f, uint32_t vertex)
{
    if (search.heapCount >= search.heapCapacity)
        return false;
    uint32_t at = search.heapCount++;
    while (at > 0) {
        const uint32_t up = (at - 1) / 2;
        if (search.heap[up].f <= f)
            break;
        search.heap[at] = search.heap[up];
        at = up;
    }
    search.heap[at] = Open{f, vertex};
    return true;
}

/**
 * What the way from a vertex to the destination costs at least, weighted: the search leans towards the
 * destination and settles far fewer vertices, for a route that can be a little longer than the best one.
 */
uint32_t RouteGraph::bound(const Vertex &v) const
{
    const double straight = metersBetween(Point{v.latitude, v.longitude}, search.end.at);
    return (uint32_t)(straight * c_weight / 8);
}

bool RouteGraph::relax(uint32_t vertex, uint32_t cost, uint32_t parent)
{
    Visit *seen = visit(vertex, true);
    if (!seen)
        return false;
    if ((seen->parent & 0x80000000u) || seen->cost <= cost)
        return true;
    Vertex v;
    if (!this->vertex(vertex, v))
        return false;
    seen->cost = cost;
    seen->parent = parent;
    const double straight = metersBetween(Point{v.latitude, v.longitude}, search.end.at);
    if (straight < search.closest)
        search.closest = straight;
    return push(cost + (uint32_t)(straight * c_weight / 8), vertex);
}

RouteGraph::Progress RouteGraph::begin(Point from, Point to, Mode mode)
{
    if (status != State::Ok)
        return Progress::ReadError;
    cancel();
    search = Search();
    search.mode = mode;
    search.from = from;
    search.to = to;
    readFailed = false;
    if (!allocate()) {
        release();
        return search.progress = Progress::NoMemory;
    }
    auto fail = [this](Progress why) {
        release();
        return search.progress = readFailed ? Progress::ReadError : why;
    };
    if (!snap(from, mode, search.start))
        return fail(Progress::NoWayNearStart);
    if (!snap(to, mode, search.end))
        return fail(Progress::NoWayNearEnd);
    Snap &start = search.start, &end = search.end;
    Edge startBack, endBack;
    const bool startTwoWay = reverseOf(start.from, start.edge, startBack) && usable(startBack, mode);
    // both on one edge: seen from the same end, the way between them may be open
    if (start.edge.shape == end.edge.shape && start.from != end.from && end.edge.target == start.from) {
        Edge other;
        if (reverseOf(end.from, end.edge, other) && usable(other, mode)) {
            std::vector<Point> points;
            if (!shape(end.from, end.edge, points))
                return fail(Progress::ReadError);
            const uint32_t segments = (uint32_t)points.size() - 1;
            end.from = start.from;
            end.along = end.edge.length() - std::min(end.along, end.edge.length());
            end.segment = segments - 1 - std::min(end.segment, segments - 1);
            end.edge = other;
        }
    }
    const bool endTwoWay = reverseOf(end.from, end.edge, endBack) && usable(endBack, mode);
    if (start.edge.shape == end.edge.shape && start.from == end.from &&
        (end.along >= start.along || (startTwoWay && endTwoWay))) {
        search.sameWay = true;
        return search.progress = Progress::Done;
    }
    const uint32_t length = std::max<uint32_t>(1, start.edge.length());
    search.startDistance = search.closest = metersBetween(start.at, end.at);
    bool ok = relax(start.edge.target, (uint32_t)((uint64_t)cost(start.edge, mode) * (length - start.along) / length), c_seed);
    if (ok && startTwoWay)
        ok = relax(start.from, (uint32_t)((uint64_t)cost(startBack, mode) * start.along / length), c_seed);
    if (!ok)
        return fail(Progress::ReadError);
    const uint32_t endLength = std::max<uint32_t>(1, end.edge.length());
    search.goal[0] = end.from;
    search.goalCost[0] = (uint32_t)((uint64_t)cost(end.edge, mode) * end.along / endLength);
    if (endTwoWay) {
        search.goal[1] = end.edge.target;
        search.goalCost[1] = (uint32_t)((uint64_t)cost(endBack, mode) * (endLength - end.along) / endLength);
    }
    return search.progress = Progress::Running;
}

RouteGraph::Progress RouteGraph::work(uint32_t milliseconds, uint32_t (*clock)(void))
{
    if (search.progress != Progress::Running)
        return search.progress;
    const uint32_t started = clock ? clock() : 0;
    auto fail = [this](Progress why) {
        release();
        return search.progress = readFailed ? Progress::ReadError : why;
    };
    for (uint32_t round = 0;; round++) {
        if (round && (round & 15) == 0 && (!clock || clock() - started >= milliseconds))
            return search.progress;
        if (search.heapCount == 0 || search.heap[0].f >= search.best) {
            if (search.best == UINT32_MAX)
                return fail(Progress::NoRoute);
            return search.progress = Progress::Done;
        }
        // the cheapest open vertex
        const Open top = search.heap[0];
        const Open last = search.heap[--search.heapCount];
        uint32_t at = 0;
        for (;;) {
            uint32_t child = 2 * at + 1;
            if (child >= search.heapCount)
                break;
            if (child + 1 < search.heapCount && search.heap[child + 1].f < search.heap[child].f)
                child++;
            if (search.heap[child].f >= last.f)
                break;
            search.heap[at] = search.heap[child];
            at = child;
        }
        if (search.heapCount)
            search.heap[at] = last;

        Visit *seen = visit(top.vertex, false);
        if (!seen || (seen->parent & 0x80000000u))
            continue; // an older entry of a vertex that is done
        seen->parent |= 0x80000000u;
        search.settled++;
        const uint32_t reached = seen->cost;
        for (int i = 0; i < 2; i++) {
            if (top.vertex == search.goal[i] && reached + search.goalCost[i] < search.best) {
                search.best = reached + search.goalCost[i];
                search.bestVertex = top.vertex;
            }
        }
        Vertex here, next;
        if (!vertex(top.vertex, here) || !vertex(top.vertex + 1, next))
            return fail(Progress::ReadError);
        for (uint32_t i = here.firstEdge; i < next.firstEdge; i++) {
            Edge e;
            if (!edge(i, e))
                return fail(Progress::ReadError);
            if (!usable(e, search.mode))
                continue;
            if (!relax(e.target, reached + cost(e, search.mode), top.vertex))
                return fail(readFailed ? Progress::ReadError : Progress::TooFar);
        }
    }
}

uint8_t RouteGraph::percent(void) const
{
    if (search.progress == Progress::Done)
        return 100;
    if (search.startDistance <= 0)
        return 0;
    return (uint8_t)std::clamp(100.0 * (1.0 - search.closest / search.startDistance), 0.0, 99.0);
}

bool RouteGraph::result(Route &out)
{
    out = Route();
    if (search.progress != Progress::Done)
        return false;
    const Mode mode = search.mode;
    const Snap &start = search.start, &end = search.end;
    std::vector<Leg> legs;
    std::vector<Point> points;
    bool ok = true;

    // the part of a snapped edge between two of its points, given as meters along and the stretch they lie on
    auto part = [&points](const Snap &a, bool fromSnap, bool towardsTarget, std::vector<Point> &into) {
        // points holds the whole edge from its source vertex to its target
        into.clear();
        const size_t count = points.size();
        const size_t segment = std::min<size_t>(a.segment, count >= 2 ? count - 2 : 0);
        if (fromSnap) {
            into.push_back(a.at);
            if (towardsTarget)
                into.insert(into.end(), points.begin() + segment + 1, points.end());
            else
                for (size_t i = segment + 1; i-- > 0;)
                    into.push_back(points[i]);
        } else {
            if (towardsTarget) // from the source vertex to the snap
                into.insert(into.end(), points.begin(), points.begin() + segment + 1);
            else
                for (size_t i = count; i-- > segment + 1;)
                    into.push_back(points[i]);
            into.push_back(a.at);
        }
    };

    if (search.sameWay) {
        ok = shape(start.from, start.edge, points);
        if (ok) {
            Leg leg;
            leg.name = start.edge.name;
            leg.kind = start.edge.kind();
            const size_t count = points.size();
            const size_t a = std::min<size_t>(start.segment, count - 2), b = std::min<size_t>(end.segment, count - 2);
            leg.points.push_back(start.at);
            if (end.along >= start.along)
                for (size_t i = a + 1; i <= b; i++)
                    leg.points.push_back(points[i]);
            else
                for (size_t i = a + 1; i-- > b + 1;)
                    leg.points.push_back(points[i]);
            leg.points.push_back(end.at);
            leg.meters = end.along >= start.along ? end.along - start.along : start.along - end.along;
            legs.push_back(std::move(leg));
        }
    } else {
        // the vertices from the destination back to where the search set out
        std::vector<uint32_t> chain;
        uint32_t v = search.bestVertex;
        for (;;) {
            chain.push_back(v);
            Visit *seen = visit(v, false);
            if (!seen || chain.size() > search.count + 1) {
                ok = false;
                break;
            }
            const uint32_t parent = seen->parent & 0x7FFFFFFFu;
            if (parent == c_seed)
                break;
            v = parent;
        }
        std::reverse(chain.begin(), chain.end());
        if (ok) {
            // from the start to the first vertex
            Leg leg;
            leg.name = start.edge.name;
            leg.kind = start.edge.kind();
            ok = shape(start.from, start.edge, points);
            const bool forward = chain.front() == start.edge.target && !(chain.front() == start.from && start.along == 0);
            if (ok)
                part(start, true, forward, leg.points);
            leg.meters = forward ? start.edge.length() - std::min(start.along, start.edge.length()) : start.along;
            legs.push_back(std::move(leg));
        }
        for (size_t i = 0; ok && i + 1 < chain.size(); i++) {
            Vertex here, next;
            ok = vertex(chain[i], here) && vertex(chain[i] + 1, next);
            Edge pick{};
            uint32_t least = UINT32_MAX;
            for (uint32_t n = here.firstEdge; ok && n < next.firstEdge; n++) {
                Edge e;
                ok = edge(n, e);
                if (ok && e.target == chain[i + 1] && usable(e, mode) && cost(e, mode) < least) {
                    least = cost(e, mode);
                    pick = e;
                }
            }
            ok = ok && least != UINT32_MAX;
            Leg leg;
            leg.name = pick.name;
            leg.kind = pick.kind();
            leg.meters = pick.length();
            leg.degree = ok ? next.firstEdge - here.firstEdge : 0;
            std::vector<Point> full;
            ok = ok && shape(chain[i], pick, full);
            leg.points = std::move(full);
            legs.push_back(std::move(leg));
        }
        if (ok) {
            // and from the last vertex to the end
            Leg leg;
            leg.name = end.edge.name;
            leg.kind = end.edge.kind();
            ok = shape(end.from, end.edge, points);
            const bool fromSource = chain.back() == end.from;
            if (ok)
                part(end, false, fromSource, leg.points);
            leg.meters = fromSource ? end.along : end.edge.length() - std::min(end.along, end.edge.length());
            Vertex here, next;
            if (ok && vertex(chain.back(), here) && vertex(chain.back() + 1, next))
                leg.degree = next.firstEdge - here.firstEdge;
            legs.push_back(std::move(leg));
        }
    }
    const bool failed = readFailed;
    cancel();
    if (!ok || legs.empty()) {
        search.progress = failed ? Progress::ReadError : Progress::NoRoute;
        return false;
    }

    // one line of points, and a step wherever the way turns or changes its name
    size_t total = 0;
    for (const Leg &leg : legs)
        total += leg.points.size();
    out.points.reserve(total);
    out.along.reserve(total);
    float seconds = 0;
    float arriving = 0;
    std::string stepName;
    for (size_t i = 0; i < legs.size(); i++) {
        const Leg &leg = legs[i];
        if (leg.points.size() < 2)
            continue;
        const float leaving = setOut(leg.points, false);
        const std::string legName = name(leg.name);
        if (out.steps.empty()) {
            Step step;
            step.turn = Start;
            step.name = legName;
            step.kind = leg.kind;
            step.bearing = leaving;
            out.steps.push_back(step);
            stepName = legName;
        } else {
            float turn = leaving - arriving;
            while (turn > 180)
                turn -= 360;
            while (turn < -180)
                turn += 360;
            const float sharp = fabsf(turn);
            const bool turned = leg.degree >= 3 && sharp >= 40;
            const bool renamed = !legName.empty() && legName != stepName;
            if (turned || renamed) {
                Step step;
                step.turn = sharp < 20    ? Continue
                            : sharp < 50  ? (turn < 0 ? SlightLeft : SlightRight)
                            : sharp < 130 ? (turn < 0 ? Left : Right)
                            : sharp < 170 ? (turn < 0 ? SharpLeft : SharpRight)
                                          : UTurn;
                step.name = legName;
                step.kind = leg.kind;
                step.point = out.points.empty() ? 0 : (uint32_t)out.points.size() - 1;
                step.bearing = leaving;
                out.steps.push_back(step);
                stepName = legName;
            } else if (out.steps.back().name.empty() && !legName.empty()) {
                out.steps.back().name = stepName = legName; // it set out on a stretch without a name
            }
        }
        arriving = setOut(leg.points, true);
        // the meters of the leg are spread over its points by their own distances
        double shapeMeters = 0;
        for (size_t p = 0; p + 1 < leg.points.size(); p++)
            shapeMeters += metersBetween(leg.points[p], leg.points[p + 1]);
        const double scale = shapeMeters > 0 ? leg.meters / shapeMeters : 0;
        double run = 0;
        for (size_t p = 0; p < leg.points.size(); p++) {
            if (p)
                run += metersBetween(leg.points[p - 1], leg.points[p]) * scale;
            if (p == 0 && !out.points.empty())
                continue; // where the leg before ended
            out.points.push_back(leg.points[p]);
            out.along.push_back(out.meters + (uint32_t)lround(run));
        }
        out.meters += leg.meters;
        out.steps.back().meters += leg.meters;
        seconds += leg.meters / speed(leg.kind, mode);
    }
    if (out.points.size() < 2) {
        // start and end are the same spot of a way
        out.points.assign(2, start.at);
        out.along.assign(2, 0);
        if (out.steps.empty())
            out.steps.push_back(Step());
    }
    Step arrive;
    arrive.turn = Arrive;
    arrive.point = (uint32_t)out.points.size() - 1;
    out.steps.push_back(arrive);
    out.seconds = (uint32_t)seconds;
    out.startOff = (uint32_t)lround(start.off);
    out.endOff = (uint32_t)lround(end.off);
    search.progress = Progress::Idle;
    return true;
}

const char *RouteGraph::turnText(uint8_t turn)
{
    static const char *const names[] = {"Head",      "Continue",     "Slight left", "Left",   "Sharp left",
                                        "Slight right", "Right",    "Sharp right", "U-turn", "Arrive"};
    return turn < sizeof(names) / sizeof(names[0]) ? names[turn] : "";
}
