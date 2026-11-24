#!/usr/bin/env python3
"""towns.py: build /geo/towns.bin, the town data of the M9's Town locator (format: docs/MAP-PACKS.md, section 5).

  towns.py --us 2025 --out towns.bin              the whole United States from the Census 1:500,000 file
  towns.py --us 2025 --box -73.5,42.7,-71.4,45.1  only what touches a box (west,south,east,north)
  towns.py --us 2025 --states VT,NH               only some states (postal codes)

The Census file cb_<year>_us_cousub_500k.zip is fetched into --cache unless it is already there.
Needs pyshp. Ends by reading its own output back with the same lookup the device uses and checking known points.
"""
import argparse
import datetime
import math
import os
import struct
import sys
import urllib.request

import shapefile

UNIT = 100000  # ring coordinates: 1e-5 degrees
CELL = 2000000  # grid cell: 0.2 degrees, in 1e-7 degrees
VERSION = 1

# Census LSAD codes: what kind of area a county subdivision is
STATISTICAL = {"22", "23", "24", "27", "28", "29", "30"}  # CCD, census subarea, district, precinct
UNORGANIZED = {"31", "32", "36", "42", "46"}  # gore, grant, location, purchase, unorganized territory
KIND_TOWN, KIND_STATISTICAL, KIND_WATER, KIND_UNORGANIZED = 0, 1, 2, 3


def fetch(year, cache):
    name = f"cb_{year}_us_cousub_500k.zip"
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        url = f"https://www2.census.gov/geo/tiger/GENZ{year}/shp/{name}"
        print("fetching", url, file=sys.stderr)
        urllib.request.urlretrieve(url, path + ".part")
        os.rename(path + ".part", path)
    return path


def thin(points, tolerance):
    """Douglas-Peucker on a closed ring of (x, y) integers; tolerance in the same unit."""
    if len(points) <= 4:
        return points
    # anchor the ring at its two points farthest apart, so that a closed line has two ends to work between
    first = 0
    far = max(range(len(points)), key=lambda i: (points[i][0] - points[0][0]) ** 2 + (points[i][1] - points[0][1]) ** 2)
    keep = [False] * len(points)
    keep[first] = keep[far] = True
    limit = tolerance * tolerance
    stack = [(first, far), (far, len(points))]
    while stack:
        a, b = stack.pop()
        ax, ay = points[a]
        bx, by = points[b % len(points)]
        dx, dy = bx - ax, by - ay
        length = dx * dx + dy * dy
        worst, at = -1.0, -1
        for i in range(a + 1, b):
            px, py = points[i][0] - ax, points[i][1] - ay
            if length:
                t = max(0.0, min(1.0, (px * dx + py * dy) / length))
                ex, ey = px - t * dx, py - t * dy
            else:
                ex, ey = px, py
            d = ex * ex + ey * ey
            if d > worst:
                worst, at = d, i
        if worst > limit:
            keep[at] = True
            stack.append((a, at))
            stack.append((at, b))
    return [p for p, k in zip(points, keep) if k]


def area2(ring):
    return sum(ring[i][0] * ring[(i + 1) % len(ring)][1] - ring[(i + 1) % len(ring)][0] * ring[i][1] for i in range(len(ring)))


def inside(rings, x, y):
    """Even-odd rule over all rings; integers only. The same test the device runs."""
    odd = False
    for ring in rings:
        n = len(ring)
        for i in range(n):
            x1, y1 = ring[i]
            x2, y2 = ring[(i + 1) % n]
            if (y1 > y) != (y2 > y):
                # does the edge cross the line running east from the point?
                side = (x2 - x1) * (y - y1) - (x - x1) * (y2 - y1)
                if (side > 0) == (y2 > y1):
                    odd = not odd
    return odd


def label_point(rings):
    """A point inside the largest ring: its centroid when that is inside, else the middle of the widest stretch
    of the area along the centroid's latitude."""
    big = max(rings, key=lambda r: abs(area2(r)))
    a = area2(big)
    if a:
        cx = sum((big[i][0] + big[(i + 1) % len(big)][0]) * (big[i][0] * big[(i + 1) % len(big)][1] - big[(i + 1) % len(big)][0] * big[i][1])
                 for i in range(len(big))) / (3 * a)
        cy = sum((big[i][1] + big[(i + 1) % len(big)][1]) * (big[i][0] * big[(i + 1) % len(big)][1] - big[(i + 1) % len(big)][0] * big[i][1])
                 for i in range(len(big))) / (3 * a)
    else:
        cx, cy = big[0]
    x, y = int(round(cx)), int(round(cy))
    if inside(rings, x, y):
        return x, y
    crossings = []
    for ring in rings:
        n = len(ring)
        for i in range(n):
            x1, y1 = ring[i]
            x2, y2 = ring[(i + 1) % n]
            if (y1 > y) != (y2 > y):
                crossings.append(x1 + (y - y1) * (x2 - x1) / (y2 - y1))
    crossings.sort()
    best = None
    for i in range(0, len(crossings) - 1, 2):
        if best is None or crossings[i + 1] - crossings[i] > best[1] - best[0]:
            best = (crossings[i], crossings[i + 1])
    if best:
        return int(round((best[0] + best[1]) / 2)), y
    return big[0]


def fold(text):
    """Case folded the way the device does it: ASCII letters only, compared byte by byte."""
    return bytes(b + 32 if 65 <= b <= 90 else b for b in text.encode("utf-8"))


def title(text):
    return " ".join(w[:1].upper() + w[1:] for w in text.split(" "))


def read_us(path, box, states, tolerance):
    reader = shapefile.Reader(path)
    records = []
    for sr in reader.iterShapeRecords():
        d = sr.record.as_dict()
        if states and d["STUSPS"] not in states:
            continue
        w, s, e, n = sr.shape.bbox
        if box and (e < box[0] or w > box[2] or n < box[1] or s > box[3]):
            continue
        parts = list(sr.shape.parts) + [len(sr.shape.points)]
        rings = []
        for a, b in zip(parts, parts[1:]):
            pts = sr.shape.points[a:b]
            # the far Aleutians lie beyond the date line; a box from -180 to 180 would make the grid useless
            if any(p[0] > 0 for p in pts):
                continue
            ring = []
            for lon, lat in pts:
                q = (int(round(lon * UNIT)), int(round(lat * UNIT)))
                if not ring or ring[-1] != q:
                    ring.append(q)
            if len(ring) > 1 and ring[0] == ring[-1]:
                ring.pop()
            ring = thin(ring, tolerance)
            if len(ring) >= 3 and area2(ring):
                rings.append(ring)
        if not rings:
            continue
        lsad = d["LSAD"]
        name = d["NAME"]
        if "not defined" in d["NAMELSAD"] or lsad == "00":
            kind = KIND_WATER if "not defined" in d["NAMELSAD"] else KIND_TOWN
            name = d["NAMELSAD"] if lsad == "00" else "Water or undefined area"
        elif lsad in UNORGANIZED:
            kind = KIND_UNORGANIZED
            name = title(d["NAMELSAD"])  # "Avery's Gore"
        elif lsad in STATISTICAL:
            kind = KIND_STATISTICAL
            if name[:1].isdigit():
                name = d["NAMELSAD"]
        else:
            kind = KIND_TOWN
            if name[:1].isdigit():
                name = d["NAMELSAD"]
        records.append({"name": name, "area": f"{d['NAMELSADCO']}, {d['STUSPS']}", "kind": kind, "rings": rings,
                        "full": title(d["NAMELSAD"])})
    # a city and the town around it often share a name within a county: "Barre City", "Barre Town"
    seen = {}
    for r in records:
        seen.setdefault((r["name"], r["area"]), []).append(r)
    for same in seen.values():
        if len(same) > 1:
            for r in same:
                r["name"] = r["full"]
    return records


def read_geojson(path, name_field, area_field, tolerance):
    import json

    records = []
    for feature in json.load(open(path, encoding="utf-8"))["features"]:
        geometry = feature["geometry"]
        polygons = geometry["coordinates"] if geometry["type"] == "MultiPolygon" else [geometry["coordinates"]]
        rings = []
        for polygon in polygons:
            for pts in polygon:
                ring = []
                for p in pts:
                    q = (int(round(p[0] * UNIT)), int(round(p[1] * UNIT)))
                    if not ring or ring[-1] != q:
                        ring.append(q)
                if len(ring) > 1 and ring[0] == ring[-1]:
                    ring.pop()
                ring = thin(ring, tolerance)
                if len(ring) >= 3 and area2(ring):
                    rings.append(ring)
        if rings:
            properties = feature["properties"]
            records.append({"name": str(properties[name_field]), "area": str(properties.get(area_field, "")) if area_field else "",
                            "kind": KIND_TOWN, "rings": rings})
    return records


def pack_ring(ring):
    """Point count, first point as two int32, then int16 steps; a step too long for int16 is split."""
    points = [ring[0]]
    for x, y in ring[1:]:
        px, py = points[-1]
        pieces = max(1, math.ceil(max(abs(x - px), abs(y - py)) / 32767))
        for k in range(1, pieces + 1):
            points.append((px + (x - px) * k // pieces, py + (y - py) * k // pieces))
    out = [struct.pack("<Iii", len(points), points[0][0], points[0][1])]
    for (px, py), (x, y) in zip(points, points[1:]):
        out.append(struct.pack("<hh", x - px, y - py))
    return b"".join(out), points


def build(records, accuracy, out_path):
    records.sort(key=lambda r: (r["area"].split(", ")[-1], r["area"], r["name"]))
    strings = bytearray()
    offsets = {}

    def string(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode("utf-8") + b"\0")
        return offsets[text]

    rings = bytearray()
    packed = []
    for r in records:
        first = len(rings)
        stored = []
        for ring in r["rings"]:
            data, points = pack_ring(ring)
            rings.extend(data)
            stored.append(points)
        r["rings"] = stored
        xs = [p[0] for ring in stored for p in ring]
        ys = [p[1] for ring in stored for p in ring]
        r["box"] = (min(xs) * 100, min(ys) * 100, max(xs) * 100, max(ys) * 100)  # 1e-7 degrees
        lx, ly = label_point(stored)
        packed.append(struct.pack("<4i2i3IHBB", *r["box"], lx * 100, ly * 100, string(r["name"]), string(r["area"]), first, len(stored),
                                  r["kind"], 0))

    west = min(r["box"][0] for r in records) // CELL * CELL
    south = min(r["box"][1] for r in records) // CELL * CELL
    east = max(r["box"][2] for r in records)
    north = max(r["box"][3] for r in records)
    columns = (east - west) // CELL + 1
    rows = (north - south) // CELL + 1
    assert columns < 65536 and rows < 65536
    cells = [[] for _ in range(columns * rows)]
    for number, r in enumerate(records):
        for row in range((r["box"][1] - south) // CELL, (r["box"][3] - south) // CELL + 1):
            for column in range((r["box"][0] - west) // CELL, (r["box"][2] - west) // CELL + 1):
                cells[row * columns + column].append(number)
    starts, total = [], 0
    for cell in cells:
        starts.append(total)
        total += len(cell)
    starts.append(total)
    grid = struct.pack(f"<{len(starts)}I", *starts) + b"".join(struct.pack(f"<{len(c)}I", *c) for c in cells if c)

    order = sorted(range(len(records)), key=lambda i: (fold(records[i]["name"]), records[i]["area"]))
    names = struct.pack(f"<{len(order)}I", *order)

    grid_at = 64
    records_at = grid_at + len(grid)
    names_at = records_at + 40 * len(records)
    strings_at = names_at + len(names)
    rings_at = strings_at + len(strings)
    length = rings_at + len(rings)
    header = struct.pack("<4sHH4iIHH8I", b"KBTN", VERSION, accuracy, west, south, east, north, CELL, columns, rows, len(records), grid_at,
                         records_at, names_at, strings_at, rings_at, length, int(datetime.date.today().strftime("%Y%m%d")))
    assert len(header) == 64
    with open(out_path, "wb") as f:
        f.write(header + grid + b"".join(packed) + names + bytes(strings) + bytes(rings))
    return length, columns, rows


class Towns:
    """The lookup as the device does it (source/util/TownIndex.cpp is checked against this)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        h = struct.unpack("<4sHH4iIHH8I", self.f.read(64))
        assert h[0] == b"KBTN" and h[1] == VERSION
        (self.accuracy, self.west, self.south, self.east, self.north, self.cell_size, self.columns, self.rows, self.count, self.grid_at,
         self.records_at, self.names_at, self.strings_at, self.rings_at, self.length, self.date) = h[2:]

    def read(self, at, size):
        self.f.seek(at)
        return self.f.read(size)

    def string(self, offset):
        data = self.read(self.strings_at + offset, 96)
        return data[: data.index(b"\0")].decode("utf-8")

    def record(self, number):
        v = struct.unpack("<4i2i3IHBB", self.read(self.records_at + 40 * number, 40))
        return {"box": v[0:4], "label": v[4:6], "name": self.string(v[6]), "area": self.string(v[7]), "ring": v[8], "rings": v[9],
                "kind": v[10]}

    def rings(self, record):
        at = self.rings_at + record["ring"]
        out = []
        for _ in range(record["rings"]):
            count, x, y = struct.unpack("<Iii", self.read(at, 12))
            steps = struct.unpack(f"<{2 * (count - 1)}h", self.read(at + 12, 4 * (count - 1)))
            ring = [(x, y)]
            for i in range(count - 1):
                x += steps[2 * i]
                y += steps[2 * i + 1]
                ring.append((x, y))
            out.append(ring)
            at += 12 + 4 * (count - 1)
        return out

    def cell_list(self, lon, lat):
        column, row = (lon - self.west) // self.cell_size, (lat - self.south) // self.cell_size
        a, b = struct.unpack("<2I", self.read(self.grid_at + 4 * (row * self.columns + column), 8))
        lists = self.grid_at + 4 * (self.columns * self.rows + 1)
        return struct.unpack(f"<{b - a}I", self.read(lists + 4 * a, 4 * (b - a)))

    def find(self, latitude, longitude):
        """Returns (record or None, near): the town of a point and, close to a line, the neighbor's record."""
        lon, lat = int(round(longitude * 1e7)), int(round(latitude * 1e7))
        if not (self.west <= lon <= self.east and self.south <= lat <= self.north):
            return None, None
        x, y = lon // 100, lat // 100
        found, edges = None, []
        scale = math.cos(math.radians(latitude))
        for number in self.cell_list(lon, lat):
            record = self.record(number)
            margin = self.accuracy * 200  # 1e-7 degrees: a record a little off still counts for "near the line"
            if not (record["box"][0] - margin <= lon <= record["box"][2] + margin and record["box"][1] - margin <= lat <= record["box"][3] + margin):
                continue
            rings = self.rings(record)
            is_in = found is None and inside(rings, x, y)
            if is_in:
                found = record
            nearest = float("inf")
            for ring in rings:
                n = len(ring)
                for i in range(n):
                    x1, y1 = ring[i]
                    x2, y2 = ring[(i + 1) % n]
                    ax, ay = (x1 - x) * scale, y1 - y
                    bx, by = (x2 - x) * scale, y2 - y
                    dx, dy = bx - ax, by - ay
                    length = dx * dx + dy * dy
                    t = max(0.0, min(1.0, -(ax * dx + ay * dy) / length)) if length else 0.0
                    ex, ey = ax + t * dx, ay + t * dy
                    nearest = min(nearest, math.hypot(ex, ey))
            edges.append((nearest * 1.11319, record))  # 1e-5 degrees of latitude are 1.11 m
        edges.sort(key=lambda e: e[0])
        if found is None and edges and edges[0][0] <= 2 * self.accuracy:
            found = edges[0][1]  # in a sliver that thinning the lines left between two towns
        near = next((r for d, r in edges if r is not found and d <= self.accuracy), None)
        return found, near

    def search(self, prefix):
        prefix = fold(prefix)
        lo, hi = 0, self.count
        number = lambda i: struct.unpack("<I", self.read(self.names_at + 4 * i, 4))[0]
        while lo < hi:
            mid = (lo + hi) // 2
            if fold(self.record(number(mid))["name"])[: len(prefix)] < prefix:
                lo = mid + 1
            else:
                hi = mid
        out = []
        while lo < self.count and len(out) < 20:
            record = self.record(number(lo))
            if not fold(record["name"]).startswith(prefix):
                break
            out.append(record)
            lo += 1
        return out


CHECKS = [
    (44.2601, -72.5754, "Montpelier", "Washington County, VT"),
    (44.4759, -73.2121, "Burlington", "Chittenden County, VT"),
    (44.4193, -72.0151, "St. Johnsbury", "Caledonia County, VT"),
    (0.0, 0.0, None, None),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", metavar="YEAR", help="the Census national county subdivision file of that year")
    ap.add_argument("--states", help="postal codes, comma separated: only these states")
    ap.add_argument("--geojson", help="any other polygon layer")
    ap.add_argument("--name-field", default="name")
    ap.add_argument("--area-field", default="")
    ap.add_argument("--box", help="west,south,east,north: keep only records that touch it")
    ap.add_argument("--accuracy", type=int, default=150, help="meters: within this of a line the device says 'near the line'")
    ap.add_argument("--thin", type=int, default=30, help="meters the lines may move when they are thinned")
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/m9-tiles/census"))
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    box = [float(v) for v in args.box.split(",")] if args.box else None
    tolerance = args.thin / 1.11319  # meters to 1e-5 degrees of latitude
    records = []
    if args.us:
        records += read_us(fetch(args.us, args.cache), box, set(args.states.split(",")) if args.states else None, tolerance)
    if args.geojson:
        records += read_geojson(args.geojson, args.name_field, args.area_field, tolerance)
    if not records:
        sys.exit("no records")
    points = sum(len(ring) for r in records for ring in r["rings"])
    length, columns, rows = build(records, args.accuracy, args.out)
    print(f"{len(records)} records, {points} points, grid {columns} x {rows}, {length} bytes -> {args.out}")

    towns = Towns(args.out)
    failed = 0
    for latitude, longitude, name, area in CHECKS:
        found, near = towns.find(latitude, longitude)
        got = (found["name"], found["area"]) if found else (None, None)
        covered = towns.west <= longitude * 1e7 <= towns.east and towns.south <= latitude * 1e7 <= towns.north
        relevant = name is None or bool(towns.search(name[:4]))
        ok = got == (name, area) or not relevant or (name and not covered)
        print(f"  {latitude:.4f}, {longitude:.4f}: {got[0]}, {got[1]}" + (f" (near the {near['name']} line)" if near else "") +
              ("" if ok else f"   EXPECTED {name}, {area}"))
        failed += not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
