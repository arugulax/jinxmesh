"""Area and zoom range of an M9 offline map, read from a pack definition.

The definition is a JSON file in packs/. M9_PACK names it (a path, or a name in packs/ without .json);
the default is the counties between Burlington and St. Johnsbury. docs/MAP-PACKS.md describes the fields.
"""
import json
import math
import os

_here = os.path.dirname(os.path.abspath(__file__))
_pack = os.environ.get("M9_PACK", "central-vt")
PACK_FILE = _pack if os.path.sep in _pack or _pack.endswith(".json") else os.path.join(_here, "packs", _pack + ".json")
with open(PACK_FILE, encoding="utf-8") as _source:
    PACK = json.load(_source)

STYLE = PACK["style"]
NAME = PACK["name"]
ATTRIBUTION = PACK["attribution"]
WEST, SOUTH, EAST, NORTH = PACK["west"], PACK["south"], PACK["east"], PACK["north"]
ZOOM_MIN, ZOOM_MAX = PACK["zoom_min"], PACK["zoom_max"]
# below this zoom the box gets a ring of extra tiles, so the screen is filled when zoomed out
PAD_BELOW = PACK.get("pad_below", 12)
CENTER_ZOOM = PACK.get("center_zoom", min(ZOOM_MAX, max(ZOOM_MIN, 11)))

if not (-180 <= WEST < EAST <= 180 and -85 <= SOUTH < NORTH <= 85):
    raise SystemExit(f"{PACK_FILE}: the box must be west < east, south < north, inside +-180 and +-85")
if not (2 <= ZOOM_MIN <= ZOOM_MAX <= 18):
    raise SystemExit(f"{PACK_FILE}: zooms must be 2 <= zoom_min <= zoom_max <= 18")
if not (1 <= len(STYLE) <= 19) or STYLE.startswith(".") or not all(c.isalnum() or c in "_-" for c in STYLE) or not STYLE.isascii():
    raise SystemExit(f"{PACK_FILE}: style must be 1 to 19 characters from A-Z a-z 0-9 _ -")


def tile(lat, lon, z):
    n = 2**z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n)
    return x, y


def tiles(z):
    x0, y0 = tile(NORTH, WEST, z)
    x1, y1 = tile(SOUTH, EAST, z)
    pad = 1 if z < PAD_BELOW else 0
    n = 2**z
    for x in range(max(0, x0 - pad), min(n - 1, x1 + pad) + 1):
        for y in range(max(0, y0 - pad), min(n - 1, y1 + pad) + 1):
            yield z, x, y


if __name__ == "__main__":
    # the plan of a pack: tiles per zoom, and the size to expect at the 24 kB a 64-colour topo tile averages
    print(f"{PACK_FILE}: style {STYLE}, box {WEST} {SOUTH} {EAST} {NORTH}, zoom {ZOOM_MIN} to {ZOOM_MAX}")
    total = 0
    for z in range(ZOOM_MIN, min(18, ZOOM_MAX + 1) + 1):
        count = sum(1 for _ in tiles(z))
        total += count
        note = "" if z <= ZOOM_MAX else "  (one level past zoom_max, for comparison)"
        print(f"z{z}: {count} tiles, {total} so far, about {total * 24056 / 1e6:.0f} MB{note}")
