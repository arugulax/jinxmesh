"""regions.py <osm folder> <region>: fetches what build.py needs for the map of a state or province.

A region is named as download.geofabrik.de/north-america has it: us/vermont, canada/quebec. Fetched into
the folder, when not there yet: its extract and the outline it was cut along, and the extracts of the
regions next to it (those whose outline comes within a cell of its own), for the cells along its borders.
Prints the arguments for build.py: --poly <outline> <extract> [--near <extracts of the neighbors>].
"""
import datetime, json, os, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # build.py, next to this file
import build

BASE = "https://download.geofabrik.de/north-america"
ZOOM = 11  # cells this size tell neighbors from regions that merely come close


def fetch(url, path):
    request = urllib.request.Request(url, headers={"User-Agent": "map-tools"})
    with urllib.request.urlopen(request, timeout=120) as response, open(path + ".part", "wb") as out:
        while True:
            piece = response.read(1 << 20)
            if not piece:
                break
            out.write(piece)
    os.replace(path + ".part", path)


def extract(folder, region):
    """The extract of a region; the one of the day, or of the days before while that is not out yet."""
    name = region.split("/")[-1]
    path = f"{folder}/{name}-latest.osm.pbf"
    if os.path.exists(path) and os.path.getsize(path):
        return path
    today = datetime.datetime.now(datetime.timezone.utc).date()
    tries = ["latest"] + [(today - datetime.timedelta(days=d)).strftime("%y%m%d") for d in range(1, 6)]
    for which in tries:
        try:
            print(f"fetching {region} ({which})", file=sys.stderr, flush=True)
            fetch(f"{BASE}/{region}/{name}-{which}.osm.pbf", path)
            return path
        except Exception as error:
            last = error
    raise SystemExit(f"no extract of {region}: {last}")


def outline(folder, region):
    name = region.split("/")[-1]
    path = f"{folder}/{name}.poly"
    if not os.path.exists(path) or not os.path.getsize(path):
        fetch(f"{BASE}/{region}/{name}.poly", path)
    return path


def all_regions(folder):
    """The states and provinces there are extracts of."""
    path = f"{folder}/geofabrik-index.json"
    if not os.path.exists(path) or time.time() - os.path.getmtime(path) > 30 * 86400:
        fetch("https://download.geofabrik.de/index-v1-nogeom.json", path)
    out = []
    for feature in json.load(open(path))["features"]:
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


def main():
    folder, region = sys.argv[1:3]
    os.makedirs(folder, exist_ok=True)
    regions = all_regions(folder)
    if region not in regions:
        raise SystemExit(f"{region} is not one of {' '.join(regions)}")
    own = build.read_poly(outline(folder, region))
    own_box = box_of(own)
    own_cells = build.dilate(build.ring_cells(own, 1 << ZOOM))
    near = []
    for other in regions:
        if other == region:
            continue
        rings = build.read_poly(outline(folder, other))
        b = box_of(rings)
        pad = 2.0 / (1 << ZOOM)
        if b[0] > own_box[2] + pad or b[2] < own_box[0] - pad or b[1] > own_box[3] + pad or b[3] < own_box[1] - pad:
            continue
        # only the part of the other near this one's box matters: its outline points there, and whether it
        # covers cells of this one
        if build.ring_cells(rings, 1 << ZOOM) & own_cells:
            near.append(other)
    print(f"{region}: next to {' '.join(near) or 'nothing'}", file=sys.stderr, flush=True)
    args = ["--poly", outline(folder, region), extract(folder, region)]
    if near:
        args += ["--near"] + [extract(folder, other) for other in near]
    print(" ".join(args))


main()
