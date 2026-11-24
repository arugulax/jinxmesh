#!/usr/bin/env python3
"""roads.py: the routing file of the M9 keyboard view, /geo/roads.bin, from OpenStreetMap extracts.

  roads.py build OUT.bin A.osm.pbf [B.osm.pbf ...] [--bbox minlon,minlat,maxlon,maxlat]
  roads.py route FILE.bin lat,lon lat,lon [car|foot]     the reference router, for comparing the device's with
  roads.py info FILE.bin

Roads, tracks, paths and ferries become a graph: a vertex wherever ways meet or end and at least every 800 m,
an edge with its shape in between. Vertices are sorted by the 0.01 degree cell they lie in (cells in Z order),
so that what is close on the ground is close in the file: the device reads the file in blocks and keeps few.

Layout, little endian (docs/MAP-PACKS.md has the same in words):
  header 68 bytes: "JMRD", u16 version 1, u16 0, i32 minLat, minLon, maxLat, maxLon (degrees 1e7), u32 cell size
      (1e7), u32 columns, u32 rows, u32 vertices, u32 directed edges, u32 offsets of: cells, vertices, edges,
      shapes, names; u32 file size
  cells: per cell, row by row from the south-west: u32 first vertex, u32 count
  vertices (+ one closing record): i32 lat, i32 lon, u32 first edge
  edges, by source vertex: u32 target, u32 shape offset, u32 meta, u32 name offset
      meta: length in meters (20 bits), class << 20 (4 bits), car << 24, foot << 25, shape is stored from the
      target << 26
  shapes: u16 n, then n times i16 dlat, i16 dlon in 1e-6 degrees, each from the point before, the first from
      the vertex the shape starts at; the end vertex is not in it
  names: zero-terminated UTF-8, offset 0 is the empty name

Data: OpenStreetMap contributors, ODbL 1.0. The file is a derived database: say so where it is passed on.
"""
import heapq
import math
import struct
import sys
from array import array

MAGIC = b"JMRD"
VERSION = 1
CELL = 100000  # 0.01 degrees in 1e-7
MAX_EDGE = 800.0  # meters
SMALL_COMPONENT = 40  # vertices; smaller islands are left out
SIMPLIFY = 3.0  # meters

CLASSES = ["motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential", "service",
           "track", "path", "cycleway", "steps", "ferry"]
CLASS_OF = {
    "motorway": 0, "motorway_link": 0, "trunk": 1, "trunk_link": 1, "primary": 2, "primary_link": 2,
    "secondary": 3, "secondary_link": 3, "tertiary": 4, "tertiary_link": 4, "unclassified": 5, "road": 5,
    "residential": 6, "living_street": 6, "service": 7, "track": 8, "path": 9, "footway": 9, "bridleway": 9,
    "pedestrian": 9, "cycleway": 10, "steps": 11,
}
# km/h a car is taken to make; what the device uses is in RouteGraph.cpp and must stay the same
CAR_SPEED = [105, 90, 80, 70, 60, 50, 40, 20, 15, 0, 0, 0, 15]
TOP_SPEED = 105.0
FOOT_FACTOR = [0, 1.3, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.1, 3.0]
NO = {"no", "private", "agricultural", "forestry", "delivery", "customers", "military", "emergency"}
YES = {"yes", "permissive", "designated", "destination", "public", "official"}


def meters(lat1, lon1, lat2, lon2):
    """degrees 1e-7 in, meters out"""
    r = math.pi / 180e7
    a = math.sin((lat2 - lat1) * r / 2) ** 2 + math.cos(lat1 * r) * math.cos(lat2 * r) * math.sin((lon2 - lon1) * r / 2) ** 2
    return 6371000.0 * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def classify(tags):
    """(class, car forward, car backward, foot, name) or None"""
    highway = tags.get("highway")
    if highway in CLASS_OF:
        cls = CLASS_OF[highway]
    elif tags.get("route") == "ferry":
        cls = 12
    else:
        return None
    if tags.get("area") == "yes":
        return None
    access = tags.get("access")
    motor = tags.get("motor_vehicle", tags.get("motorcar", tags.get("vehicle")))
    foot = tags.get("foot")
    car = cls <= 8 or cls == 12
    if car and (motor in NO or (access in NO and motor not in YES)):
        car = False
    if car and cls == 8 and tags.get("tracktype") in ("grade4", "grade5"):
        car = False
    walk = cls != 0
    if walk and (foot in NO or (access in NO and foot not in YES)):
        walk = False
    if cls == 1 and foot not in YES and tags.get("sidewalk") in (None, "no", "none"):
        walk = walk and tags.get("motorroad") != "yes"
    if not car and not walk:
        return None
    oneway = tags.get("oneway")
    forward = backward = car
    if oneway in ("yes", "1", "true") or (oneway is None and (cls == 0 and highway == "motorway" or tags.get("junction") in ("roundabout", "circular"))):
        backward = False
    elif oneway in ("-1", "reverse"):
        forward = False
    name = tags.get("name", "")
    ref = tags.get("ref", "")
    if ref and (cls <= 3 or not name):
        name = ref.split(";")[0].strip() if not name or cls <= 3 else name
    return cls, forward, backward, walk, " ".join(name.split())[:60]


def read_ways(paths, bbox):
    import osmium

    ways = {}  # id -> (class, fwd, back, foot, name, start, count) into the flat arrays
    ids, lats, lons = array("q"), array("i"), array("i")
    minlon, minlat, maxlon, maxlat = bbox

    class Handler(osmium.SimpleHandler):
        def way(self, w):
            if w.id in ways:
                return
            tags = {t.k: t.v for t in w.tags}
            kind = classify(tags)
            if kind is None or len(w.nodes) < 2:
                return
            start = len(ids)
            inside = False
            try:
                for n in w.nodes:
                    lat, lon = n.location.lat, n.location.lon
                    inside = inside or (minlat <= lat <= maxlat and minlon <= lon <= maxlon)
                    ids.append(n.ref)
                    lats.append(int(round(lat * 1e6)) * 10)
                    lons.append(int(round(lon * 1e6)) * 10)
            except osmium.InvalidLocationError:
                inside = False
            if not inside:
                del ids[start:], lats[start:], lons[start:]
                return
            ways[w.id] = kind + (start, len(ids) - start)

    for path in paths:
        print("reading", path, flush=True)
        Handler().apply_file(path, locations=True, idx="flex_mem")
        print("  ways so far:", len(ways), "points:", len(ids), flush=True)
    return ways, ids, lats, lons


def simplify(points, tolerance):
    """Douglas-Peucker on (lat, lon) in 1e-7 degrees; keeps both ends"""
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    k = math.cos(points[0][0] * math.pi / 180e7)
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        ax, ay = points[a][1] * k, points[a][0]
        bx, by = points[b][1] * k, points[b][0]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        worst, at = -1.0, -1
        for i in range(a + 1, b):
            px, py = points[i][1] * k - ax, points[i][0] - ay
            if norm:
                t = max(0.0, min(1.0, (px * dx + py * dy) / (norm * norm)))
                d = math.hypot(px - t * dx, py - t * dy)
            else:
                d = math.hypot(px, py)
            if d > worst:
                worst, at = d, i
        if worst * 0.0111 > tolerance:  # 1e-7 degrees of latitude are 1.11 cm
            keep[at] = True
            stack.append((a, at))
            stack.append((at, b))
    return [p for p, k_ in zip(points, keep) if k_]


def morton(x, y):
    code = 0
    for bit in range(16):
        code |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
    return code


def build(out, paths, bbox):
    ways, ids, lats, lons = read_ways(paths, bbox)
    # a node is a vertex where ways meet or end
    uses = {}
    for cls, fwd, back, foot, name, start, count in ways.values():
        for i in range(start, start + count):
            n = ids[i]
            uses[n] = uses.get(n, 0) + (2 if i in (start, start + count - 1) else 1)
    print("nodes on ways:", len(uses), flush=True)

    vertex_of = {}  # node id (or a negative made-up one) -> index
    vlat, vlon = array("i"), array("i")

    def vertex(key, lat, lon):
        at = vertex_of.get(key)
        if at is None:
            at = vertex_of[key] = len(vlat)
            vlat.append(lat)
            vlon.append(lon)
        return at

    edges = []  # (a, b, class, fwd, back, foot, name, length, points between)
    made = [0]

    def emit(a, b, kind, pts):
        """pts: all points from a to b, both included"""
        length = sum(meters(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]) for i in range(len(pts) - 1))
        if a == b and length < 1:
            return
        inner = simplify(pts, SIMPLIFY)[1:-1]
        edges.append((a, b) + kind + (max(1, int(round(length))), inner))

    for cls, fwd, back, foot, name, start, count in ways.values():
        kind = (cls, fwd, back, foot, name)
        pts = [(lats[start], lons[start])]
        a = vertex(ids[start], lats[start], lons[start])
        run = 0.0
        for i in range(start + 1, start + count):
            lat, lon = lats[i], lons[i]
            step = meters(pts[-1][0], pts[-1][1], lat, lon)
            # one stretch without a node that is too long gets vertices of its own
            while step > MAX_EDGE:
                f = MAX_EDGE * 0.9 / step
                mlat = int(round((pts[-1][0] + (lat - pts[-1][0]) * f) / 10)) * 10
                mlon = int(round((pts[-1][1] + (lon - pts[-1][1]) * f) / 10)) * 10
                made[0] -= 1
                b = vertex(made[0], mlat, mlon)
                pts.append((mlat, mlon))
                emit(a, b, kind, pts)
                a, pts, run = b, [(mlat, mlon)], 0.0
                step = meters(mlat, mlon, lat, lon)
            pts.append((lat, lon))
            run += step
            last = i == start + count - 1
            if last or uses[ids[i]] > 1 or run > MAX_EDGE * 0.75:
                b = vertex(ids[i], lat, lon)
                emit(a, b, kind, pts)
                a, pts, run = b, [(lat, lon)], 0.0
    del ways, ids, lats, lons, uses
    print("vertices:", len(vlat), "edges:", len(edges), "made-up vertices:", -made[0], flush=True)

    # islands nobody can route out of are left out
    parent = list(range(len(vlat)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for e in edges:
        ra, rb = find(e[0]), find(e[1])
        if ra != rb:
            parent[ra] = rb
    size = {}
    for v in range(len(vlat)):
        r = find(v)
        size[r] = size.get(r, 0) + 1
    edges = [e for e in edges if size[find(e[0])] >= SMALL_COMPONENT]
    used = set()
    for e in edges:
        used.add(e[0])
        used.add(e[1])
    print("after dropping small islands: vertices", len(used), "edges", len(edges), flush=True)

    minlat = min(vlat[v] for v in used) // CELL * CELL
    minlon = min(vlon[v] for v in used) // CELL * CELL
    maxlat = max(vlat[v] for v in used)
    maxlon = max(vlon[v] for v in used)
    cols = (maxlon - minlon) // CELL + 1
    rows = (maxlat - minlat) // CELL + 1

    def cell(v):
        return (vlon[v] - minlon) // CELL, (vlat[v] - minlat) // CELL

    order = sorted(used, key=lambda v: (morton(*cell(v)), vlat[v], vlon[v]))
    index = {v: i for i, v in enumerate(order)}
    cells = [[0, 0] for _ in range(cols * rows)]
    for i, v in enumerate(order):
        x, y = cell(v)
        c = cells[y * cols + x]
        if c[1] == 0:
            c[0] = i
        c[1] += 1

    names = bytearray(b"\0")
    name_at = {"": 0}

    def name_offset(name):
        at = name_at.get(name)
        if at is None:
            at = name_at[name] = len(names)
            names.extend(name.encode("utf-8") + b"\0")
        return at

    shapes = bytearray()
    out_edges = [[] for _ in order]  # per vertex: (target, shape, meta, name)
    for a, b, cls, fwd, back, foot, name, length, inner in edges:
        shape = len(shapes)
        shapes += struct.pack("<H", len(inner))
        plat, plon = vlat[a], vlon[a]
        for lat, lon in inner:
            dlat, dlon = (lat - plat) // 10, (lon - plon) // 10
            assert -32768 <= dlat <= 32767 and -32768 <= dlon <= 32767, (dlat, dlon)
            shapes += struct.pack("<hh", dlat, dlon)
            plat, plon = lat, lon
        length = min(length, (1 << 20) - 1)
        n = name_offset(name)
        if fwd or foot:
            out_edges[index[a]].append((index[b], shape, length | cls << 20 | int(fwd) << 24 | int(foot) << 25, n))
        if back or foot:
            out_edges[index[b]].append((index[a], shape, length | cls << 20 | int(back) << 24 | int(foot) << 25 | 1 << 26, n))

    directed = sum(len(e) for e in out_edges)
    off_cells = 68
    off_vertices = off_cells + 8 * len(cells)
    off_edges = off_vertices + 12 * (len(order) + 1)
    off_shapes = off_edges + 16 * directed
    off_names = off_shapes + len(shapes)
    total = off_names + len(names)
    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<HH4i3I2I5II", VERSION, 0, minlat, minlon, maxlat, maxlon, CELL, cols, rows, len(order),
                                    directed, off_cells, off_vertices, off_edges, off_shapes, off_names, total))
        assert f.tell() == 68
        for first, count in cells:
            f.write(struct.pack("<II", first, count))
        first = 0
        for i, v in enumerate(order):
            f.write(struct.pack("<iiI", vlat[v], vlon[v], first))
            first += len(out_edges[i])
        f.write(struct.pack("<iiI", 0, 0, first))
        for list_ in out_edges:
            for record in list_:
                f.write(struct.pack("<IIII", *record))
        f.write(shapes)
        f.write(names)
        assert f.tell() == total
    print(f"{out}: {total:,} bytes, {len(order):,} vertices, {directed:,} directed edges, {len(name_at):,} names, "
          f"{cols} x {rows} cells", flush=True)


class Graph:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        assert d[:4] == MAGIC
        (self.version, _, self.minlat, self.minlon, self.maxlat, self.maxlon, self.cell, self.cols, self.rows, self.nv,
         self.ne, self.oc, self.ov, self.oe, self.os, self.on, self.size) = struct.unpack_from("<HH4i3I2I5II", d, 4)
        assert self.size == len(d)

    def vertex(self, v):
        return struct.unpack_from("<iiI", self.d, self.ov + 12 * v)

    def edges(self, v):
        first = self.vertex(v)[2]
        last = self.vertex(v + 1)[2]
        for e in range(first, last):
            target, shape, meta, name = struct.unpack_from("<IIII", self.d, self.oe + 16 * e)
            yield e, target, shape, meta, name

    def name(self, at):
        end = self.d.index(b"\0", self.on + at)
        return self.d[self.on + at:end].decode("utf-8")

    def shape(self, v, target, shape, meta):
        """all points from v to target"""
        a, b = (target, v) if meta >> 26 & 1 else (v, target)
        lat, lon, _ = self.vertex(a)
        pts = [(lat, lon)]
        (n,) = struct.unpack_from("<H", self.d, self.os + shape)
        for i in range(n):
            dlat, dlon = struct.unpack_from("<hh", self.d, self.os + shape + 2 + 4 * i)
            lat, lon = lat + dlat * 10, lon + dlon * 10
            pts.append((lat, lon))
        pts.append(self.vertex(b)[:2])
        return pts[::-1] if meta >> 26 & 1 else pts

    def nearest(self, lat, lon, car):
        """the vertex of a usable edge closest to a position: good enough for the reference"""
        cx, cy = (lon - self.minlon) // self.cell, (lat - self.minlat) // self.cell
        best = (1e18, None)
        for ring in range(0, 40):
            for y in range(cy - ring, cy + ring + 1):
                for x in range(cx - ring, cx + ring + 1):
                    if max(abs(x - cx), abs(y - cy)) != ring or not (0 <= x < self.cols and 0 <= y < self.rows):
                        continue
                    first, count = struct.unpack_from("<II", self.d, self.oc + 8 * (y * self.cols + x))
                    for v in range(first, first + count):
                        if not any(meta >> (24 if car else 25) & 1 for _, _, _, meta, _ in self.edges(v)):
                            continue
                        vlat, vlon, _ = self.vertex(v)
                        d = meters(lat, lon, vlat, vlon)
                        if d < best[0]:
                            best = (d, v)
            if best[1] is not None and ring >= 1 and best[0] < ring * 800:
                break
        return best[1]


def cost_of(meta, car):
    length, cls = meta & 0xFFFFF, meta >> 20 & 15
    if car:
        if not meta >> 24 & 1 or not CAR_SPEED[cls]:
            return None
        return length * TOP_SPEED / CAR_SPEED[cls]
    if not meta >> 25 & 1:
        return None
    return length * FOOT_FACTOR[cls]


def route(g, start, goal, car):
    """plain Dijkstra with the straight-line bound: the optimal cost, for comparing the device's against"""
    glat, glon, _ = g.vertex(goal)
    best = {start: 0.0}
    prev = {}
    heap = [(0.0, start)]
    done = set()
    while heap:
        f, v = heapq.heappop(heap)
        if v in done:
            continue
        done.add(v)
        if v == goal:
            break
        for e, target, shape, meta, name in g.edges(v):
            c = cost_of(meta, car)
            if c is None:
                continue
            cost = best[v] + c
            if cost < best.get(target, 1e18):
                best[target] = cost
                prev[target] = (v, meta, name)
                tlat, tlon, _ = g.vertex(target)
                heapq.heappush(heap, (cost + meters(tlat, tlon, glat, glon), target))
    if goal not in best:
        return None
    path, length = [], 0
    v = goal
    while v != start:
        v, meta, name = prev[v]
        path.append((v, meta, name))
        length += meta & 0xFFFFF
    return best[goal], length, path[::-1], len(done)


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "build":
        args = sys.argv[2:]
        bbox = (-180.0, -90.0, 180.0, 90.0)
        if "--bbox" in args:
            at = args.index("--bbox")
            bbox = tuple(float(x) for x in args[at + 1].split(","))
            del args[at:at + 2]
        build(args[0], args[1:], bbox)
    elif len(sys.argv) >= 3 and sys.argv[1] == "info":
        g = Graph(sys.argv[2])
        print(f"version {g.version}, {g.nv:,} vertices, {g.ne:,} directed edges, {g.cols} x {g.rows} cells, "
              f"{g.minlat / 1e7:.4f},{g.minlon / 1e7:.4f} to {g.maxlat / 1e7:.4f},{g.maxlon / 1e7:.4f}, {g.size:,} bytes")
    elif len(sys.argv) >= 5 and sys.argv[1] == "route":
        g = Graph(sys.argv[2])
        car = len(sys.argv) < 6 or sys.argv[5] == "car"
        a = [int(round(float(x) * 1e7)) for x in sys.argv[3].split(",")]
        b = [int(round(float(x) * 1e7)) for x in sys.argv[4].split(",")]
        start, goal = g.nearest(a[0], a[1], car), g.nearest(b[0], b[1], car)
        result = route(g, start, goal, car)
        if result is None:
            print("no route")
            return
        cost, length, path, settled = result
        print(f"cost {cost:.0f} length {length} m, {len(path)} edges, {settled} vertices settled")
        last = None
        for v, meta, name in path:
            n = g.name(name)
            if n != last:
                print("  ", n or "(no name)", CLASSES[meta >> 20 & 15])
                last = n
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
