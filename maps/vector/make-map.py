#!/usr/bin/env python3
"""make-map.py: one vector map for the JinxMesh map of the ThinkNode M9, from start to the files for the card.

  make-map.py NAME --region us/vermont
      A state or province as download.geofabrik.de/north-america names it. Its extract, its outline and the
      extracts of its neighbors are fetched. This is how the maps of the archive were made.

  make-map.py NAME --pbf FILE.osm.pbf [more.osm.pbf ...] (--poly OUTLINE.poly | --box W S E N | --state "Name")
      Your own OpenStreetMap data, of any land. What belongs to the map: an outline file (the .poly format of
      osmosis and Geofabrik), a box in degrees, or the name an administrative area of level 4 has in the data.
      --near FILE... : extracts of the land next to it, so that the cells along the border are whole.

  make-map.py --info FILE.jmv
      What a map file holds.

Elevation, for the contour lines (--elevation):
  terrain-tiles    the Terrain Tiles data set on AWS (the default; what the archive was made with)
  https://.../{z}/{x}/{y}.png   another server of "terrarium" tiles at zoom 14
  /a/folder        the same tiles in a folder, as <folder>/14/<x>/<y>.png; what is missing gets no lines
  hgt:/a/folder    your own elevation as SRTM .hgt files (N44W073.hgt, also .hgt.gz; 1 or 3 arc seconds),
                   which are turned into such tiles first. Needs --poly or --box.
  none             no contour lines

Other options:
  --root DIR       where everything is kept (default ~/.cache/m9-tiles/custom): osm/, work/NAME/, out/NAME/
  --font FILE      the letter widths of the device (device-ui/include/util/VectorMapFont.h); made with font.py
                   when there is none
  --workers N      blocks built at a time (default 4); each needs memory, see docs/MAPS.md
  --limit MB       the largest piece (default 48); --no-split keeps the map in one file
  --keep-work      keeps the work folder (contour lines, elevation tiles) for a second run

Ends with out/NAME/: NAME.jmv or NAME-1.jmv, NAME-2.jmv, ... and NAME.json. The .jmv files go to a folder
below /maps on the card, such as /maps/state/. The whole of it is described in docs/MAPS.md.

Needs Python 3.9 or later with osmium (pyosmium) and Pillow, and g++. Next to this file: build.py, split.py,
contours.cpp, regions.py, font.py.
"""
import argparse, array, gzip, hashlib, json, math, multiprocessing, os, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
TERRAIN_TILES = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
DEM, BLOCK, CELL = 14, 10, 13  # zooms: elevation tiles, the blocks build.py works in, its finest cells


def log(*text):
    print(time.strftime("%H:%M:%S"), *text, flush=True)


def stop(text):
    raise SystemExit("make-map: " + text)


# ---- what a map file holds

def info(path):
    with open(path, "rb") as f:
        head = f.read(28)
        if len(head) < 28 or head[:4] != b"JMVM":
            stop(f"{path} is no vector map (it does not start with JMVM)")
        version, count = struct.unpack_from("<HH", head, 4)
        south, west, north, east = (v / 1e7 for v in struct.unpack_from("<iiii", head, 8))
        low, high, group = struct.unpack_from("<BBH", head, 24)
        levels = [struct.unpack("<BBBBIIHHI", f.read(20)) for _ in range(count)]
        out = dict(file=path, bytes=os.path.getsize(path), version=version, south=south, west=west, north=north, east=east,
                   lowest_zoom=low, highest_zoom=high, group=group, levels=[])
        for cell_zoom, first, last, _, x0, y0, cols, rows, where in levels:
            f.seek(where)
            table = f.read(12 * cols * rows)
            used = sum(1 for i in range(cols * rows) if struct.unpack_from("<I", table, 12 * i)[0])
            size = sum(sum(struct.unpack_from("<II", table, 12 * i + 4)) for i in range(cols * rows))
            out["levels"].append(dict(cell_zoom=cell_zoom, tile_zooms=[first, last], columns=cols, rows=rows, cells=used, bytes=size))
    return out


def show(path):
    m = info(path)
    print(f"{m['file']}: {m['bytes']:,} bytes, JMVM version {m['version']}")
    print(f"  south {m['south']:.5f}  west {m['west']:.5f}  north {m['north']:.5f}  east {m['east']:.5f}")
    print(f"  drawn at zooms {m['lowest_zoom']} to {m['highest_zoom']}" + (f", a piece of map {m['group']}" if m["group"] else ""))
    for level in m["levels"]:
        print(f"  cells of zoom {level['cell_zoom']} for zooms {level['tile_zooms'][0]} to {level['tile_zooms'][1]}: "
              f"{level['cells']} of {level['columns']} x {level['rows']}, {level['bytes']:,} bytes")


# ---- elevation of one's own: .hgt files into terrarium tiles

def merc_y(lat, n):
    return (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n


def lat_of(y, n):
    return math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / n))))


def read_poly_box(path):
    """West, south, east, north of an outline file: lines of 'lon lat' between names and END."""
    lons, lats = [], []
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            try:
                lon, lat = float(parts[0]), float(parts[1])
            except ValueError:
                continue
            lons.append(lon)
            lats.append(lat)
    if not lons:
        stop(f"{path} holds no outline (lines of longitude and latitude)")
    return min(lons), min(lats), max(lons), max(lats)


class Hgt:
    """Heights from a folder of .hgt files, a degree each, named by their south-west corner."""

    def __init__(self, folder):
        self.folder, self.held, self.order = folder, {}, []

    def tile(self, lat, lon):
        key = (lat, lon)
        if key in self.held:
            return self.held[key]
        name = f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}.hgt"
        grid = None
        for path, opener in ((f"{self.folder}/{name}", open), (f"{self.folder}/{name}.gz", gzip.open)):
            if os.path.exists(path):
                with opener(path, "rb") as f:
                    data = f.read()
                side = math.isqrt(len(data) // 2)
                if side * side * 2 != len(data) or side not in (1201, 3601):
                    stop(f"{path}: {len(data)} bytes is neither 1201 x 1201 nor 3601 x 3601 heights")
                heights = array.array("h")
                heights.frombytes(data)
                if sys.byteorder == "little":
                    heights.byteswap()  # the files are big endian
                grid = (side, heights)
                break
        self.held[key] = grid
        self.order.append(key)
        if len(self.order) > 6:
            self.held.pop(self.order.pop(0), None)
        return grid

    def at(self, lat, lon):
        """Meters at a place, between the four heights around it; None where there is no data."""
        la, lo = math.floor(lat), math.floor(lon)
        grid = self.tile(la, lo)
        if grid is None:
            return None
        side, heights = grid
        fx, fy = (lon - lo) * (side - 1), (la + 1 - lat) * (side - 1)  # rows run from the north
        x, y = min(int(fx), side - 2), min(int(fy), side - 2)
        fx, fy = fx - x, fy - y
        i = y * side + x
        a, b, c, d = heights[i], heights[i + 1], heights[i + side], heights[i + side + 1]
        if a == -32768 or b == -32768 or c == -32768 or d == -32768:
            return None
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


G = {}


def hgt_tile(job):
    from PIL import Image
    x, y, path = job
    if os.path.exists(path):
        return 0
    source, n = G["hgt"], 1 << DEM
    lons = [(x + (px + 0.5) / 256.0) / n * 360.0 - 180.0 for px in range(256)]
    pixels, any_data = bytearray(256 * 256 * 3), False
    for py in range(256):
        lat = lat_of(y + (py + 0.5) / 256.0, n)
        at = py * 768
        for px in range(256):
            h = source.at(lat, lons[px])
            if h is None:
                continue  # black: no data, and no contour line runs through it
            v = min(max(h + 32768.0, 1 / 256.0), 65535.99)
            whole = int(v)
            pixels[at + px * 3] = whole >> 8
            pixels[at + px * 3 + 1] = whole & 255
            pixels[at + px * 3 + 2] = int((v - whole) * 256)
            any_data = True
    if not any_data:
        return 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.frombytes("RGB", (256, 256), bytes(pixels)).save(f"{path}.{os.getpid()}.part", "PNG")
    os.replace(f"{path}.{os.getpid()}.part", path)
    return 1


def tiles_from_hgt(folder, box, out, workers):
    """Terrarium tiles of zoom 14 for the land in a box, and what build.py reads around it."""
    if not os.path.isdir(folder):
        stop(f"no folder {folder}")
    west, south, east, north = box
    n = 1 << CELL
    # build.py reaches a cell over the outline, works in whole blocks and reads a tile around each
    cx0, cx1 = int((west + 180) / 360 * n) - 1, int((east + 180) / 360 * n) + 1
    cy0, cy1 = int(merc_y(north, n)) - 1, int(merc_y(south, n)) + 1
    shift, per = CELL - BLOCK, 1 << (DEM - BLOCK)
    x0, x1 = (cx0 >> shift) * per - 1, ((cx1 >> shift) + 1) * per
    y0, y1 = (cy0 >> shift) * per - 1, ((cy1 >> shift) + 1) * per
    jobs = [(x, y, f"{out}/{DEM}/{x}/{y}.png") for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]
    log(f"elevation: {len(jobs):,} tiles of zoom {DEM} from the .hgt files in {folder}")
    G["hgt"] = Hgt(folder)
    made = 0
    with multiprocessing.get_context("fork").Pool(workers) as pool:
        for i, one in enumerate(pool.imap_unordered(hgt_tile, jobs, chunksize=16), 1):
            made += one
            if i % 2000 == 0:
                log(f"elevation: {i:,} of {len(jobs):,}")
    have = sum(1 for _, _, path in jobs if os.path.exists(path))
    log(f"elevation: {made:,} tiles made, {have:,} there, {len(jobs) - have:,} without data")
    if not have:
        stop(f"none of the .hgt files in {folder} covers the land of the map")


# ---- the steps

def need_tools():
    missing = []
    for module, package in (("osmium", "osmium"), ("PIL", "Pillow")):
        try:
            __import__(module)
        except ImportError:
            missing.append(f"the Python package {package} (pip install {package})")
    if not shutil.which("g++"):
        missing.append("g++ (the contour tool is compiled at the first run)")
    for name in ("build.py", "split.py", "contours.cpp", "regions.py", "font.py"):
        if not os.path.exists(f"{HERE}/{name}"):
            missing.append(f"{name} next to make-map.py")
    if missing:
        stop("missing: " + "; ".join(missing))


def find_font(given, work):
    places = [given] if given else [os.environ.get("FONT"), f"{HERE}/VectorMapFont.h",
                                    f"{HERE}/../../../device-ui/include/util/VectorMapFont.h",
                                    os.path.expanduser("~/github_forks/device-ui/include/util/VectorMapFont.h")]
    for path in places:
        if path and os.path.exists(path):
            return os.path.abspath(path)
    if given:
        stop(f"no font file {given}")
    path = f"{work}/VectorMapFont.h"
    log("no VectorMapFont.h of the device found: making one with font.py (the names are placed by its letter widths;"
        " they fit a device whose font was made the same way)")
    if subprocess.run([sys.executable, f"{HERE}/font.py", path]).returncode != 0 or not os.path.exists(path):
        stop("font.py failed; it needs the Liberation Sans Bold font (package fonts-liberation), or give --font")
    return path


def run(command, env=None):
    log("running:", " ".join(command))
    code = subprocess.run(command, env=env).returncode
    if code != 0:
        stop(f"{os.path.basename(command[1])} ended with {code}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, usage=argparse.SUPPRESS)
    p.add_argument("name", nargs="?")
    p.add_argument("--info")
    p.add_argument("--region")
    p.add_argument("--pbf", nargs="+", default=[])
    p.add_argument("--near", nargs="+", default=[])
    p.add_argument("--poly")
    p.add_argument("--box", nargs=4, type=float, metavar=("W", "S", "E", "N"))
    p.add_argument("--state")
    p.add_argument("--elevation", default="terrain-tiles")
    p.add_argument("--root", default=os.path.expanduser("~/.cache/m9-tiles/custom"))
    p.add_argument("--font")
    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--limit", type=int, default=48)
    p.add_argument("--no-split", action="store_true")
    p.add_argument("--keep-work", action="store_true")
    a = p.parse_args()
    if a.info:
        return show(a.info)
    if not a.name or not all(c.isalnum() or c in "-_" for c in a.name):
        stop("give the map a name of letters, digits, - and _ (it becomes the file name); --help says more")
    if bool(a.region) == bool(a.pbf):
        stop("give either --region or --pbf")
    shape = [x is not None for x in (a.poly, a.box, a.state)]
    if a.region and any(shape):
        stop("--region brings its own outline: leave --poly, --box and --state out")
    if a.pbf and sum(shape) != 1:
        stop("with --pbf give one of --poly, --box, --state")
    if a.box and not (-180 <= a.box[0] < a.box[2] <= 180 and -85 <= a.box[1] < a.box[3] <= 85):
        stop("--box is west south east north in degrees, west of east and south of north")
    for path in a.pbf + a.near + ([a.poly] if a.poly else []):
        if not os.path.exists(path):
            stop(f"no file {path}")
    need_tools()

    root = os.path.abspath(a.root)
    work, out = f"{root}/work/{a.name}", f"{root}/out/{a.name}"
    os.makedirs(work, exist_ok=True)
    os.makedirs(out, exist_ok=True)
    font = find_font(a.font, work)

    # the sources
    if a.region:
        log(f"fetching what {a.region} needs into {root}/osm")
        got = subprocess.run([sys.executable, f"{HERE}/regions.py", f"{root}/osm", a.region], stdout=subprocess.PIPE, text=True)
        if got.returncode != 0 or not got.stdout.strip():
            stop(f"could not fetch the data of {a.region}")
        source = got.stdout.split()
        poly = source[1]
    else:
        source = (["--poly", a.poly] if a.poly else ["--box"] + [str(v) for v in a.box] if a.box else ["--state", a.state]) + a.pbf
        if a.near:
            source += ["--near"] + a.near
        poly = a.poly

    # the elevation
    env = dict(os.environ)
    how = a.elevation
    if how == "terrain-tiles":
        env["M9_DEM_URL"] = TERRAIN_TILES
    elif how == "none":
        env["M9_DEM_URL"] = "file://" + f"{work}/no-elevation" + "/{z}/{x}/{y}.png"
    elif how.startswith("hgt:"):
        box = tuple(a.box) if a.box else read_poly_box(poly) if poly else None
        if not box:
            stop("elevation from .hgt files needs --poly or --box, to know which land to make tiles of")
        tiles_from_hgt(os.path.abspath(how[4:]), box, f"{work}/elevation", a.workers)
        env["M9_DEM_URL"] = "file://" + f"{work}/elevation" + "/{z}/{x}/{y}.png"
    elif how.startswith(("http://", "https://", "file://")):
        if not all(part in how for part in ("{z}", "{x}", "{y}")):
            stop("an elevation address needs {z}, {x} and {y} in it")
        env["M9_DEM_URL"] = how
    elif os.path.isdir(how):
        env["M9_DEM_URL"] = "file://" + os.path.abspath(how) + "/{z}/{x}/{y}.png"
    else:
        stop(f"--elevation {how}: neither terrain-tiles, none, hgt:<folder>, an address nor a folder")
    # contour lines are kept per block in the work folder: those of another source must not be taken for these
    mark = f"{work}/elevation-source.txt"
    if os.path.exists(mark) and open(mark).read() != env["M9_DEM_URL"]:
        log("the elevation source changed: the contour lines kept from the run before are removed")
        for folder in ("contours", "tiles"):
            shutil.rmtree(f"{work}/{folder}", ignore_errors=True)
    open(mark, "w").write(env["M9_DEM_URL"])

    # the map
    whole = f"{work}/{a.name}.jmv"
    begun = time.time()
    run([sys.executable, f"{HERE}/build.py", whole, font, work, "--workers", str(a.workers)] + source, env)
    if not os.path.exists(whole):
        stop("build.py wrote no map")

    # the pieces
    for old in os.listdir(out):
        if old.endswith((".jmv", ".json")):
            os.remove(f"{out}/{old}")
    if a.no_split:
        shutil.copyfile(whole, f"{out}/{a.name}.jmv")
    else:
        run([sys.executable, f"{HERE}/split.py", whole, out, "--limit", str(a.limit)])
    files = sorted(f for f in os.listdir(out) if f.endswith(".jmv"))
    if not files:
        stop("no map file came out")
    print()
    for name in files:
        show(f"{out}/{name}")
        digest = hashlib.sha256(open(f"{out}/{name}", "rb").read()).hexdigest()
        print(f"  SHA-256 {digest}")
    json.dump({"name": a.name, "made": time.strftime("%Y-%m-%d"), "sources": source, "elevation": a.elevation,
               "files": files}, open(f"{out}/{a.name}.made.json", "w"), indent=1)
    if not a.keep_work:
        for folder in ("contours", "tiles", "tmp", "elevation"):
            shutil.rmtree(f"{work}/{folder}", ignore_errors=True)
        os.remove(whole)
    print()
    log(f"done in {int((time.time() - begun) / 60)} min: {len(files)} file(s) in {out}")
    print("Copy the .jmv files to a folder below /maps on the card, for example /maps/state/. The other files stay here.")


if __name__ == "__main__":
    main()
