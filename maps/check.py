"""Check a .pmtiles archive against what the M9 map can read; exit status 0 when it passes.

usage: check.py ARCHIVE.pmtiles [STYLE]
STYLE is the folder name it will get on the card (/maps/STYLE/STYLE.pmtiles); the default is the file's name.
Run with a Python that has the pmtiles package: python -I check.py ...
"""
import os
import sys

from pmtiles.reader import MmapSource, Reader
from pmtiles.tile import Compression, TileType, tileid_to_zxy

path = sys.argv[1]
style = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(os.path.basename(path))[0]
size = os.path.getsize(path)
problems = []
with open(path, "rb") as archive:
    reader = Reader(MmapSource(archive))
    header = reader.header()
    metadata = reader.metadata()
    if header["version"] != 3:
        problems.append(f"PMTiles version {header['version']}, the device reads 3")
    if header["tile_type"] != TileType.PNG:
        problems.append(f"tile type {header['tile_type']}, the device reads PNG")
    if header["tile_compression"] != Compression.NONE:
        problems.append(f"tile compression {header['tile_compression']}, the device needs NONE")
    if header["internal_compression"] != Compression.GZIP:
        problems.append(f"internal compression {header['internal_compression']}, the device needs GZIP")
    if header["min_zoom"] < 2:
        problems.append(f"min_zoom {header['min_zoom']}, the device starts at 2")
    if header["max_zoom"] > 18:
        problems.append(f"max_zoom {header['max_zoom']}, the device stops at 18")
    if not (header["min_lon_e7"] < header["max_lon_e7"] and header["min_lat_e7"] < header["max_lat_e7"]):
        problems.append("the header has no bounding box; the map centers on it when there is no GPS fix")
    # one tile from the middle of the deepest zoom: 256 pixels, PNG, palette
    z = header["max_zoom"]
    lon = (header["min_lon_e7"] + header["max_lon_e7"]) / 2e7
    lat = (header["min_lat_e7"] + header["max_lat_e7"]) / 2e7
    import math

    n = 2**z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n)
    sample = reader.get(z, x, y)
    if sample is None:
        problems.append(f"no tile at the center of zoom {z} ({z}/{x}/{y})")
    elif sample[:8] != b"\x89PNG\r\n\x1a\n":
        problems.append(f"tile {z}/{x}/{y} is not a PNG")
    else:
        width, height = int.from_bytes(sample[16:20], "big"), int.from_bytes(sample[20:24], "big")
        if (width, height) != (256, 256):
            problems.append(f"tile {z}/{x}/{y} is {width}x{height}, the device needs 256x256")
if size >= 2**32:
    problems.append(f"{size} bytes: FAT32 holds at most 4,294,967,295 bytes per file")
if not (1 <= len(style) <= 19) or style.startswith(".") or not style.isascii() or not all(c.isalnum() or c in "_-" for c in style):
    problems.append(f"style name {style!r}: use 1 to 19 characters from A-Z a-z 0-9 _ -")

print(f"{path}: {size} bytes, {header['addressed_tiles_count']} tiles, zoom {header['min_zoom']} to {header['max_zoom']}")
print(f"box west {header['min_lon_e7'] / 1e7} south {header['min_lat_e7'] / 1e7} east {header['max_lon_e7'] / 1e7} north {header['max_lat_e7'] / 1e7}")
print(f"name {metadata.get('name')!r}, attribution {metadata.get('attribution')!r}")
print(f"on the card: /maps/{style}/{style}.pmtiles; the map zooms from {max(2, header['min_zoom'])} to {min(18, header['max_zoom'] + 2)}")
for problem in problems:
    print("PROBLEM:", problem)
print("CHECK_OK" if not problems else f"CHECK_FAILED {len(problems)}")
sys.exit(1 if problems else 0)
