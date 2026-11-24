"""dem.py <west> <south> <east> <north> <out prefix>: elevation for an area as one grid.

Fetches the "terrarium" elevation tiles (zoom 14, about 7 m a pixel at 44 degrees north; in the United States
they are made from the USGS 3DEP 10 m model) from the public bucket elevation-tiles-prod, keeps them below
<out prefix>-tiles/, and writes <out prefix>.f32 (meters, float32, rows from the north) and <out prefix>.json
(which tiles: the grid is in Web Mercator pixels of zoom 14, so that contours come out in map coordinates).
"""
import json, math, os, struct, sys, urllib.request
from concurrent.futures import ThreadPoolExecutor
from PIL import Image

Z = 14
URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"


def tile_of(lon, lat):
    n = 1 << Z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return int(x), int(y)


def fetch(job):
    x, y, path = job
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        request = urllib.request.Request(URL.format(z=Z, x=x, y=y), headers={"User-Agent": "map-tools"})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    return path


def main():
    west, south, east, north = map(float, sys.argv[1:5])
    out = sys.argv[5]
    x0, y0 = tile_of(west, north)
    x1, y1 = tile_of(east, south)
    folder = out + "-tiles"
    os.makedirs(folder, exist_ok=True)
    jobs = [(x, y, f"{folder}/{x}-{y}.png") for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)]
    with ThreadPoolExecutor(4) as pool:
        list(pool.map(fetch, jobs))
    cols, rows = x1 - x0 + 1, y1 - y0 + 1
    with open(out + ".f32", "wb") as f:
        for ty in range(rows):
            tiles = [Image.open(f"{folder}/{x0 + tx}-{y0 + ty}.png").convert("RGB").tobytes() for tx in range(cols)]
            for py in range(256):
                line = []
                for t in tiles:
                    row = t[py * 768 : (py + 1) * 768]
                    line.extend((row[i] * 256.0 + row[i + 1] + row[i + 2] / 256.0) - 32768.0 for i in range(0, 768, 3))
                f.write(struct.pack(f"<{len(line)}f", *line))
    json.dump({"zoom": Z, "x0": x0, "y0": y0, "cols": cols, "rows": rows, "width": cols * 256, "height": rows * 256}, open(out + ".json", "w"))
    print(f"{cols} x {rows} tiles, grid {cols * 256} x {rows * 256}")


main()
