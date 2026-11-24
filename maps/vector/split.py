"""split.py <map.jmv> <out folder> [--limit MB]: cuts a vector map into pieces of at most --limit (default 48).

A state can be hundreds of megabytes: too much for one file to host or to send over a cable. The pieces are
maps like any other ("JMVM", see device-ui/include/util/VectorMap.h), which the device shows side by side as
it does states, so a card needs only those of the land it is used in. Nothing is built again: the cells are
copied as they are.

The land is cut along the cells of the third level (zoom 9, about 78 x 57 km at 44 degrees north): halved
across its longer side, by bytes, until each part fits. A piece holds the cells of the finer levels within
its own, of the coarsest level every cell that reaches into it, and in every level the cells around its own
(a tile shows names of the cells next to it). So pieces hold the same cells along their cuts, as the maps of two
states do along their border; the header of each names the map they are pieces of (a number made of its name),
by which the device draws such a cell from one of them only.

Writes <name>-<n>.jmv, numbered from the north-west, or <name>.jmv when it fits in one, and <name>.json:
the pieces with their corners, sizes and SHA-256.
"""
import hashlib, json, math, os, struct, sys, zlib

CUT = 9  # the cell zoom the land is cut along


def unmerc(x, y):
    return x * 360.0 - 180.0, math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y))))


def read(path):
    f = open(path, "rb")
    head = f.read(28)
    if head[:4] != b"JMVM":
        raise SystemExit(f"{path} is no vector map")
    version, count = struct.unpack_from("<HH", head, 4)
    if version != 4:
        raise SystemExit(f"{path}: version {version}, this tool knows 4")
    zooms = head[24:28]
    levels = []
    for _ in range(count):
        cell_zoom, first, last, _, x0, y0, cols, rows, where = struct.unpack("<BBBBIIHHI", f.read(20))
        levels.append(dict(zoom=cell_zoom, first=first, last=last, x0=x0, y0=y0, cols=cols, rows=rows, where=where))
    for level in levels:
        f.seek(level["where"])
        table = f.read(12 * level["cols"] * level["rows"])
        cells = {}
        for i in range(level["cols"] * level["rows"]):
            offset, lines, names = struct.unpack_from("<III", table, 12 * i)
            if offset:
                cells[(level["x0"] + i % level["cols"], level["y0"] + i // level["cols"])] = (offset, lines, names)
        level["cells"] = cells
    return f, zooms, levels


def halves(parts, weight, limit):
    """parts: cells of the cut zoom; split until the weight of each group is within the limit."""
    total = sum(weight[c] for c in parts)
    if total <= limit or len(parts) == 1:
        return [parts]
    xs, ys = [c[0] for c in parts], [c[1] for c in parts]
    axis = 0 if max(xs) - min(xs) >= max(ys) - min(ys) else 1
    lines = sorted(set(c[axis] for c in parts))
    if len(lines) == 1:
        axis = 1 - axis
        lines = sorted(set(c[axis] for c in parts))
    best, at = None, None
    for line in lines[1:]:  # the cut that comes nearest to half the bytes
        low = sum(weight[c] for c in parts if c[axis] < line)
        if best is None or abs(low - total / 2) < best:
            best, at = abs(low - total / 2), line
    return halves([c for c in parts if c[axis] < at], weight, limit) + halves([c for c in parts if c[axis] >= at], weight, limit)


def write(f, zooms, levels, own, path, group):
    """own: the cells of the cut zoom this piece is made of."""
    chosen = []
    for level in levels:
        shift = level["zoom"] - CUT
        if shift >= 0:
            mine = {c for c in level["cells"] if (c[0] >> shift, c[1] >> shift) in own}
        else:
            above = {(c[0] >> -shift, c[1] >> -shift) for c in own}
            mine = {c for c in level["cells"] if c in above}
        # and the cells around them: a tile shows names of the cells next to its own, and the device asks
        # one map for both, as it does along the border of a state
        ring = {(c[0] + dx, c[1] + dy) for c in mine for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
        chosen.append({c: level["cells"][c] for c in ring if c in level["cells"]})
        if level is levels[0]:
            core = mine
    if not core:
        return None  # no land in the finest level: nothing a map would show
    grids = []
    for level, cells in zip(levels, chosen):
        if cells:
            xs, ys = [c[0] for c in cells], [c[1] for c in cells]
            grids.append((min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1))
        else:
            grids.append((level["x0"], level["y0"], 1, 1))
    where = [28 + 20 * len(levels)]
    for _, _, cols, rows in grids[:-1]:
        where.append(where[-1] + 12 * cols * rows)
    at = where[-1] + 12 * grids[-1][2] * grids[-1][3]
    tables = [bytearray(12 * cols * rows) for _, _, cols, rows in grids]
    copies = []
    for i, cells in enumerate(chosen):
        x0, y0, cols, rows = grids[i]
        for (cx, cy), (offset, lines, names) in sorted(cells.items(), key=lambda item: item[1][0]):
            struct.pack_into("<III", tables[i], 12 * ((cy - y0) * cols + (cx - x0)), at, lines, names)
            copies.append((offset, lines + names))
            at += lines + names
    n = 1 << levels[0]["zoom"]
    xs, ys = [c[0] for c in core], [c[1] for c in core]  # the corners: of its own land, without the rim
    west, north = unmerc(min(xs) / n, min(ys) / n)
    east, south = unmerc((max(xs) + 1) / n, (max(ys) + 1) / n)
    digest = hashlib.sha256()
    with open(path + ".part", "wb") as out:
        def put(data):
            out.write(data)
            digest.update(data)
        put(b"JMVM" + struct.pack("<HH", 4, len(levels)))
        put(struct.pack("<iiii", int(south * 1e7), int(west * 1e7), int(north * 1e7), int(east * 1e7)))
        put(zooms[:2] + struct.pack("<H", group))
        for level, (x0, y0, cols, rows), w in zip(levels, grids, where):
            put(struct.pack("<BBBBIIHHI", level["zoom"], level["first"], level["last"], 0, x0, y0, cols, rows, w))
        for table in tables:
            put(bytes(table))
        for offset, length in copies:
            f.seek(offset)
            put(f.read(length))
    os.replace(path + ".part", path)
    return dict(file=os.path.basename(path), bytes=at, sha256=digest.hexdigest(), south=round(south, 5), west=round(west, 5),
                north=round(north, 5), east=round(east, 5))


def main():
    args = sys.argv[1:]
    limit = 48.0
    if "--limit" in args:
        i = args.index("--limit")
        limit = float(args[i + 1])
        del args[i : i + 2]
    source, folder = args
    name = os.path.basename(source)[: -len(".jmv")]
    f, zooms, levels = read(source)
    if not any(level["zoom"] == CUT for level in levels):
        raise SystemExit(f"{source} has no level of zoom {CUT}")
    weight = {}
    for level in levels:  # the bytes of the finer levels, by the cell they are cut with
        shift = level["zoom"] - CUT
        if shift >= 0:
            for (cx, cy), (_, lines, names) in level["cells"].items():
                key = (cx >> shift, cy >> shift)
                weight[key] = weight.get(key, 0) + lines + names + 12
    coarse = sum(l + n for level in levels if level["zoom"] < CUT for _, l, n in level["cells"].values())
    room = int(limit * 1e6 * 0.82) - coarse - 4096  # the coarsest cells may all be in one piece; the rest is for the rim
    if room <= 0:
        raise SystemExit("the limit is below the size of the coarsest level")
    groups = halves(sorted(weight), weight, room)
    groups.sort(key=lambda g: (min(c[1] for c in g), min(c[0] for c in g)))
    os.makedirs(folder, exist_ok=True)
    shared = zlib.crc32(name.encode()) % 65535 + 1  # what the pieces of this map share: see VectorMap::group()
    pieces = []
    for group in groups:
        number = len(pieces) + 1
        path = f"{folder}/{name}.jmv" if len(groups) == 1 else f"{folder}/{name}-{number}.jmv"
        piece = write(f, zooms, levels, set(group), path, 0 if len(groups) == 1 else shared)
        if piece:
            pieces.append(piece)
    with open(f"{folder}/{name}.json", "w") as out:
        json.dump(dict(name=name, format="JMVM 4", group=0 if len(groups) == 1 else shared, bytes=sum(p["bytes"] for p in pieces), pieces=pieces), out, indent=1)
    for p in pieces:
        print(f"{p['file']}: {p['bytes']:,} bytes, {p['south']} {p['west']} to {p['north']} {p['east']}")
    over = [p["file"] for p in pieces if p["bytes"] > limit * 1e6]
    if over:
        print(f"over the limit (one cell of zoom {CUT} is that large): {' '.join(over)}")


main()
