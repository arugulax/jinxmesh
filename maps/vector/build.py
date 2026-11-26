"""build.py <out.jmv> <font.h> <work folder> (--box W S E N | --state NAME | --poly FILE) [--workers N] [--readers N] [--keep-tiles]
         <osm.pbf>... [--near <osm.pbf>...]

Makes a vector map for the device (format "JMVM", read by device-ui's util/VectorMap): contour lines, water,
roads and trails, rails, town and state lines and names, as point lists in cells, in four levels of detail.
The format is described in device-ui/include/util/VectorMap.h. One file is one state or province or the land
in a box; the device shows all files of a folder side by side. What belongs to a state: --poly, the outline
its extract was cut along (the .poly file next to the extract at download.geofabrik.de; the surest), or
--state, its name as OpenStreetMap has it, whose outline is then taken from the extract.

A state's map reaches a cell over its outline. For those cells along its borders to be whole, the extracts
of its neighbors are given too (--near): what of them lies in the cells of this map is put in, in the two
fine levels. So the same cell comes out the same in the files of both neighbors, and the device draws it
from either; the two coarse levels hold the state alone, and the device lays those of neighbors over each
other.

The land is worked through in blocks, a tile of zoom 10 each (about 39 x 28 km at 44 degrees north), several at
a time: per block the elevation tiles are fetched ("terrarium" tiles of zoom 14 from the public bucket
elevation-tiles-prod; in the United States made from the USGS 3DEP 10 m model), the contour lines made of
them (contours.cpp next to this file) and kept in the work folder, and the two fine levels of the map built.
The contour lines kept are those of whole blocks, so that states that share a block share them as well.
The two coarse levels are made in one piece each. So a state of any size needs no more memory than its roads.

Fetching the tiles and making the contour lines is the slow part and needs none of the map's data, so it runs
in processes of its own (--workers of them), a little ahead of the building of the blocks, however few of
those the memory allows at a time. A build that was stopped goes on where it was: the blocks it had made are
kept in the work folder and taken again by the next run with the same extracts.

The format holds a map of up to 4 GB. A larger one (a province of the far north) is written with 64-bit
offsets instead, as "JMVW": no map for the device, but what split.py reads to cut it into pieces, which are
maps like any other.

Names are settled here, not on the device: for each zoom, which of them have room without running into each
other, the more important first, and how they lie along their road or stream. For that the widths of the
device's letters are read from its font (device-ui/include/util/VectorMapFont.h).
"""
import array, bisect, gc, json, math, multiprocessing, os, pickle, re, shutil, struct, subprocess, sys, time, urllib.request
from concurrent.futures import ThreadPoolExecutor
import osmium
from PIL import Image

FT = 0.3048
EXTENT = 4096  # units across a cell
BUFFER = 96  # units a cell reaches over its edges, so that lines meet without gaps
# level: cell zoom, tile zooms it serves, thinning tolerance in units
LEVELS = [(13, 13, 17, 2.0), (11, 11, 12, 3.0), (9, 9, 10, 4.0), (7, 7, 8, 6.0)]
BLOCK = 10  # zoom of the tiles the work is cut into; holds whole cells of the first two levels
DEM = 14  # zoom of the elevation tiles
# where the elevation tiles come from; M9_DEM_URL names another source of the same kind of tiles, which may be
# a folder (file:///.../{z}/{x}/{y}.png): see docs/MAPS.md
DEM_URL = os.environ.get("M9_DEM_URL", "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png")
INTERVAL = 20 * FT  # between contour lines, meters
WIDE = 0xFFFFFFFF  # a map whose offsets pass this is written as "JMVW", with 64-bit offsets, for split.py

CONTOUR20, CONTOUR100, CONTOUR500 = 1, 2, 3
STREAM, RIVER, WATER, COAST = 10, 11, 12, 13
MOTORWAY, PRIMARY, SECONDARY, MINOR, SERVICE, TRACK, PATH, RAIL = 20, 21, 22, 23, 24, 25, 26, 27
BOUNDARY, STATELINE, NATIONLINE = 30, 31, 32
CITY, TOWN, VILLAGE, PEAK, ROADNAME, WATERNAME, CONTOURNAME = 64, 65, 66, 67, 68, 69, 70
# the order the device draws in, the last on top: the features of a cell are written in it
ORDER = [WATER, CONTOUR20, CONTOUR100, CONTOUR500, STREAM, RIVER, COAST, BOUNDARY, STATELINE, NATIONLINE, PATH, TRACK, SERVICE, RAIL,
         MINOR, SECONDARY, PRIMARY, MOTORWAY]
SPACING = 260  # pixels between two names along one road or stream
SAME = 200  # and at least this far between two that read the same
SPACING_CONTOUR, SAME_CONTOUR = 190, 90  # the same for the heights on contour lines
MARGIN = 3  # pixels kept free around a name

# a name of a line: the zoom from which it may show, and its rank among the names (lower first)
LINE_NAMES = {
    MOTORWAY: (9, 20), PRIMARY: (10, 21), RIVER: (11, 30), SECONDARY: (13, 40), MINOR: (14, 50),
    STREAM: (14, 60), TRACK: (14, 70), PATH: (14, 71),
}
PLACE_NAMES = {CITY: (0, 0), TOWN: (9, 1), VILLAGE: (11, 2), PEAK: (12, 10)}

HIGHWAY = {
    "motorway": MOTORWAY, "motorway_link": MOTORWAY, "trunk": PRIMARY, "trunk_link": PRIMARY,
    "primary": PRIMARY, "primary_link": PRIMARY, "secondary": SECONDARY, "secondary_link": SECONDARY,
    "tertiary": SECONDARY, "tertiary_link": SECONDARY, "unclassified": MINOR, "residential": MINOR,
    "living_street": MINOR, "road": MINOR, "service": SERVICE, "track": TRACK, "path": PATH,
    "footway": PATH, "cycleway": PATH, "bridleway": PATH, "steps": PATH,
}
# what a level keeps
KEEP = [
    None,  # everything
    {CONTOUR500, RIVER, WATER, COAST, MOTORWAY, PRIMARY, SECONDARY, MINOR, RAIL, BOUNDARY, STATELINE, NATIONLINE, CITY, TOWN, VILLAGE, PEAK},
    {RIVER, WATER, COAST, MOTORWAY, PRIMARY, SECONDARY, STATELINE, NATIONLINE, CITY, TOWN},
    {RIVER, WATER, COAST, MOTORWAY, PRIMARY, STATELINE, NATIONLINE, CITY, TOWN},
]
# names of lines a level shows: those ranked before this (see LINE_NAMES)
NAME_RANK = [100, 80.5, 25, 25]
MIN_WATER = [0, 40000, 500000, 4000000]  # square meters a water area needs to be on a level

ENDINGS = {
    "Street": "St", "Road": "Rd", "Avenue": "Ave", "Drive": "Dr", "Lane": "Ln", "Court": "Ct", "Place": "Pl",
    "Terrace": "Ter", "Circle": "Cir", "Highway": "Hwy", "Boulevard": "Blvd", "Mountain": "Mtn", "Extension": "Ext",
}
SIDES = {"North": "N", "South": "S", "East": "E", "West": "W"}


def short(name):
    """A name as it fits a small screen: N Main St, Hunger Mtn, VT 14."""
    name = re.sub(r"\bVermont Route\b", "VT", name)
    name = re.sub(r"\bU\.?S\.? (Route|Highway)\b", "US", name)
    name = re.sub(r"\bInterstate\b", "I", name)
    words = name.split()
    if len(words) >= 2 and words[0] == "Mount":
        words[0] = "Mt"
    # a first word stays whole (Mountain Road, North Street); a side only goes short in front of a name
    words = [words[0]] + [ENDINGS.get(w, w) for w in words[1:]] if words else words
    if len(words) >= 3 and words[0] in SIDES:
        words[0] = SIDES[words[0]]
    return " ".join(words).encode("ascii", "replace")[:32].decode()


def font_widths(header):
    """Per font of the device, the width of each letter from 32 to 126."""
    fonts = {}
    text = open(header).read()
    for name, rows in re.findall(r"const uint16_t (\w+)\[95\]\[\d+\] = \{(.*?)\n\};", text, re.S):
        fonts[name] = [int(w) for w in re.findall(r"^\s*\{(\d+),", rows, re.M)]
        assert len(fonts[name]) == 95, name
    heights = {name: int(h) for name, h in re.findall(r"constexpr uint8_t (\w+)Height = (\d+);", text)}
    return fonts, heights

def merc(lon, lat):
    return (lon + 180.0) / 360.0, (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0


def unmerc(mx, my):
    return mx * 360.0 - 180.0, math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * my))))


def pairs(flat):
    return list(zip(flat[0::2], flat[1::2]))


class Nations(osmium.SimpleHandler):
    """The lines that two countries share. Along a river such a line is often written down as the river alone,
    or as nothing at all, and is a border only by being part of both countries; the line 12 miles out at sea
    is part of one, and is none."""

    def __init__(self):
        super().__init__()
        self.of = {}  # way: the countries it is part of

    def relation(self, r):
        if r.tags.get("boundary") == "administrative" and r.tags.get("admin_level") == "2":
            for m in r.members:
                if m.type == "w":
                    self.of.setdefault(m.ref, set()).add(r.id)


class Source(osmium.SimpleHandler):
    """What the map is made of, from OpenStreetMap. Points are kept as flat rows of numbers (x, y, x, y ... of
    the Web Mercator square, 0 to 1): a state has tens of millions of them."""

    def __init__(self, box, state):
        super().__init__()
        self.box = box  # west, south, east, north; None: all there is
        self.state = state.lower() if state else None
        self.lines, self.areas, self.labels = [], [], []  # (kind, flat), (kind, flat, m2), (kind, mx, my, extra, text)
        self.named = []  # (kind, text, index of the line): what a road or stream is called
        self.outline = []  # the rings of the state
        self.seen = set()  # (what, id): extracts of neighbors hold the same things along their border
        self.nations = Nations()
        # for an extract of a neighbor that is read in a process of its own (read_near): what the others have
        # already, and what each thing here is, so that what two extracts hold is taken once
        self.base = frozenset()
        self.keys = None  # (of the lines, of the areas, of the labels)
        # the blocks the map is made of, when only what lies in or next to them is wanted: of the extract of a
        # neighbor that is most, and a province with all of it does not fit into the memory
        self.blocks = None

    def read(self, pbf):
        self.nations.apply_file(pbf)
        self.apply_file(pbf, locations=True)

    def inside(self, points):
        if not self.box:
            return True
        w, s, e, n = self.box
        if not any(w <= lon <= e and s <= lat <= n for lon, lat in points):
            return False
        if self.blocks is None:
            return True
        lons, lats = [p[0] for p in points], [p[1] for p in points]
        k = 1 << BLOCK
        x0, y0 = merc(min(lons), max(lats))
        x1, y1 = merc(max(lons), min(lats))
        return any((bx, by) in self.blocks for bx in range(int(x0 * k) - 1, int(x1 * k) + 2)
                   for by in range(int(y0 * k) - 1, int(y1 * k) + 2))

    def node(self, n):
        tags = n.tags
        if not n.location.valid():
            return
        name = tags.get("name")
        if not name:
            return
        lon, lat = n.location.lon, n.location.lat
        if not self.inside([(lon, lat)]):
            return
        place = tags.get("place")
        kind = {"city": CITY, "town": TOWN, "village": VILLAGE, "hamlet": VILLAGE}.get(place)
        extra = 0
        if tags.get("natural") == "peak":
            kind = PEAK
            try:
                # the format holds a height as an unsigned number: a peak below the sea (Death Valley) gets none
                extra = max(0, int(round(float(tags.get("ele", "0").split()[0]) / FT)))
            except ValueError:
                extra = 0
        if kind and ("n", n.id) not in self.seen and ("n", n.id) not in self.base:
            self.seen.add(("n", n.id))
            self.labels.append((kind, *merc(lon, lat), extra, short(name) if kind == PEAK else name))
            if self.keys:
                self.keys[2].append(("n", n.id))

    def way(self, w):
        tags = w.tags
        kind = None
        if "highway" in tags:
            kind = HIGHWAY.get(tags["highway"])
            # the walks along streets and across them are no trails
            if tags.get("footway") in ("sidewalk", "crossing", "traffic_island") or tags.get("cycleway") == "crossing":
                kind = None
            if tags.get("access") == "private" and kind in (SERVICE, TRACK, PATH):
                kind = None
        elif tags.get("waterway") in ("river", "canal"):
            kind = RIVER
        elif tags.get("waterway") in ("stream",):
            kind = STREAM
        elif tags.get("natural") == "coastline":
            kind = COAST
        elif tags.get("railway") == "rail" and "service" not in tags:
            kind = RAIL
        elif tags.get("boundary") == "administrative" and tags.get("admin_level") in ("6", "8"):
            kind = BOUNDARY
        elif tags.get("boundary") == "administrative" and tags.get("admin_level") == "4":
            kind = STATELINE
        # the device also warns of the border of a country when one comes near, so none may be missed: a
        # river or a road that is the border is both
        nation = ((tags.get("boundary") == "administrative" and tags.get("admin_level") == "2") or
                  len(self.nations.of.get(w.id, ())) >= 2)
        if kind is None and nation:
            kind, nation = NATIONLINE, False
        if kind is None or ("w", w.id) in self.seen or ("w", w.id) in self.base:
            return
        try:
            points = [(n.lon, n.lat) for n in w.nodes]
        except osmium.InvalidLocationError:
            return
        if len(points) >= 2 and self.inside(points):
            self.seen.add(("w", w.id))
            flat = array.array("d")
            for p in points:
                flat.extend(merc(*p))
            self.lines.append((kind, flat))
            if self.keys:
                self.keys[0].append(("w", w.id))
            if kind in LINE_NAMES:
                # the big roads by their number, which is what signs and people use; the others by name
                number = tags.get("ref", "").split(";")[0].strip()
                name = tags.get("name", "")
                if kind in (MOTORWAY, PRIMARY) and number:
                    self.named.append((kind, number, len(self.lines) - 1))
                elif name:
                    self.named.append((kind, short(name), len(self.lines) - 1))
                elif number and kind == SECONDARY:
                    self.named.append((kind, number, len(self.lines) - 1))
            if nation:
                self.lines.append((NATIONLINE, flat))
                if self.keys:
                    self.keys[0].append(("w", w.id))

    def area(self, a):
        tags = a.tags
        state = (self.state and tags.get("boundary") == "administrative" and tags.get("admin_level") == "4" and
                 tags.get("name", "").lower() == self.state)
        water = tags.get("natural") == "water" or tags.get("waterway") == "riverbank" or tags.get("landuse") == "reservoir"
        if not (state or water):
            return
        if water:
            if ("a", a.id) in self.seen or ("a", a.id) in self.base:
                return
            self.seen.add(("a", a.id))
        try:
            for outer in a.outer_rings():
                points = [(n.lon, n.lat) for n in outer]
                if len(points) < 4:
                    continue
                flat = array.array("d")
                for p in points:
                    flat.extend(merc(*p))
                if state:
                    self.outline.append(flat)
                if water and self.inside(points):
                    lat = points[0][1]
                    k = 111320.0
                    m2 = abs(sum((points[i][0] - points[i - 1][0]) * (points[i][1] + points[i - 1][1]) for i in range(len(points)))) / 2
                    m2 *= k * k * math.cos(math.radians(lat))
                    self.areas.append((WATER, flat, m2))
                    if self.keys:
                        self.keys[1].append(("a", a.id))
                    if tags.get("name") and m2 >= 40000:
                        self.labels.append((WATERNAME, sum(flat[0::2]) / len(points), sum(flat[1::2]) / len(points), 0, short(tags["name"])))
                        if self.keys:
                            self.keys[2].append(("a", a.id))
        except osmium.InvalidLocationError:
            return


def joined(named, lines):
    """Pieces of one road or stream put end to end: maps cut them at every crossing. [(kind, text, flat)]"""
    groups = {}
    for kind, text, index in named:
        groups.setdefault((kind, text), []).append(lines[index][1])
    out = []
    for (kind, text), pieces in groups.items():
        ends = {}
        for i, piece in enumerate(pieces):
            ends.setdefault((piece[0], piece[1]), []).append(i)
            ends.setdefault((piece[-2], piece[-1]), []).append(i)
        used = [False] * len(pieces)
        for i in range(len(pieces)):
            if used[i]:
                continue
            used[i] = True
            chain = pairs(pieces[i])
            for front in (False, True):
                while True:
                    tip = chain[0] if front else chain[-1]
                    more = [j for j in ends.get(tip, []) if not used[j]]
                    if not more:
                        break
                    j = more[0]
                    used[j] = True
                    piece = pairs(pieces[j])
                    if (piece[0] == tip) == front:
                        piece.reverse()
                    chain = piece[:-1] + chain if front else chain + piece[1:]
            flat = array.array("d")
            for p in chain:
                flat.extend(p)
            out.append((kind, text, flat))
    return out


class Line:
    """A road, stream or contour line that names are laid along."""

    def __init__(self, points):
        self.points = points
        self.at = [0.0]  # the length up to each point
        for i in range(len(points) - 1):
            self.at.append(self.at[-1] + math.hypot(points[i + 1][0] - points[i][0], points[i + 1][1] - points[i][1]))
        self.total = self.at[-1]

    def point(self, length):
        i = min(max(bisect.bisect_right(self.at, length) - 1, 0), len(self.points) - 2)
        span = self.at[i + 1] - self.at[i]
        t = (length - self.at[i]) / span if span > 0 else 0
        return (self.points[i][0] + (self.points[i + 1][0] - self.points[i][0]) * t,
                self.points[i][1] + (self.points[i + 1][1] - self.points[i][1]) * t)

    def lay(self, fraction, width, bend):
        """Where a name of that width lies when its middle is that far along the line: (mx, my, angle), or
        None when the line is too short there or bends more than that under it. Lengths in map units; the
        angle in 256ths of a turn, clockwise, such that the name reads from the left."""
        middle = self.total * fraction
        if middle - width / 2 < 0 or middle + width / 2 > self.total:
            # a short street still gets its name, reaching over both its ends, when it is the only one on it
            if fraction != 0.5 or self.total < width * 0.55:
                return None
            width = self.total
        a, b = self.point(middle - width / 2), self.point(middle + width / 2)
        dx, dy = b[0] - a[0], b[1] - a[1]
        chord = math.hypot(dx, dy)
        if chord < width * 0.9:
            return None
        first, last = bisect.bisect_right(self.at, middle - width / 2), bisect.bisect_left(self.at, middle + width / 2)
        worst = max((abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / chord for p in self.points[first:last]), default=0.0)
        if worst > bend:
            return None
        angle = math.atan2(dy, dx)
        if angle > math.pi / 2:
            angle -= math.pi
        elif angle <= -math.pi / 2:
            angle += math.pi
        return ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, max(-64, min(64, int(round(angle / math.pi * 128)))))


def candidates(labels, lines, contours, inside):
    """All names that could be shown, the most important first:
    (rank, first zoom, kind, mx, my, number, text, line, fraction, several, share).

    labels: (kind, mx, my, number, text); lines: (kind, text, points); contours: (kind, points, feet);
    inside(mx, my): whether a place is in the part of the land that is being worked on.

    Those on a line carry it and how far along it they are: each line has one in its middle, then one in
    the middle of each half (several; share: the part of the line each of them has), and so on. Where
    exactly they lie, and how turned, is settled per zoom.
    """
    out = []
    for kind, mx, my, extra, text in labels:
        if not inside(mx, my):
            continue
        if kind == WATERNAME:
            out.append((35, 12, kind, mx, my, 0, text, None, 0, False, 0))
        else:
            first, rank = PLACE_NAMES[kind]
            # the higher of two tops first
            out.append((rank - min(extra, 9999) / 10000.0 if kind == PEAK else rank, first, kind, mx, my, extra, text, None, 0, False, 0))

    def spread(points, rank, first, label, number, text, depths):
        line = Line(points)
        if line.total <= 0:
            return
        for depth in range(depths):
            parts = 1 << depth
            for i in range(parts):
                fraction = (i + 0.5) / parts
                mx, my = line.point(line.total * fraction)
                if inside(mx, my):
                    out.append((rank + depth * 0.01, first, label, mx, my, number, text, line, fraction, depth > 0, 1.0 / parts))

    for kind, text, points in lines:
        first, rank = LINE_NAMES[kind]
        spread(points, rank, first, WATERNAME if kind in (STREAM, RIVER) else ROADNAME, 0, text, 7)
    for kind, points, feet in contours:
        if kind == CONTOUR500:
            spread(points, 80, 12, CONTOURNAME, feet, str(feet), 6)
        elif kind == CONTOUR100:
            spread(points, 81, 14, CONTOURNAME, feet, str(feet), 6)
    out.sort(key=lambda c: (c[0], c[3], c[4]))
    return out


def apart(a, b):
    """Whether two turned boxes (middle x, y, half width, half height, cosine, sine) keep clear of each other."""
    for one, other in ((a, b), (b, a)):
        cx, cy, hw, hh, co, si = one
        ox, oy, ow, oh, oc, os_ = other
        dx, dy = ox - cx, oy - cy
        for ax, ay, half in ((co, si, hw), (-si, co, hh)):
            reach = ow * abs(oc * ax + os_ * ay) + oh * abs(-os_ * ax + oc * ay)
            if abs(dx * ax + dy * ay) > half + reach:
                return True
    return False


def settle(names, zooms, large_from, fonts, heights):
    """Which names show at which zooms, and how they lie: [(kind, mx, my, first zoom, last zoom, angle, number, text)].

    Zoom by zoom, the names that may show there are put down in the order of their rank, each only where
    it runs into none that is there already. What is down stays down: zooming in only makes more room. A
    name on a line is laid anew at each zoom, as the stretch of the line under it gets shorter.
    """
    def size(kind, text, z):
        font = "large" if kind in (CITY, TOWN) and z >= large_from else "small"
        if kind == PEAK:
            text += " 9999'"
        width = sum(fonts[font][ord(c) - 32] if 32 <= ord(c) < 127 else fonts[font][31] for c in text)
        return width, heights[font] + (8 if kind == PEAK else 0)

    placed = []  # indexes into names
    since = {}
    out = []
    open_ = {}  # index of a name: its entry of out that is still running
    zooms = list(zooms)
    for z in zooms:
        world = 256 << z
        grid, boxes = {}, {}

        def lie(i):
            """Middle (pixels), angle and box of a name at this zoom; None when its line has no room for it."""
            _, _, kind, mx, my, _, text, line, fraction, _, _ = names[i]
            wd, ht = size(kind, text, z)
            angle = 0
            if line:
                where = line.lay(fraction, (wd + 4) / world, (3.0 if kind == CONTOURNAME else 3.5) / world)
                if not where:
                    return None
                mx, my, angle = where
            x, y = mx * world, my * world
            turn = angle * math.pi / 128
            middle_y = y - 4 + ht / 2 if kind == PEAK else y
            return x, y, angle, (x, middle_y, wd / 2 + MARGIN, ht / 2 + MARGIN, math.cos(turn), math.sin(turn))

        def cells_of(b):
            reach = b[2] + b[3]
            return [(cx, cy) for cx in range(int((b[0] - reach) // 128), int((b[0] + reach) // 128) + 1)
                    for cy in range(int((b[1] - reach) // 128), int((b[1] + reach) // 128) + 1)]

        def put(i, lying):
            x, y, angle, box = lying
            boxes[i] = box
            for c in cells_of(box):
                grid.setdefault(c, []).append(i)
            # one entry as long as the name lies about the same
            entry = open_.get(i)
            if entry is not None:
                e = out[entry]
                ex, ey = e[1] * world, e[2] * world
                if abs(e[5] - angle) <= 4 and math.hypot(ex - x, ey - y) <= 1.5:
                    e[4] = z
                    return
            open_[i] = len(out)
            out.append([names[i][2], x / world, y / world, z, z, angle, names[i][5], names[i][6]])

        same = {}
        for i in placed:
            lying = lie(i)
            if lying is None:
                # stays as it lay: its entry simply runs on
                e = out[open_[i]]
                turn = e[5] * math.pi / 128
                wd, ht = size(names[i][2], names[i][6], z)
                lying = (e[1] * world, e[2] * world, e[5], (e[1] * world, e[2] * world, wd / 2 + MARGIN, ht / 2 + MARGIN, math.cos(turn), math.sin(turn)))
            put(i, lying)
            same.setdefault(names[i][6], []).append(lying[:2])
        for i, (rank, first, kind, mx, my, number, text, line, fraction, several, share) in enumerate(names):
            if i in since or z < first:
                continue
            lying = lie(i)
            if lying is None:
                continue
            x, y, angle, box = lying
            if several:
                # as many names along the line as have their distance from each other
                step = line.total * world * share
                if step < (SPACING_CONTOUR if kind == CONTOURNAME else SPACING):
                    continue
            if any(not apart(box, boxes[j]) for c in cells_of(box) for j in grid.get(c, [])):
                continue
            if line and any(math.hypot(ox - x, oy - y) < (SAME_CONTOUR if kind == CONTOURNAME else SAME) for ox, oy in same.get(text, [])):
                continue
            since[i] = z
            placed.append(i)
            same.setdefault(text, []).append((x, y))
            put(i, lying)
    last = zooms[-1]
    return [(k, mx, my, a, 255 if b == last else b, angle, number, text) for k, mx, my, a, b, angle, number, text in out]


def thin(points, tolerance):
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    todo = [(0, len(points) - 1)]
    while todo:
        a, b = todo.pop()
        if b <= a + 1:
            continue
        ax, ay = points[a]
        dx, dy = points[b][0] - ax, points[b][1] - ay
        length = math.hypot(dx, dy)
        worst, at = 0.0, a
        for i in range(a + 1, b):
            ex, ey = points[i][0] - ax, points[i][1] - ay
            d = abs(ex * dy - ey * dx) / length if length > 0 else math.hypot(ex, ey)
            if d > worst:
                worst, at = d, i
        if worst > tolerance:
            keep[at] = True
            todo.append((a, at))
            todo.append((at, b))
    return [p for p, k in zip(points, keep) if k]


def clip_line(points, lo, hi):
    """The pieces of a line inside the square lo..hi (both axes)."""
    pieces, piece = [], []
    for i in range(len(points) - 1):
        x0, y0 = points[i]
        x1, y1 = points[i + 1]
        t0, t1 = 0.0, 1.0
        dx, dy = x1 - x0, y1 - y0
        ok = True
        for p, q in ((-dx, x0 - lo), (dx, hi - x0), (-dy, y0 - lo), (dy, hi - y0)):
            if p == 0:
                if q < 0:
                    ok = False
                    break
            else:
                r = q / p
                if p < 0:
                    if r > t1:
                        ok = False
                        break
                    t0 = max(t0, r)
                else:
                    if r < t0:
                        ok = False
                        break
                    t1 = min(t1, r)
        if not ok:
            if piece:
                pieces.append(piece)
                piece = []
            continue
        a = (x0 + t0 * dx, y0 + t0 * dy)
        b = (x0 + t1 * dx, y0 + t1 * dy)
        if not piece or t0 > 0:
            if piece:
                pieces.append(piece)
            piece = [a]
        piece.append(b)
        if t1 < 1:
            pieces.append(piece)
            piece = []
    if piece:
        pieces.append(piece)
    return [p for p in pieces if len(p) >= 2]


def clip_ring(points, lo, hi):
    """A closed ring cut to the square (Sutherland-Hodgman)."""
    def cut(ring, inside, cross):
        out = []
        for i in range(len(ring)):
            a, b = ring[i - 1], ring[i]
            if inside(b):
                if not inside(a):
                    out.append(cross(a, b))
                out.append(b)
            elif inside(a):
                out.append(cross(a, b))
        return out

    def xcross(v):
        return lambda a, b: (v, a[1] + (b[1] - a[1]) * (v - a[0]) / (b[0] - a[0]))

    def ycross(v):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (v - a[1]) / (b[1] - a[1]), v)

    ring = points[:-1] if points[0] == points[-1] else points[:]
    for inside, cross in ((lambda p: p[0] >= lo, xcross(lo)), (lambda p: p[0] <= hi, xcross(hi)),
                          (lambda p: p[1] >= lo, ycross(lo)), (lambda p: p[1] <= hi, ycross(hi))):
        ring = cut(ring, inside, cross)
        if len(ring) < 3:
            return []
    return ring


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append(v & 0x7F | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def zigzag(v):
    return varint((v << 1) ^ (v >> 63))


def encode_points(points):
    out = bytearray(varint(len(points)))
    px = py = 0
    for x, y in points:
        out += zigzag(x - px) + zigzag(y - py)
        px, py = x, y
    return bytes(out)


G = {}  # what the workers share: read before they are started, so that they get it without a copy


def log(*text):
    print(time.strftime("%H:%M:%S"), *text, flush=True)


def dilate(cells):
    return {(x + dx, y + dy) for x, y in cells for dx in (-1, 0, 1) for dy in (-1, 0, 1)}


def memory():
    """(bytes this process holds, bytes of memory that are free to take)."""
    held = room = 0
    for line in open("/proc/self/status"):
        if line.startswith(("VmRSS:", "VmSwap:")):
            held += int(line.split()[1]) * 1024
    for line in open("/proc/meminfo"):
        if line.startswith("MemAvailable:"):
            room = int(line.split()[1]) * 1024
    return max(held, 1), room


def read_near(pbf):
    """In a process of its own: what the extract of a neighbor holds in the cells of the map, but for what the
    state's own extract gave already. Put into a file, whose name is returned."""
    own = G["source"]
    part = Source(own.box, None)
    part.base = own.seen  # only looked at: the pages of it stay shared
    part.blocks = own.blocks
    part.nations = own.nations
    part.keys = ([], [], [])
    part.read(pbf)
    path = f"{G['work']}/tmp/near-{os.getpid()}-{os.path.basename(pbf)}.pickle"
    with open(path, "wb") as f:
        pickle.dump((part.lines, part.areas, part.labels, part.named, part.keys), f, protocol=pickle.HIGHEST_PROTOCOL)
    return path


def take_near(source, path):
    """Adds what read_near found, as reading the extract in its turn would have: what an earlier neighbor
    gave is left out."""
    with open(path, "rb") as f:
        lines, areas, labels, named, keys = pickle.load(f)
    os.remove(path)
    twice = {k for group in keys for k in group if k in source.seen}
    moved = {}
    for i, (line, key) in enumerate(zip(lines, keys[0])):
        if key not in twice:
            moved[i] = len(source.lines)
            source.lines.append(line)
    source.named.extend((kind, text, moved[i]) for kind, text, i in named if i in moved)
    source.areas.extend(a for a, key in zip(areas, keys[1]) if key not in twice)
    source.labels.extend(l for l, key in zip(labels, keys[2]) if key not in twice)
    for group in keys:
        source.seen.update(group)


def read_poly(path):
    """The rings of an outline in the "poly" form: a name, then per ring a name, its points (longitude,
    latitude) and END, then END. Rings named with a ! in front are holes; they are left filled."""
    rings, ring, hole = [], None, False
    for line in open(path).read().splitlines()[1:]:
        words = line.split()
        if not words:
            continue
        if words[0] == "END":
            if ring is not None and len(ring) >= 6 and not hole:
                rings.append(ring)
            ring = None
        elif ring is None:
            ring, hole = array.array("d"), words[0].startswith("!")
        elif len(words) >= 2:
            ring.extend(merc(float(words[0]), float(words[1])))
    return rings


def make_mask(source, box):
    """The cells of the finest level that belong to the map: those of the state and one more around it, or
    those of the box."""
    n = 1 << LEVELS[0][0]
    cells = set()
    if box:
        w, s, e, north = box
        (x0, y0), (x1, y1) = merc(w, north), merc(e, s)
        return {(cx, cy) for cx in range(int(x0 * n), int(x1 * n) + 1) for cy in range(int(y0 * n), int(y1 * n) + 1)}
    if not source.outline:
        log("no outline of the state in the data: the map covers the cells that have roads or water")
        for kind, flat in source.lines:
            for i in range(0, len(flat), 2):
                cells.add((int(flat[i] * n), int(flat[i + 1] * n)))
        return dilate(dilate(cells))
    return dilate(ring_cells(source.outline, n))


def ring_cells(rings, n):
    """The cells, n across the world, that rings (flat rows of points) cover or touch."""
    cells = set()
    rows = {}
    for flat in rings:
        ring = [(x * n, y * n) for x, y in pairs(flat)]
        for i in range(len(ring)):
            a, b = ring[i - 1], ring[i]
            cells.add((int(b[0]), int(b[1])))
            if a[1] == b[1]:
                continue
            low, high = min(a[1], b[1]), max(a[1], b[1])
            # the rows whose middle the edge crosses
            for cy in range(math.ceil(low - 0.5), math.floor(high - 0.5) + 1):
                middle = cy + 0.5
                if low <= middle < high:
                    rows.setdefault(cy, []).append(a[0] + (middle - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
    for cy, crossings in rows.items():
        crossings.sort()
        for i in range(0, len(crossings) - 1, 2):
            for cx in range(math.ceil(crossings[i] - 0.5), math.floor(crossings[i + 1] - 0.5) + 1):
                cells.add((cx, cy))
    return cells


def place(acc, level, kind, points, head=b"", polygon=False, only=None, thinned=False):
    """Puts a line or an area into the cells of a level it runs through: acc[cell][kind] grows by it."""
    cell_zoom, _, _, tolerance = LEVELS[level]
    units = 1 << (cell_zoom + 12)
    pts = [(x * units, y * units) for x, y in points]
    if not thinned:
        pts = thin(pts, tolerance)
    mask = G["masks"][level]
    # a long line in stretches: each then only meets the few cells it runs through
    chunks = [pts] if polygon or len(pts) <= 64 else [pts[i:i + 64] for i in range(0, len(pts) - 1, 63)]
    count = 0
    for chunk in chunks:
        xs, ys = [p[0] for p in chunk], [p[1] for p in chunk]
        for cy in range(int((min(ys) - BUFFER) // EXTENT), int((max(ys) + BUFFER) // EXTENT) + 1):
            for cx in range(int((min(xs) - BUFFER) // EXTENT), int((max(xs) + BUFFER) // EXTENT) + 1):
                if (cx, cy) not in mask or (only is not None and (cx, cy) not in only):
                    continue
                local = [(x - cx * EXTENT, y - cy * EXTENT) for x, y in chunk]
                pieces = [clip_ring(local, -BUFFER, EXTENT + BUFFER)] if polygon else clip_line(local, -BUFFER, EXTENT + BUFFER)
                for piece in pieces:
                    ints = []
                    for x, y in piece:
                        p = (int(round(x)), int(round(y)))
                        if not ints or ints[-1] != p:
                            ints.append(p)
                    if len(ints) < (3 if polygon else 2):
                        continue
                    acc.setdefault((cx, cy), {}).setdefault(kind, bytearray()).extend(bytes([kind]) + head + encode_points(ints))
                    count += len(ints)
    return count


def place_names(names_acc, level, shown, only=None):
    cell_zoom = LEVELS[level][0]
    units = 1 << (cell_zoom + 12)
    mask = G["masks"][level]
    for kind, mx, my, zoom, until, angle, extra, text in shown:
        ux, uy = mx * units, my * units
        cell = (int(ux // EXTENT), int(uy // EXTENT))
        if cell not in mask or (only is not None and cell not in only):
            continue
        raw = b"" if kind == CONTOURNAME else text.encode("ascii", "replace")[:32]
        names_acc.setdefault(cell, bytearray()).extend(
            bytes([kind]) + zigzag(int(ux - cell[0] * EXTENT)) + zigzag(int(uy - cell[1] * EXTENT)) +
            bytes([zoom, until, angle & 0xFF]) + varint(extra) + bytes([len(raw)]) + raw)


def wanted_names(names, level):
    keep = KEEP[level]
    return [c for c in names if keep is None or c[2] in keep or
            (c[2] in (ROADNAME, WATERNAME, CONTOURNAME) and c[0] < NAME_RANK[level])]


def write_cells(out, index, level, acc, names_acc):
    for cell in sorted(set(acc) | set(names_acc)):
        kinds = acc.get(cell, {})
        lines = b"".join(bytes(kinds[k]) for k in ORDER if k in kinds)
        names = bytes(names_acc.get(cell, b""))
        index.append((level, cell[0], cell[1], out.tell(), len(lines), len(names)))
        out.write(lines)
        out.write(names)


def fetch_tile(job):
    x, y, path = job
    if os.path.exists(path):
        return
    last = None
    for attempt in range(12):  # about ten minutes in all: the server has slow spells
        try:
            request = urllib.request.Request(DEM_URL.format(z=DEM, x=x, y=y), headers={"User-Agent": "map-tools"})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
            break
        except urllib.error.HTTPError as error:
            if error.code in (403, 404):
                data = b""  # no such tile: left empty
                break
            last = error
        except Exception as error:
            if DEM_URL.startswith("file:") and isinstance(getattr(error, "reason", error), FileNotFoundError):
                data = b""  # a folder of tiles that has none here
                break
            last = error
        time.sleep(min(60, 2 + attempt * 6))
    else:
        raise RuntimeError(f"cannot fetch elevation tile {x}/{y}: {last}")
    with open(f"{path}.{os.getpid()}.part", "wb") as f:
        f.write(data)
    os.replace(f"{path}.{os.getpid()}.part", path)


def whole_contours(path):
    """Whether a file of contour lines is whole: one that was cut short (a full disk, a build that was
    stopped) is made again, and does not end the build of a province hours later."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        at = 0
        while at < size:
            head = f.read(8)
            if len(head) < 8:
                return False
            at += 8 + struct.unpack("<fI", head)[1] * 8
            if at > size:
                return False
            f.seek(at)
    return True


def block_contours(bx, by):
    """The contour lines of a block and a tile around it, made once and kept. The file of them, and the
    elevation tile its grid starts at."""
    work = G["work"]
    per = 1 << (DEM - BLOCK)
    x0, y0 = bx * per - 1, by * per - 1
    path = f"{work}/contours/{BLOCK}-{bx}-{by}.bin"
    if os.path.exists(path) and whole_contours(path):
        return path, x0, y0
    side = per + 2
    waited = False
    while shutil.disk_usage(work).free < 1.5e9:  # rather wait than fill the disk for everything else on it
        if not waited:
            print(f"{time.strftime('%H:%M:%S')} less than 1.5 GB free in {work}: waiting", flush=True)
            waited = True
        time.sleep(60)
    jobs = [(x, y, f"{work}/tiles/{x}-{y}.png") for y in range(y0, y0 + side) for x in range(x0, x0 + side)]
    with ThreadPoolExecutor(6) as pool:
        list(pool.map(fetch_tile, jobs))
    # one picture of all tiles; where there is none it stays black, which the contour maker takes as no land
    grid = Image.new("RGB", (side * 256, side * 256))
    for x, y, tile in jobs:
        if os.path.getsize(tile):
            grid.paste(Image.open(tile).convert("RGB"), ((x - x0) * 256, (y - y0) * 256))
    raw = f"{work}/tmp/{bx}-{by}.rgb"
    with open(raw, "wb") as f:
        f.write(grid.tobytes())
    del grid
    subprocess.run([G["contours"], raw, str(side * 256), str(side * 256), str(INTERVAL), "0.35", path + ".part"], check=True,
                   stderr=subprocess.DEVNULL)
    os.replace(path + ".part", path)
    os.remove(raw)
    if not G.get("keep_tiles"):
        # the tiles no other block reads (all but the two outer rings of what was fetched) are of no use any
        # more: a province holds 100 GB of them
        for x, y, tile in jobs:
            if x0 + 2 <= x < x0 + side - 2 and y0 + 2 <= y < y0 + side - 2:
                try:
                    os.remove(tile)
                except OSError:
                    pass
    return path, x0, y0


def read_contours(path, x0, y0):
    """The contour lines of a file, cut to the cells of the map: (kind, feet, points)."""
    scale = 1.0 / (256 << DEM)
    n = 1 << LEVELS[0][0]
    mask = G["masks"][0]
    with open(path, "rb") as f:
        while True:
            head = f.read(8)
            if len(head) < 8:
                return
            elevation, count = struct.unpack("<fI", head)
            xy = struct.unpack(f"<{count * 2}f", f.read(count * 8))
            feet = int(round(elevation / FT))
            if feet <= 0:
                continue
            kind = CONTOUR500 if feet % 500 == 0 else CONTOUR100 if feet % 100 == 0 else CONTOUR20
            piece = []
            for i in range(0, count * 2, 2):
                mx, my = (x0 * 256 + xy[i]) * scale, (y0 * 256 + xy[i + 1]) * scale
                if (int(mx * n), int(my * n)) in mask:
                    piece.append((mx, my))
                else:
                    if len(piece) >= 2:
                        yield kind, feet, piece
                    piece = []
            if len(piece) >= 2:
                yield kind, feet, piece


def kept_block(out_path):
    """What an earlier run of the same map left of a block: (index, counts), or nothing."""
    try:
        with open(out_path + ".done", "rb") as f:
            run, size, index, counts = pickle.load(f)
        if run == G["run"] and os.path.getsize(out_path) == size:
            return index, counts
    except Exception:
        pass
    return None


def make_contours(block):
    """In a process of the contour pool: the contour lines of a block, unless the block is made already.
    Not more than a few blocks ahead of the building, which gives the place back (G["ahead"])."""
    G["ahead"].acquire()
    if not kept_block(f"{G['work']}/tmp/block-{block[0]}-{block[1]}.cells"):
        block_contours(*block)
    return block


def drop_tiles(before):
    """Removes the elevation tiles west of a column of tiles: the blocks are done from west to east, and
    without this a province of the north leaves more tiles behind than the disk holds."""
    folder = f"{G['work']}/tiles"
    for name in os.listdir(folder):
        if name.endswith(".png"):
            try:
                if int(name.split("-")[0]) < before:
                    os.remove(f"{folder}/{name}")
            except (ValueError, OSError):
                pass


def do_block(block):
    """The two fine levels of one block, into a file of its own. What is in it: (level, cell x, y, offset,
    bytes of lines, bytes of names) per cell."""
    bx, by = block
    started = time.time()
    out_path = f"{G['work']}/tmp/block-{bx}-{by}.cells"
    kept = kept_block(out_path)
    if kept:
        return out_path, kept[0], kept[1], 0.0, 0.0
    shift0, shift1 = LEVELS[0][0] - BLOCK, LEVELS[1][0] - BLOCK
    only0 = {(cx, cy) for cx in range(bx << shift0, (bx + 1) << shift0) for cy in range(by << shift0, (by + 1) << shift0)}
    only1 = {(cx, cy) for cx in range(bx << shift1, (bx + 1) << shift1) for cy in range(by << shift1, (by + 1) << shift1)}
    blocks = 1 << BLOCK

    def inside(mx, my):
        return bx <= mx * blocks < bx + 1 and by <= my * blocks < by + 1

    def to_block(points):
        """The pieces of a line that lie in the block."""
        local = [(x * blocks - bx, y * blocks - by) for x, y in points]
        return [[((x + bx) / blocks, (y + by) / blocks) for x, y in piece] for piece in clip_line(local, 0.0, 1.0)]

    acc = [{}, {}]
    names_acc = [{}, {}]
    counts = {}
    path, x0, y0 = block_contours(bx, by)
    made = time.time()
    to_name = []
    for kind, feet, points in read_contours(path, x0, y0):
        head = varint(feet)
        counts[kind] = counts.get(kind, 0) + place(acc[0], 0, kind, points, head, only=only0, thinned=True)
        if kind == CONTOUR500:
            place(acc[1], 1, kind, points, head, only=only1)
        if kind != CONTOUR20:
            to_name.extend((kind, piece, feet) for piece in to_block(points))
    for i in G["block_lines"].get(block, ()):
        kind, flat = G["lines"][i]
        points = pairs(flat)
        counts[kind] = counts.get(kind, 0) + place(acc[0], 0, kind, points, only=only0)
        if kind in KEEP[1]:
            place(acc[1], 1, kind, points, only=only1)
    for i in G["block_areas"].get(block, ()):
        kind, flat, m2 = G["areas"][i]
        # a lake can be far larger than the block: first cut to it and a rim around it
        local = [(x * blocks - bx, y * blocks - by) for x, y in pairs(flat)]
        ring = [((x + bx) / blocks, (y + by) / blocks) for x, y in clip_ring(local, -0.05, 1.05)]
        if len(ring) < 3:
            continue
        counts[kind] = counts.get(kind, 0) + place(acc[0], 0, kind, ring, polygon=True, only=only0)
        if m2 >= MIN_WATER[1]:
            place(acc[1], 1, kind, ring, polygon=True, only=only1)
    named = [(kind, text, pairs(flat)) for kind, text, flat in (G["named"][i] for i in G["block_named"].get(block, ()))]
    names = candidates(G["labels"], named, to_name, inside)
    for level, only in ((0, only0), (1, only1)):
        _, tile_min, tile_max, _ = LEVELS[level]
        shown = settle(wanted_names(names, level), range(tile_min, tile_max + 1), 10, G["fonts"], G["heights"])
        place_names(names_acc[level], level, shown, only)
    index = []
    with open(out_path, "wb") as out:
        for level in (0, 1):
            write_cells(out, index, level, acc[level], names_acc[level])
    with open(out_path + ".done.part", "wb") as f:
        pickle.dump((G["run"], os.path.getsize(out_path), index, counts), f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(out_path + ".done.part", out_path + ".done")
    # The contour file is kept for a second run of the same map, but not at the price of the disk: a province
    # holds 10 GB of them, and with the disk full every block waits for room that nothing else makes.
    if shutil.disk_usage(G["work"]).free < 8e9:
        try:
            os.remove(path)
        except OSError:
            pass
    return out_path, index, counts, made - started, time.time() - made


def do_level(level):
    """A coarse level of the whole map, into a file of its own."""
    started = time.time()
    keep = KEEP[level]
    acc, names_acc = {}, {}
    own = G["own"]
    for kind, flat in G["lines"][:own[0]]:
        if kind in keep:
            place(acc, level, kind, pairs(flat))
    for kind, flat, m2 in G["areas"][:own[1]]:
        if kind in keep and m2 >= MIN_WATER[level]:
            place(acc, level, kind, pairs(flat), polygon=True)
    named = [(kind, text, pairs(flat)) for kind, text, flat in G["named_own"] if LINE_NAMES[kind][1] < NAME_RANK[level]]
    names = candidates([l for l in G["labels"][:own[2]] if l[0] in keep], named, [], lambda mx, my: True)
    _, tile_min, tile_max, _ = LEVELS[level]
    place_names(names_acc, level, settle(wanted_names(names, level), range(tile_min, tile_max + 1), 10, G["fonts"], G["heights"]))
    out_path = f"{G['work']}/tmp/level-{level}.cells"
    index = []
    with open(out_path, "wb") as out:
        write_cells(out, index, level, acc, names_acc)
    return out_path, index, {}, 0.0, time.time() - started


def do_task(task):
    return do_block(task[1]) if task[0] == "block" else do_level(task[1])


def by_block(boxes, blocks, pad):
    """Which things lie in which block: boxes are (min x, min y, max x, max y) in the Mercator square."""
    n = 1 << BLOCK
    out = {}
    for i, (x0, y0, x1, y1) in enumerate(boxes):
        for bx in range(int(x0 * n - pad), int(x1 * n + pad) + 1):
            for by in range(int(y0 * n - pad), int(y1 * n + pad) + 1):
                if (bx, by) in blocks:
                    out.setdefault((bx, by), []).append(i)
    return out


def box_of(flat):
    xs, ys = flat[0::2], flat[1::2]
    return min(xs), min(ys), max(xs), max(ys)


def main():
    args = sys.argv[1:]
    box, state, poly, workers, readers, keep_tiles, near = None, None, None, 8, 4, False, []
    plain = []
    while args:
        a = args.pop(0)
        if a == "--box":
            box = tuple(float(args.pop(0)) for _ in range(4))
        elif a == "--state":
            state = args.pop(0)
        elif a == "--poly":
            poly = args.pop(0)
        elif a == "--workers":
            workers = int(args.pop(0))
        elif a == "--readers":
            readers = int(args.pop(0))
        elif a == "--keep-tiles":
            keep_tiles = True
        elif a == "--near":
            near = args
            args = []
        else:
            plain.append(a)
    if len(plain) < 4 or sum(x is not None for x in (box, state, poly)) != 1:
        sys.exit(__doc__)
    out_file, font_file, work = plain[:3]
    for folder in ("tiles", "contours", "tmp"):
        os.makedirs(f"{work}/{folder}", exist_ok=True)
    contours_bin = f"{work}/contours-tool"
    here = os.path.dirname(os.path.abspath(__file__))
    if not os.path.exists(contours_bin) or os.path.getmtime(contours_bin) < os.path.getmtime(f"{here}/contours.cpp"):
        subprocess.run(["g++", "-O2", "-std=c++17", "-o", contours_bin, f"{here}/contours.cpp"], check=True)
    fonts, heights = font_widths(font_file)

    source = Source(box, state)
    for pbf in plain[3:]:
        log("reading", pbf)
        source.read(pbf)
    if poly:
        source.outline = read_poly(poly)
    mask = make_mask(source, box)
    # what is the state's own: the coarse levels are made of that alone
    own = (len(source.lines), len(source.areas), len(source.labels), len(source.named))
    if near:
        n = 1 << LEVELS[0][0]
        xs, ys = [c[0] for c in mask], [c[1] for c in mask]
        west, north = unmerc(min(xs) / n, min(ys) / n)
        east, south = unmerc((max(xs) + 1) / n, (max(ys) + 1) / n)
        source.box = (west, south, east, north)
        source.state = None
        shift = LEVELS[0][0] - BLOCK
        source.blocks = {(cx >> shift, cy >> shift) for cx, cy in mask}
        if readers > 1 and len(near) > 1:
            # each extract in a process of its own, some at a time; taken in the order they were given in
            G.update(source=source, work=work)
            gc.freeze()  # or the collector would write to every page the processes share
            log(f"reading {len(near)} extracts for the cells along the border, {min(readers, len(near))} at a time")
            with multiprocessing.get_context("fork").Pool(min(readers, len(near)), maxtasksperchild=1) as pool:
                for pbf, path in zip(near, pool.imap(read_near, near, chunksize=1)):
                    take_near(source, path)
                    log("read", pbf)
            del G["source"]
        else:
            for pbf in near:
                log("reading", pbf, "for the cells along the border")
                source.read(pbf)
    source.seen = None
    named = joined(source.named, source.lines)
    named_own = joined(source.named[:own[3]], source.lines) if near else named
    log(f"{len(source.lines)} lines ({own[0]} the state's own), {len(source.areas)} water areas, {len(source.labels)} names of places, "
        f"tops and waters, {len(named)} named roads and streams (from {len(source.named)} pieces)")

    masks = [mask]
    for cell_zoom, _, _, _ in LEVELS[1:]:
        shift = LEVELS[0][0] - cell_zoom
        masks.append({(cx >> shift, cy >> shift) for cx, cy in mask})
    shift = LEVELS[0][0] - BLOCK
    blocks = {(cx >> shift, cy >> shift) for cx, cy in mask}
    log(f"{len(mask)} cells in {len(blocks)} blocks")
    pad = BUFFER / EXTENT / (1 << shift) * 2
    G.update(work=work, keep_tiles=keep_tiles, contours=contours_bin, fonts=fonts, heights=heights, masks=masks,
             lines=source.lines, areas=source.areas, labels=source.labels, named=named, named_own=named_own, own=own,
             block_lines=by_block([box_of(flat) for _, flat in source.lines], blocks, pad),
             block_areas=by_block([box_of(flat) for _, flat, _ in source.areas], blocks, pad),
             block_named=by_block([box_of(flat) for _, _, flat in named], blocks, 0))
    log("indexed")

    block_list = sorted(blocks)  # from west to east: drop_tiles counts on it
    tasks = [("level", 2), ("level", 3)] + [("block", b) for b in block_list]
    results, totals, done = [], {}, 0
    # what a block kept from an earlier run must have been made of: the same map from the same data
    G["run"] = (os.path.basename(out_file), own, len(source.lines), len(source.areas), len(source.labels), len(named),
                len(mask), LEVELS, INTERVAL)
    context = multiprocessing.get_context("fork")
    makers = workers
    G["ahead"] = context.BoundedSemaphore(4 * makers)
    gc.collect()
    gc.freeze()  # or the collector of each worker would write to, and so copy, the pages they share
    held, room = memory()
    fit = max(1, int((room - 1e9) / (held * 0.5)))  # a worker comes to hold up to half of it by itself
    if fit < workers:
        log(f"{held / 1e9:.1f} GB held, {room / 1e9:.1f} GB free: {fit} workers and not {workers}")
        workers = fit
    per = 1 << (DEM - BLOCK)

    def ready(contour_pool):
        """The tasks, each block when its contour lines are there."""
        yield tasks[0]
        yield tasks[1]
        column = None
        for block in contour_pool.imap(make_contours, block_list, chunksize=1):
            if block[0] != column and not keep_tiles:
                column = block[0]
                drop_tiles(column * per - 1)  # all blocks west of this one have their contour lines
            yield ("block", block)

    with context.Pool(makers, initializer=gc.disable) as contour_pool, context.Pool(workers, initializer=gc.disable) as pool:
        for task, result in zip(tasks, pool.imap(do_task, ready(contour_pool), chunksize=1)):
            if task[0] == "block":
                G["ahead"].release()
            results.append(result)
            done += 1
            path, index, counts, contour_s, build_s = result
            for kind, count in counts.items():
                totals[kind] = totals.get(kind, 0) + count
            size = sum(e[4] + e[5] for e in index)
            log(f"{done}/{len(tasks)} {task[0]} {task[1]}: {len(index)} cells, {size:,} bytes, contours {contour_s:.0f} s, map {build_s:.0f} s")

    # the file: header, levels, a directory per level, then the cells as the workers wrote them
    grids = []
    for level, (cell_zoom, tile_min, tile_max, _) in enumerate(LEVELS):
        xs, ys = [c[0] for c in masks[level]], [c[1] for c in masks[level]]
        grids.append((min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1))
    head_size = 28 + 20 * len(LEVELS)
    data_size = sum(os.path.getsize(path) for path, _, _, _, _ in results)
    for entry, entry_format, magic in ((12, "<III", b"JMVM"), (16, "<QII", b"JMVW")):
        directory_at = [head_size]
        for x0, y0, cols, rows in grids[:-1]:
            directory_at.append(directory_at[-1] + entry * cols * rows)
        data_at = directory_at[-1] + entry * grids[-1][2] * grids[-1][3]
        if data_at + data_size <= WIDE:
            break
    if magic == b"JMVW":
        log(f"{(data_at + data_size) / 1e9:.1f} GB: over what the format holds, written with 64-bit offsets for split.py to cut")
    directories = [bytearray(entry * cols * rows) for _, _, cols, rows in grids]
    largest, per_level = [0] * len(LEVELS), [0] * len(LEVELS)
    at = data_at
    for path, index, _, _, _ in results:
        for level, cx, cy, offset, lines, names in index:
            x0, y0, cols, rows = grids[level]
            struct.pack_into(entry_format, directories[level], entry * ((cy - y0) * cols + (cx - x0)), at + offset, lines, names)
            largest[level] = max(largest[level], lines)
            per_level[level] += lines + names
        at += os.path.getsize(path)
    n = 1 << LEVELS[0][0]
    xs, ys = [c[0] for c in mask], [c[1] for c in mask]
    west, north = unmerc(min(xs) / n, min(ys) / n)
    east, south = unmerc((max(xs) + 1) / n, (max(ys) + 1) / n)
    with open(out_file + ".part", "wb") as out:
        out.write(magic + struct.pack("<HH", 4, len(LEVELS)))
        out.write(struct.pack("<iiii", int(south * 1e7), int(west * 1e7), int(north * 1e7), int(east * 1e7)))
        out.write(struct.pack("<BBH", min(l[1] for l in LEVELS), max(l[2] for l in LEVELS), 0))
        for (cell_zoom, tile_min, tile_max, _), (x0, y0, cols, rows), where in zip(LEVELS, grids, directory_at):
            out.write(struct.pack("<BBBBIIHHI", cell_zoom, tile_min, tile_max, 0, x0, y0, cols, rows, where))
        for directory in directories:
            out.write(directory)
        for path, _, _, _, _ in results:
            with open(path, "rb") as f:
                while True:
                    piece = f.read(1 << 22)
                    if not piece:
                        break
                    out.write(piece)
    os.replace(out_file + ".part", out_file)
    for path, _, _, _, _ in results:  # only now: a build that ends before this goes on from them
        for name in (path, path + ".done"):
            if os.path.exists(name):
                os.remove(name)
    if not keep_tiles:
        for name in os.listdir(f"{work}/tiles"):
            os.remove(f"{work}/tiles/{name}")
    log(f"points by kind {dict(sorted(totals.items()))}")
    for level in range(len(LEVELS)):
        log(f"level {level}: cell zoom {LEVELS[level][0]}, {grids[level][2]} x {grids[level][3]} cells, {per_level[level]:,} bytes, "
            f"largest cell {largest[level]:,}")
    log(f"{out_file}: {os.path.getsize(out_file):,} bytes")


if __name__ == "__main__":
    main()
