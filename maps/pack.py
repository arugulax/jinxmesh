"""Turn the cached tiles into one raster .pmtiles archive the M9 map reads.

usage: pack.py CACHE_DIR OUT.pmtiles [ZOOM_MAX]
The device needs PNG tiles, stored uncompressed, with gzip directories; tiles are cut to 64 colours to keep them small.
"""
import io
import os
import sys
from multiprocessing import Pool

from PIL import Image
from pmtiles.tile import Compression, TileType, zxy_to_tileid
from pmtiles.writer import Writer

# run with python -I (the cache holds downloaded files), which drops the script's directory from the path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import area  # noqa: E402

COLOURS = 64


def to_png(data):
    image = Image.open(io.BytesIO(data)).convert("RGB")
    if image.size != (256, 256):
        image = image.resize((256, 256), Image.LANCZOS)
    # octree keeps thin contour and road lines; median cut and max coverage wash them out or cost more bytes
    image = image.quantize(COLOURS, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    out = io.BytesIO()
    image.save(out, "PNG", optimize=True)
    return out.getvalue()


def convert(job):
    tile_id, path = job
    try:
        with open(path, "rb") as source:
            return tile_id, to_png(source.read())
    except Exception as error:
        return tile_id, error


def main():
    cache, target = sys.argv[1], sys.argv[2]
    zoom_max = int(sys.argv[3]) if len(sys.argv) > 3 else area.ZOOM_MAX
    jobs = []
    for z in range(area.ZOOM_MIN, zoom_max + 1):
        for _, x, y in area.tiles(z):
            path = os.path.join(cache, str(z), str(x), f"{y}.jpg")
            if os.path.exists(path):
                jobs.append((zxy_to_tileid(z, x, y), path))
    jobs.sort()
    bad = 0
    with open(target, "wb") as out, Pool(2) as pool:
        writer = Writer(out)
        for done, (tile_id, png) in enumerate(pool.imap(convert, jobs, chunksize=64), 1):
            if isinstance(png, Exception):
                bad += 1
                continue
            writer.write_tile(tile_id, png)
            if done % 2000 == 0:
                print(f"{done}/{len(jobs)}", flush=True)
        centre = ((area.WEST + area.EAST) / 2, (area.SOUTH + area.NORTH) / 2)
        writer.finalize(
            {
                "tile_type": TileType.PNG,
                "tile_compression": Compression.NONE,
                "min_lon_e7": int(area.WEST * 1e7),
                "min_lat_e7": int(area.SOUTH * 1e7),
                "max_lon_e7": int(area.EAST * 1e7),
                "max_lat_e7": int(area.NORTH * 1e7),
                "center_zoom": area.CENTER_ZOOM,
                "center_lon_e7": int(centre[0] * 1e7),
                "center_lat_e7": int(centre[1] * 1e7),
            },
            {
                "name": area.NAME,
                "attribution": area.ATTRIBUTION,
                "format": "png",
            },
        )
    print(f"PACK_DONE {len(jobs) - bad} tiles, {bad} unreadable, {os.path.getsize(target)} bytes", flush=True)


if __name__ == "__main__":
    main()
