# Other map data for the card

Besides the [vector maps](MAPS.md), three kinds of data can go on the M9's SD card. Each is
optional and works without the others.

| Path on the card | What | Used by |
|---|---|---|
| `/maps/<style>/<style>.pmtiles` | Raster map tiles in one archive | Map tab |
| `/maps/<style>/<z>/<x>/<y>.png` | The same as loose files | Map tab |
| `/maps/<style>/.url` | Lets the device fetch missing tiles over Wi-Fi | Map tab |
| `/geo/towns.bin` | Town outlines and names | Town locator, emergency message cards, node details, the Town tile |
| `/geo/roads.bin` | Roads and trails as a routing graph | Navigate |

This page describes each, and how to build one for your own area. All tools are in `maps/` of
this repository and need Python 3.

## Raster maps

A raster map is a set of ordinary 256-pixel PNG map tiles, either as loose files or packed
into one PMTiles archive. Use it for a scanned topo sheet or any other picture map. For most
uses the vector maps are smaller and sharper.

### Rules an archive must follow

Break one of these and the device shows nothing, or the wrong thing, without an error
message. `maps/check.py` tests the ones marked.

| Rule | Why | check.py |
|---|---|---|
| Archive path is `/maps/<style>/<style>.pmtiles`: folder and file carry the same name | the loader builds the path from the style name | prints the path |
| Style name: 1 to 19 characters from `A-Z a-z 0-9 _ -`, not starting with a dot | buffer size in device-ui; the folder name is the name shown in the Map style list | yes |
| PMTiles version 3 | the only one the reader knows | yes |
| Tile type PNG, 256 x 256 pixels | the decoder is PNG only and the map math assumes 256 | yes, one sample tile |
| Tile compression NONE | tiles are handed to the PNG decoder as stored | yes |
| Internal compression GZIP | the directory reader only inflates gzip | yes |
| At most 4 directory levels including the root | reader limit; the Python writer makes at most 2 | no |
| `min_zoom` at least 2, `max_zoom` at most 18 | the map's range | yes |
| The header carries the bounding box | with no GPS fix the map centers on it | yes |
| File smaller than 4,294,967,296 bytes | the cards are FAT32 | yes |
| Nothing under `/maps` that is not a tile style | every folder below `/maps` is listed as a map style | no |


Worth knowing before you choose zoom levels:

- **Zoom range on the device:** a style that is only an archive zooms from the archive's
  `min_zoom` to its `max_zoom + 2`, never past 18. The two extra levels are the deepest tiles
  drawn at 2x and 4x. An archive that ends at zoom 15 therefore zooms to 17.
- **Missing tiles:** a tile that is absent is drawn from its parent or grandparent when one
  exists. Holes show as blur, not as blanks, as long as two levels above exist.
- **Default style:** when none has been picked on the device, a style with an archive wins
  over styles with loose tiles.
- **Colors:** `pack.py` cuts tiles to 64 colors. That keeps a topo tile near 24 kB and keeps
  contour and road lines readable.
- **Size:** the largest archive tried so far is 500 MB with 20,849 tiles. How much memory a
  much larger one needs on the device has not been measured, so split big areas into several
  styles.
- **Replacing the archive of the style in use:** the map keeps the old file open. Pick the
  style again in the Map style list, or restart, after replacing it.

### Building an archive

The tools download tiles for a box you define and pack them:

| File | Does |
|---|---|
| `maps/packs/<name>.json` | The definition of one archive: style, name, box, zooms |
| `maps/area.py` | Reads the definition named by `M9_PACK`; run alone, it prints the plan |
| `maps/fetch.py` | Downloads the tiles of the box into a cache; run again to resume |
| `maps/pack.py` | Converts the cached tiles and writes the archive |
| `maps/check.py` | Tests an archive against the rules above |

They need `pip install Pillow pmtiles`.

**1. Define it.** Copy `maps/packs/central-vt.json` to a new name and edit it.

| Field | Meaning | Limits |
|---|---|---|
| `style` | folder and file name on the card | 1 to 19 of `A-Z a-z 0-9 _ -` |
| `name` | title stored in the archive | free text |
| `attribution` | credit stored in the archive | free text |
| `west`, `south`, `east`, `north` | the box, degrees | west < east, south < north |
| `zoom_min`, `zoom_max` | zoom levels to include | 2 to 18 |
| `pad_below` | zooms below this get one ring of extra tiles around the box | default 12 |
| `center_zoom` | zoom stored as the archive's default | default 11, clamped to the range |


**2. Look at the plan.**

```
cd maps
M9_PACK=<name> python3 area.py
```

It prints tiles per zoom, the running total and the size to expect. For the example, a box
of central Vermont about 120 by 90 km:

| Zoom | Tiles | Total | About |
|---|---|---|---|
| 6 to 12 | 481 | 481 | 12 MB |
| 13 | 1,036 | 1,517 | 36 MB |
| 14 | 3,888 | 5,405 | 130 MB |
| 15 | 15,444 | 20,849 | 502 MB |
| 16 (not built) | 61,060 | 81,909 | 1,970 MB |

Each zoom level has four times the tiles of the one before. A box four times the area costs
the same as one more zoom level. At 24 kB per tile, the FAT32 limit is reached near 178,000
tiles in one archive.

Each zoom level has four times the tiles of the one before. A box four times the area costs
the same as one more zoom level.

**3. Fetch.**

```
M9_PACK=<name> python3 fetch.py ~/.cache/m9-tiles/raw > fetch.log 2>&1
```

- **Source:** USGS The National Map, service `USGSTopo`, public domain, United States only.
  The address is the constant `URL` in `fetch.py`. For another source, change it, use another
  cache folder, read that source's terms for bulk download (OpenStreetMap's own tile servers
  forbid it), and set the `attribution` to match.
- **Manners:** four connections, a User-Agent that says what this is, five tries per tile
  with growing pauses. Please keep it that way; the server is a public service.
- The last line of the log is `FETCH_DONE` with counts. `failed` above zero means run it
  again.

**4. Pack.**

```
M9_PACK=<name> python3 -I pack.py ~/.cache/m9-tiles/raw ~/.cache/m9-tiles/out/<style>.pmtiles > pack.log 2>&1
```

The last line of the log is `PACK_DONE <tiles>, <unreadable>, <bytes>`. Unreadable above zero
means a broken download: delete that file from the cache and fetch again. `-I` is there
because the cache holds downloaded files.

**5. Check.**

```
python3 -I check.py ~/.cache/m9-tiles/out/<style>.pmtiles
```

It prints the size, tile count, zoom range, box, the path on the card and the zoom range the
device will offer, then `CHECK_OK` or one `PROBLEM:` line per broken rule.

**6. Put it on the card** as `/maps/<style>/<style>.pmtiles`, then pick the style on the
device: Map, OK held, Map style.

### Choosing area and zoom

- **What the screen shows:** 320 x 240 pixels is a little more than one tile. At zoom 15 a
  tile at 44 degrees north is about 880 m wide, so the screen spans a bit over 1 km; at zoom
  13, about 4.4 km.
- **Zoom 15 is the detail level of the USGS topo sheet.** Zoom 16 from that source is the
  same drawing enlarged, at four times the bytes, and the device already stretches the
  deepest level by 2x and 4x. Zoom 16 is rarely worth it.
- **Wide and shallow plus narrow and deep:** two styles work well, a state at zoom 6 to 12
  and the home area at 6 to 15. You switch between them in the Map style list.
- **Low zooms are nearly free.** Start at zoom 6 so that zooming out shows context.

## Town data: `/geo/towns.bin`

The Town locator answers "which town am I in?" from this file, and the same lookup puts town
names on emergency message cards and in node details. The unit is the town as the US Census
draws it (the county subdivision), for the whole United States.

The national file has 36,332 towns in 14.6 MB and builds in under a minute:

```
pip install pyshp
python3 -I maps/towns.py --us 2025 --out towns.bin
```

Other arguments: `--states VT,NH` or `--box west,south,east,north` for a smaller file;
`--geojson FILE --name-field F --area-field F` for any other polygon layer, such as the
municipalities of another country; `--accuracy METERS` and `--thin METERS` for how far lines
are simplified.

Things to know about the data:

- **Source:** the Census cartographic boundary file `cb_<year>_us_cousub_500k`, public
  domain. It is drawn for small-scale maps, so a line can be off by 100 m or more. The file
  records an accuracy figure (150 m by default), and within that distance of a line the
  device says "near the ... line" instead of asserting a town.
- **What "town" means differs by state.** In about half the states the Census subdivisions
  are real governments (towns, townships). In the rest they are statistical areas named
  "... CCD" that nobody uses in speech. The file marks each record's kind.
- **Same name twice in a county** (Barre city and Barre town in Vermont): both get their kind
  in the name, "Barre City" and "Barre Town".
- **Water and unorganized areas** such as gores and grants are kept as records of their own
  kind where the source has them.
- **The far Aleutians** beyond the date line are left out.
- **Names** are UTF-8; the device shows Latin letters only.
- The file must be `/geo/towns.bin`, one per card, and never below `/maps`, where every
  folder is taken for a map style.

### File format, version 1

Little-endian. Offsets are from the start of the file. Coordinates in the header and the
records are `int32` in units of 1e-7 degrees, the same as Meshtastic's `latitude_i`.

**Header, 64 bytes**

| Offset | Type | Field |
|---|---|---|
| 0 | 4 bytes | magic `KBTN` |
| 4 | uint16 | version, 1 |
| 6 | uint16 | accuracy in meters |
| 8 | int32 x 4 | box of all records: min lon, min lat, max lon, max lat |
| 24 | uint32 | cell size in 1e-7 degrees (the builder picks; 0.1° = 1,000,000) |
| 28 | uint16 x 2 | grid columns, grid rows |
| 32 | uint32 | record count |
| 36 | uint32 | offset of the grid |
| 40 | uint32 | offset of the records |
| 44 | uint32 | offset of the name index |
| 48 | uint32 | offset of the strings |
| 52 | uint32 | offset of the rings |
| 56 | uint32 | file length |
| 60 | uint32 | build date as the number yyyymmdd |

**Grid.** `columns x rows + 1` values of `uint32`: where each cell's list starts, counted in
entries, row by row from the south-west corner; the last value is the total. Then the lists:
`uint32` record numbers, each cell listing every record whose box touches it.

**Records, 40 bytes each**

| Offset | Type | Field |
|---|---|---|
| 0 | int32 x 4 | box: min lon, min lat, max lon, max lat |
| 16 | int32 x 2 | label point: lon, lat (a point inside the largest ring) |
| 24 | uint32 | name, offset into the strings ("Montpelier") |
| 28 | uint32 | area, offset into the strings ("Washington County, VT") |
| 32 | uint32 | first ring, offset into the rings |
| 36 | uint16 | ring count |
| 38 | uint8 | kind: 0 town or city with a government, 1 statistical area, 2 water or not defined, 3 unorganized (gore, grant, territory) |
| 39 | uint8 | reserved, 0 |

**Name index.** Record numbers as `uint32`, sorted by name without regard to case, for search
by typing (binary search on the first letters, then a scan).

**Strings.** UTF-8, each ended by a zero byte, stored once and shared.

**Rings.** Each ring: `uint32` point count, then the first point as two `int32` in units of
1e-5 degrees (lon, lat), then every further point as two `int16` steps in the same unit. The
builder splits a step too long for `int16`. Rings are closed without repeating the first
point. Outer rings and holes are stored alike.

### How the device looks a point up

1. Outside the header's box: "outside the town data".
2. Find the cell, read its list.
3. For each record in the list whose box holds the point: count how many edges of all its
   rings a line running east from the point crosses. Odd means inside. Holes and islands need
   no special case. Integer arithmetic only, in units of 1e-5 degrees, with 64-bit products.
4. The first record that is inside is the answer. Records do not overlap in the sources.
5. While walking the rings of the records in the list, keep the smallest distance to an edge
   of a different record; below the header's accuracy it becomes "near the ... line".

Rings are read through a small buffer, never loaded whole. The answer for the own position is
cached and looked up again only after moving 50 m.


The reader is `include/util/TownIndex.h` on the device-ui branch. It never loads the file
whole, and caches the answer for the own position until the device has moved 50 m.

## Road data: `/geo/roads.bin`

What Navigate routes over: roads, tracks, paths and ferries from OpenStreetMap as a graph.

```
pip install osmium
python3 maps/roads.py build roads.bin vermont.osm.pbf new-hampshire.osm.pbf --bbox minlon,minlat,maxlon,maxlat
```

The inputs are OpenStreetMap extracts, for example the per-state files from Geofabrik. A file
covering Vermont, New Hampshire and northern New York is 78 MB (1.26 million vertices, 3.09
million directed edges) and takes about four minutes and 2 GB of memory to build. No
ready-made road files are published yet; build one for the area you need.

- **What is in it:** every way with a `highway` class a car or a person may use, and ferries.
  A vertex stands wherever ways meet or end, and at least every 800 m, so that a way through a
  cell of the grid has a vertex within one cell of it. Islands of fewer than 40 vertices are left
  out: nothing leads out of them.
- **Per edge:** length, class (motorway to steps, ferry), whether a car may use it in this
  direction (one-way streets, `access`, `motor_vehicle`, poor tracks), whether feet may, its shape
  simplified to 3 m, its name (the `ref` for main roads, else the name).
- **Not in it:** turn restrictions, speed limits, seasonal closures, surface. A car is given a
  speed per class; the times are estimates.
- **Order:** vertices are sorted by their 0.01 degree cell, cells in Z order, so that what is near
  on the ground is near in the file. The device reads 4 kB blocks and keeps 256 of them.
- **Layout:** in the head of `maps/roads.py` and in `device-ui/include/util/RouteGraph.h`.
- **Checking a file:** `roads.py route FILE lat,lon lat,lon car|foot` is the reference router;
  `tools/route-check` runs the device's router on a desktop and prints length, settled vertices
  and block reads. On six pairs up to 365 km the two agree to within a few meters.
- **License:** OpenStreetMap contributors, ODbL 1.0. The file is a derived database: whoever
  passes it on says so and offers it under the same license.

## Credits your data carries

If you pass a map archive or data file on, pass its credit on with it.

| Data | Credit |
|---|---|
| USGS topo tiles | "USGS The National Map" (public domain; credit by courtesy) |
| Census boundaries | "U.S. Census Bureau" (public domain; credit by courtesy) |
| OpenStreetMap, including `roads.bin` and fetched tiles | "(c) OpenStreetMap contributors", required; the file is a derived database under the ODbL |
