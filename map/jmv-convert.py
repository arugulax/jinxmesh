#!/usr/bin/env python3
"""jmv-convert.py MAP.jmv [MORE.jmv ...] OUT.geojson | OUT.gpx | OUT.kml

Turns JinxMesh vector maps (.jmv, "JMVM" version 4: docs/MAPS.md) into a file other programs read.
Needs Python 3.8 or later and nothing else.

  --layers contours,water,roads,trails,boundaries,names
        What to take. Default: everything for GeoJSON; everything but contour lines for GPX and KML,
        which get very large with them.
  --box WEST,SOUTH,EAST,NORTH
        Only the cells of the map that touch this box, in degrees.
  --info
        Prints what the map files hold and writes nothing.

What comes out is the finest level of the map: what the device draws at zooms 13 to 17. The map keeps its
land in cells about 3 to 5 km wide, and a road that crosses from one cell to the next is two lines in the
map; it is two lines in the output as well, meeting at the edge. Names that the map repeats for several
zooms are written once. Heights are in feet, as in the map.

GeoJSON holds everything, with "kind", "layer" and, where there is one, "name" and "height_ft" for each
feature. GPX has tracks (one per kind of line, a segment per line) and waypoints for cities, towns,
villages and peaks. KML has a folder per kind.
"""
import json, math, os, struct, sys
from xml.sax.saxutils import escape

EXTENT = 4096
KINDS = {
    1: ("contour-20ft", "contours"), 2: ("contour-100ft", "contours"), 3: ("contour-500ft", "contours"),
    10: ("stream", "water"), 11: ("river", "water"), 12: ("water", "water"), 13: ("coast", "water"),
    20: ("motorway", "roads"), 21: ("primary", "roads"), 22: ("secondary", "roads"), 23: ("minor", "roads"),
    24: ("service", "roads"), 25: ("track", "trails"), 26: ("path", "trails"), 27: ("rail", "roads"),
    30: ("boundary", "boundaries"), 31: ("state-line", "boundaries"), 32: ("national-border", "boundaries"),
    64: ("city", "names"), 65: ("town", "names"), 66: ("village", "names"), 67: ("peak", "names"),
    68: ("road-name", "names"), 69: ("water-name", "names"), 70: ("contour-name", "names"),
}
WATER, CONTOUR_NAME = 12, 70
LAYERS = ("contours", "water", "roads", "trails", "boundaries", "names")
COLORS = {  # as the device draws them, for KML
    "contour-20ft": "C2BAA8", "contour-100ft": "9F9888", "contour-500ft": "7B756A", "stream": "2F78BF", "river": "2F78BF",
    "water": "A9CFF0", "coast": "2F78BF", "motorway": "C0392B", "primary": "D35400", "secondary": "1E1E1E", "minor": "444444",
    "service": "6E6E6E", "track": "8D6E3F", "path": "2E7D32", "rail": "111111", "boundary": "8E44AD", "state-line": "8E44AD",
    "national-border": "8E44AD",
}


def fail(text):
    sys.exit("jmv-convert: " + text)


class Reader:
    def __init__(self, data, at=0, end=None):
        self.data, self.at, self.end = data, at, len(data) if end is None else end

    def byte(self):
        self.at += 1
        return self.data[self.at - 1]

    def varint(self):
        value = shift = 0
        while True:
            b = self.data[self.at]
            self.at += 1
            value |= (b & 127) << shift
            shift += 7
            if not b & 128:
                return value

    def zigzag(self):
        v = self.varint()
        return (v >> 1) ^ -(v & 1)


def read_map(path):
    """The header of a map: (file, zooms, levels); a level has zoom, first, last, x0, y0, cols, rows, cells
    {(cx, cy): (offset, bytes of lines, bytes of names)}."""
    f = open(path, "rb")
    head = f.read(28)
    if len(head) < 28 or head[:4] != b"JMVM":
        fail(f"{path} is no JinxMesh vector map" + (": it is the builder's own large form, cut it with split.py first"
                                                     if head[:4] == b"JMVW" else ""))
    version, count = struct.unpack("<HH", head[4:8])
    if version != 4:
        fail(f"{path} is version {version} of the format; this tool reads version 4")
    box = [v / 1e7 for v in struct.unpack("<iiii", head[8:24])]
    levels = []
    for _ in range(count):
        zoom, first, last, _, x0, y0, cols, rows, where = struct.unpack("<BBBBIIHHI", f.read(20))
        levels.append(dict(zoom=zoom, first=first, last=last, x0=x0, y0=y0, cols=cols, rows=rows, where=where))
    for level in levels:
        f.seek(level["where"])
        table = f.read(12 * level["cols"] * level["rows"])
        level["cells"] = {}
        for i in range(level["cols"] * level["rows"]):
            offset, lines, names = struct.unpack_from("<III", table, 12 * i)
            if lines or names:
                level["cells"][(level["x0"] + i % level["cols"], level["y0"] + i // level["cols"])] = (offset, lines, names)
    return f, box, levels


def lonlat(zoom, cx, cy, ux, uy):
    n = 1 << zoom
    x, y = (cx + ux / EXTENT) / n, (cy + uy / EXTENT) / n
    return round(x * 360 - 180, 6), round(math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y)))), 6)


def clip_line(points):
    """A line cut at the edge of its cell (lines reach a little over it): the parts inside."""
    def inside(p):
        return 0 <= p[0] <= EXTENT and 0 <= p[1] <= EXTENT

    def cut(a, b):
        # the part of a-b inside the cell, by Liang-Barsky; None when there is none
        t0, t1, dx, dy = 0.0, 1.0, b[0] - a[0], b[1] - a[1]
        for p, q in ((-dx, a[0]), (dx, EXTENT - a[0]), (-dy, a[1]), (dy, EXTENT - a[1])):
            if p == 0:
                if q < 0:
                    return None
                continue
            t = q / p
            if p < 0:
                if t > t1:
                    return None
                t0 = max(t0, t)
            else:
                if t < t0:
                    return None
                t1 = min(t1, t)
        return (a[0] + t0 * dx, a[1] + t0 * dy), (a[0] + t1 * dx, a[1] + t1 * dy)

    parts, now = [], []
    for a, b in zip(points, points[1:]):
        if inside(a) and inside(b):
            if not now:
                now.append(a)
            now.append(b)
            continue
        piece = cut(a, b)
        if piece is None:
            if len(now) > 1:
                parts.append(now)
            now = []
            continue
        if now and now[-1] == piece[0]:
            now.append(piece[1])
        else:
            if len(now) > 1:
                parts.append(now)
            now = [piece[0], piece[1]]
        if not inside(b):
            parts.append(now)
            now = []
    if len(now) > 1:
        parts.append(now)
    return [p for p in parts if len(p) > 1 and any(q != p[0] for q in p)]


def clip_ring(points):
    """An area cut at the edge of its cell, by Sutherland-Hodgman."""
    for axis, limit, keep_low in ((0, 0, False), (0, EXTENT, True), (1, 0, False), (1, EXTENT, True)):
        out = []
        for i, b in enumerate(points):
            a = points[i - 1]
            a_in = a[axis] <= limit if keep_low else a[axis] >= limit
            b_in = b[axis] <= limit if keep_low else b[axis] >= limit
            if a_in != b_in:
                t = (limit - a[axis]) / (b[axis] - a[axis])
                out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
            if b_in:
                out.append(b)
        points = out
        if len(points) < 3:
            return []
    return points


def features(paths, layers, box):
    """Everything of the finest level of the maps, as (shape, kind, layer, properties, coordinates) with shape
    "line", "area" or "point" and coordinates in degrees (longitude, latitude)."""
    seen_cells, seen_names = set(), set()
    for path in paths:
        f, _, levels = read_map(path)
        level = max(levels, key=lambda l: l["zoom"])
        zoom = level["zoom"]
        for (cx, cy), (offset, lines, names) in sorted(level["cells"].items()):
            if (cx, cy) in seen_cells:
                continue  # the rim of a piece is the same cell as in the piece next to it
            seen_cells.add((cx, cy))
            if box:
                west, north = lonlat(zoom, cx, cy, 0, 0)
                east, south = lonlat(zoom, cx, cy, EXTENT, EXTENT)
                if east < box[0] or west > box[2] or north < box[1] or south > box[3]:
                    continue
            f.seek(offset)
            data = f.read(lines + names)
            r = Reader(data, 0, lines)
            while r.at < lines:
                kind = r.byte()
                height = r.varint() if 1 <= kind <= 3 else 0
                count = r.varint()
                x = y = 0
                points = []
                for _ in range(count):
                    x += r.zigzag()
                    y += r.zigzag()
                    points.append((x, y))
                name, layer = KINDS.get(kind, (f"kind-{kind}", "roads"))
                if layer not in layers:
                    continue
                properties = {"height_ft": height} if 1 <= kind <= 3 else {}
                if kind == WATER:
                    ring = clip_ring(points)
                    if len(ring) >= 3:
                        ring = [lonlat(zoom, cx, cy, *p) for p in ring]
                        yield "area", name, layer, properties, ring + [ring[0]]
                else:
                    for part in clip_line(points):
                        yield "line", name, layer, properties, [lonlat(zoom, cx, cy, *p) for p in part]
            r = Reader(data, lines)
            while r.at < len(data):
                kind = r.byte()
                x, y = r.zigzag(), r.zigzag()
                r.at += 3  # the zooms it shows at, and its turn
                number, length = r.varint(), r.byte()
                text = data[r.at:r.at + length].decode("utf-8", "replace")
                r.at += length
                if "names" not in layers or kind == CONTOUR_NAME or not (0 <= x < EXTENT and 0 <= y < EXTENT):
                    continue
                name = KINDS.get(kind, (f"kind-{kind}",))[0]
                place = lonlat(zoom, cx, cy, x, y)
                key = (kind, text) if kind in (64, 65, 66, 67) else (kind, text, place)
                if key in seen_names and kind in (64, 65, 66, 67):
                    continue  # the same place, there again for other zooms
                if (kind, text, place) in seen_names:
                    continue
                seen_names.add(key)
                seen_names.add((kind, text, place))
                properties = {"name": text}
                if kind == 67 and number:
                    properties["height_ft"] = number
                yield "point", name, "names", properties, place
        f.close()


def write_geojson(out, items):
    out.write('{"type":"FeatureCollection","features":[\n')
    first = True
    for shape, kind, layer, properties, coordinates in items:
        geometry = {"line": {"type": "LineString", "coordinates": coordinates},
                    "area": {"type": "Polygon", "coordinates": [coordinates]},
                    "point": {"type": "Point", "coordinates": coordinates}}[shape]
        feature = {"type": "Feature", "properties": dict(kind=kind, layer=layer, **properties), "geometry": geometry}
        out.write(("" if first else ",\n") + json.dumps(feature, separators=(",", ":"), ensure_ascii=False))
        first = False
    out.write("\n]}\n")


def write_gpx(out, items):
    out.write('<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1" creator="jmv-convert" '
              'xmlns="http://www.topografix.com/GPX/1/1">\n')
    tracks = {}
    for shape, kind, layer, properties, coordinates in items:
        if shape == "point":
            if kind in ("city", "town", "village", "peak"):
                height = f"<ele>{properties['height_ft'] * 0.3048:.0f}</ele>" if "height_ft" in properties else ""
                out.write(f'<wpt lat="{coordinates[1]}" lon="{coordinates[0]}">{height}<name>{escape(properties["name"])}</name>'
                          f"<type>{kind}</type></wpt>\n")
            continue
        tracks.setdefault(kind, []).append(coordinates)  # waypoints come before tracks in a GPX file
    for kind, lines in tracks.items():
        out.write(f"<trk><name>{kind}</name><type>{kind}</type>\n")
        for line in lines:
            out.write("<trkseg>" + "".join(f'<trkpt lat="{lat}" lon="{lon}"/>' for lon, lat in line) + "</trkseg>\n")
        out.write("</trk>\n")
    out.write("</gpx>\n")


def write_kml(out, items):
    out.write('<?xml version="1.0" encoding="UTF-8"?>\n<kml xmlns="http://www.opengis.net/kml/2.2"><Document>\n')
    for kind, color in COLORS.items():
        abgr = "ff" + color[4:6] + color[2:4] + color[0:2]
        out.write(f'<Style id="{kind}"><LineStyle><color>{abgr}</color><width>{3 if kind in ("motorway", "primary") else 2}</width>'
                  f"</LineStyle><PolyStyle><color>{abgr}</color></PolyStyle></Style>\n")
    folders = {}
    for shape, kind, layer, properties, coordinates in items:
        if shape == "point":
            text = f'<Placemark><name>{escape(properties["name"])}</name><Point><coordinates>{coordinates[0]},{coordinates[1]}' \
                   "</coordinates></Point></Placemark>\n"
        else:
            row = " ".join(f"{lon},{lat}" for lon, lat in coordinates)
            label = f"<name>{properties['height_ft']} ft</name>" if "height_ft" in properties else ""
            shape_xml = f"<Polygon><outerBoundaryIs><LinearRing><coordinates>{row}</coordinates></LinearRing></outerBoundaryIs></Polygon>" \
                if shape == "area" else f"<LineString><coordinates>{row}</coordinates></LineString>"
            text = f"<Placemark>{label}<styleUrl>#{kind}</styleUrl>{shape_xml}</Placemark>\n"
        folders.setdefault(kind, []).append(text)
    for kind, rows in folders.items():
        out.write(f"<Folder><name>{kind}</name>\n" + "".join(rows) + "</Folder>\n")
    out.write("</Document></kml>\n")


def info(paths):
    for path in paths:
        f, box, levels = read_map(path)
        print(f"{path}: {os.path.getsize(path):,} bytes, south {box[0]:.5f} west {box[1]:.5f} north {box[2]:.5f} east {box[3]:.5f}")
        for level in levels:
            size = sum(lines + names for _, lines, names in level["cells"].values())
            print(f"  cells of zoom {level['zoom']}, drawn at zooms {level['first']} to {level['last']}: "
                  f"{len(level['cells'])} cells, {size:,} bytes")
        f.close()


def main():
    args, layers, box, only_info = [], None, None, False
    rest = sys.argv[1:]
    while rest:
        a = rest.pop(0)
        if a in ("-h", "--help"):
            print(__doc__.strip())
            return
        if a == "--info":
            only_info = True
        elif a in ("--layers", "--box"):
            if not rest:
                fail(f"{a} needs a value")
            value = rest.pop(0)
            if a == "--layers":
                layers = set(value.split(","))
                if layers - set(LAYERS):
                    fail("no such layer: " + ", ".join(sorted(layers - set(LAYERS))) + "; there are " + ", ".join(LAYERS))
            else:
                try:
                    box = [float(v) for v in value.split(",")]
                except ValueError:
                    box = []
                if len(box) != 4 or box[0] >= box[2] or box[1] >= box[3]:
                    fail("--box takes WEST,SOUTH,EAST,NORTH in degrees")
        elif a.startswith("--"):
            fail(f"no such option: {a}; --help lists them")
        else:
            args.append(a)
    if only_info:
        if not args:
            fail("which map? jmv-convert.py --info MAP.jmv")
        return info(args)
    if len(args) < 2:
        fail("usage: jmv-convert.py MAP.jmv [MORE.jmv ...] OUT.geojson|OUT.gpx|OUT.kml   (--help says more)")
    *paths, target = args
    kind = os.path.splitext(target)[1].lower()
    writer = {".geojson": write_geojson, ".json": write_geojson, ".gpx": write_gpx, ".kml": write_kml}.get(kind)
    if not writer:
        fail(f"cannot tell the format from {target}: name it .geojson, .gpx or .kml")
    for path in paths:
        if not os.path.exists(path):
            fail(f"no such file: {path}")
    if layers is None:
        layers = set(LAYERS) if writer is write_geojson else set(LAYERS) - {"contours"}
    count = [0]

    def counted():
        for item in features(paths, layers, box):
            count[0] += 1
            yield item

    with open(target + ".part", "w", encoding="utf-8") as out:
        writer(out, counted())
    os.replace(target + ".part", target)
    print(f"{target}: {count[0]:,} features, {os.path.getsize(target):,} bytes")
    if not count[0]:
        print("nothing was in there: check --box and --layers")


if __name__ == "__main__":
    main()
