"""build-all.py [--first <region>] [--floor GB] [--plan]: the maps of all states and provinces that are not built yet.

One after the other with build-states.sh, the nearest to --first (default us/vermont) first; the far north and
the islands (Alaska, Hawaii, Yukon, Northwest Territories, Nunavut) last. Each is cut into pieces of at most 48 MB
(split.py) into ~/.cache/m9-tiles/vector/pieces, with a list <state>.json, and put into the maps repository (publish.py), after which the pieces are removed
here; a state whose list is there is skipped, a state that failed is tried again by the next run.

The disk is small next to a continent, so after each state: the contours kept for it are removed (they are
made again when a neighbor needs the same blocks), and so is every extract that no state still to do needs,
as itself or as a neighbor. It stops when less than --floor GB (default 4) are free.
"""
import json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build

ROOT = os.environ.get("M9_TILES", os.path.expanduser("~/.cache/m9-tiles"))
OSM, STATES, WORK = f"{ROOT}/osm", f"{ROOT}/vector/states", f"{ROOT}/vector/work/all"
PIECES = f"{ROOT}/vector/pieces"  # the same maps cut into files of at most 48 MB (split.py), with a list each
ZOOM = 11
LAST = ["us/hawaii", "us/alaska", "canada/yukon", "canada/northwest-territories", "canada/nunavut"]


def log(text):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), text, flush=True)


def regions():
    out = []
    for feature in json.load(open(f"{OSM}/geofabrik-index.json"))["features"]:
        p = feature["properties"]
        if p.get("parent") == "us" and p["id"].startswith("us/"):
            out.append(p["id"])
        elif p.get("parent") == "canada":
            out.append("canada/" + p["id"])
    return out


def box_of(rings):
    xs = [x for r in rings for x in r[0::2]]
    ys = [y for r in rings for y in r[1::2]]
    return min(xs), min(ys), max(xs), max(ys)


def name(region):
    return region.split("/")[-1]


def main():
    args = sys.argv[1:]
    first, floor, plan = "us/vermont", 4.0, False
    while args:
        a = args.pop(0)
        if a == "--first":
            first = args.pop(0)
        elif a == "--floor":
            floor = float(args.pop(0))
        elif a == "--plan":
            plan = True
    every = regions()
    polys, boxes, cells = {}, {}, {}
    for r in every:
        path = f"{OSM}/{name(r)}.poly"
        if not os.path.exists(path):
            log(f"no outline of {r}: left out")
            continue
        polys[r] = build.read_poly(path)
        boxes[r] = box_of(polys[r])
        cells[r] = build.ring_cells(polys[r], 1 << ZOOM)
    every = [r for r in every if r in polys]
    pad = 2.0 / (1 << ZOOM)

    def near(r):  # as regions.py finds them
        own, b = build.dilate(cells[r]), boxes[r]
        out = []
        for o in every:
            c = boxes[o]
            if o == r or c[0] > b[2] + pad or c[2] < b[0] - pad or c[1] > b[3] + pad or c[3] < b[1] - pad:
                continue
            if cells[o] & own:
                out.append(o)
        return out

    def middle(r):
        b = boxes[r]
        return (b[0] + b[2]) / 2, (b[1] + b[3]) / 2

    fx, fy = middle(first)
    order = sorted(every, key=lambda r: (r in LAST, (middle(r)[0] - fx) ** 2 + (middle(r)[1] - fy) ** 2))
    # what is built but not cut up yet (the states made before there were pieces): cut, the whole one stays
    for r in order:
        if os.path.exists(f"{STATES}/{name(r)}.jmv") and not os.path.exists(f"{PIECES}/{name(r)}.json"):
            subprocess.run([sys.executable, "-I", f"{HERE}/split.py", f"{STATES}/{name(r)}.jmv", PIECES], stdout=subprocess.DEVNULL)
            log(f"cut {r}")
    # and what is cut but did not get into the maps repository
    for r in order:
        if os.path.exists(f"{PIECES}/{name(r)}.json") and "published" not in json.load(open(f"{PIECES}/{name(r)}.json")):
            sent = subprocess.run([sys.executable, "-I", f"{HERE}/publish.py", r], capture_output=True, text=True)
            log(sent.stdout.strip() or sent.stderr.strip()[-300:])
    todo = [r for r in order if not os.path.exists(f"{PIECES}/{name(r)}.json")]
    log(f"{len(every)} regions, {len(every) - len(todo)} there, to do: {' '.join(todo)}")
    neighbors = {r: near(r) for r in todo}
    if plan:
        return
    failed = []
    while todo:
        free = shutil.disk_usage(ROOT).free / 1e9
        if free < floor:
            log(f"STOPPED: {free:.1f} GB free, below {floor} GB. Not done: {' '.join(todo)}")
            break
        r = todo.pop(0)
        log(f"START {r} ({free:.1f} GB free, {len(todo)} after it)")
        started = time.time()
        subprocess.run(["bash", f"{HERE}/build-states.sh", r])
        path = f"{STATES}/{name(r)}.jmv"
        if os.path.exists(path):
            size = os.path.getsize(path)
            cut = subprocess.run([sys.executable, "-I", f"{HERE}/split.py", path, PIECES], capture_output=True, text=True)
            if cut.returncode == 0 and os.path.exists(f"{PIECES}/{name(r)}.json"):
                os.remove(path)  # the pieces are the map now
                log(f"OK {r} {size:,} bytes in {len(cut.stdout.splitlines())} pieces, {(time.time() - started) / 60:.0f} min")
                # into the maps repository, which frees the room here; a state that did not get there keeps its
                # pieces, and "publish.py <region>" brings it there later
                sent = subprocess.run([sys.executable, "-I", f"{HERE}/publish.py", r], capture_output=True, text=True)
                log(sent.stdout.strip() or sent.stderr.strip()[-300:])
            else:
                failed.append(r)
                log(f"FAILED {r}: built, but not cut: {cut.stderr.strip()[-300:]}")
        else:
            failed.append(r)
            log(f"FAILED {r} after {(time.time() - started) / 60:.0f} min")
        for folder in ("contours", "tiles", "tmp"):
            for f in os.listdir(f"{WORK}/{folder}"):
                os.remove(f"{WORK}/{folder}/{f}")
        needed = set()
        for t in todo:
            needed.add(name(t))
            needed.update(name(n) for n in neighbors[t])
        for f in os.listdir(OSM):
            if f.endswith("-latest.osm.pbf") and f[: -len("-latest.osm.pbf")] not in needed:
                os.remove(f"{OSM}/{f}")
            elif f.endswith(".part"):
                os.remove(f"{OSM}/{f}")
    log(f"END. Failed: {' '.join(failed) or 'none'}")


main()
