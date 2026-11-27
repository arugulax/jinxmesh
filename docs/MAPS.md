# How the JinxMesh vector maps are made

For anyone who wants to build a map again, or build one of other land or from other data.

The maps are the `.jmv` files the ThinkNode M9 draws its map from: contour lines, water, roads
and trails, rails, borders and names. The finished maps are in the
repositories `jinxmesh-maps-<region>` next to this one; the tools are in `maps/vector/` of this
repository.

Other map data of the device has its own description in `docs/MAP-DATA.md`: raster tiles
(`.pmtiles`), the town file (`towns.bin`, from the US Census) and the road file for Navigate
(`roads.bin`, from OpenStreetMap by `maps/roads.py`).

## 1. The short way

One command makes a map from start to the files for the card:

```
cd maps/vector

# a state or province, as the archive's maps were made
python make-map.py vermont --region us/vermont

# your own data: any OpenStreetMap extract, with an outline, a box or an area's name
python make-map.py valley --pbf my-extract.osm.pbf --box -72.9 44.1 -72.4 44.5
python make-map.py tirol  --pbf austria.osm.pbf --poly tirol.poly --near germany.osm.pbf italy.osm.pbf

# your own elevation, as SRTM .hgt files in a folder
python make-map.py valley --pbf my-extract.osm.pbf --box -72.9 44.1 -72.4 44.5 --elevation hgt:/data/hgt

# what a map file holds
python make-map.py --info valley.jmv
```

The result is in `~/.cache/m9-tiles/custom/out/<name>/` (`--root` puts it elsewhere): the map as
`<name>.jmv`, or cut into `<name>-1.jmv`, `<name>-2.jmv`, ... when it is over 48 MB. Copy the
`.jmv` files to a folder below `/maps` on the card (section 9). `python make-map.py --help`
lists every option.

Tried with Washington DC: own extract with an outline and no contour lines (25 s),
with the default elevation (69 s), and with a box and elevation from an `.hgt` file (68 s).
`--region` hands over to the same two tools the queue runs every day, but that path of
`make-map.py` itself has not been run to its end. None of the three test maps was looked at
on a device.

## 2. What you need

| What | Why |
|---|---|
| Linux, Python 3.9 or later | The tools start their workers by fork |
| `pip install osmium Pillow` | Reading OpenStreetMap extracts (pyosmium 4), reading and writing elevation tiles |
| `g++` | `contours.cpp` is compiled at the first run; it has no dependencies |
| `VectorMapFont.h` of device-ui | The widths of the device's letters, by which names are placed. Found by itself when device-ui lies next to the scripts repository; otherwise `--font`, or it is made by `font.py` from Liberation Sans Bold (package `fonts-liberation`), the same way the device's was |
| Internet | For Geofabrik extracts and the elevation tiles, unless both are your own |
| `git` and `gh`, signed in | Only for `publish.py` |

The maps in the archive were built with Python 3.13, osmium 4.3 and Pillow 13.

What to expect, measured on a machine with 20 cores and 16 GB of memory:

| Map | Time | Memory | Note |
|---|---|---|---|
| Washington DC, 1.7 MB | 1 min | small | 2 workers |
| Louisiana, 78 MB | 20 min | not measured | Up to 16 workers |
| Ontario, 1,999 blocks | over 4 hours, of which 2 fetching and reading eight neighbors | 3.7 GB held by the data, so 5 workers instead of 16 | The largest so far |

The number of workers is cut down by `build.py` itself when memory is short: each worker may
come to need about half of what the data holds. Disk: an extract is 10 MB to 2 GB; the work
folder of a large province reaches 5 GB with the tile pruning described in section 5.

## 3. Where the data is from

| Data | Source | License | Used for |
|---|---|---|---|
| OpenStreetMap extracts, `.osm.pbf` | `https://download.geofabrik.de/north-america/<country>/<name>-latest.osm.pbf`, one per state or province, renewed daily | ODbL, (c) OpenStreetMap contributors | Roads, trails, rails, water, coast, borders, names, peaks |
| Outlines, `.poly` | Next to each extract at Geofabrik: the line the extract was cut along | As above | What belongs to a map, and which states are neighbors |
| The list of regions | `https://download.geofabrik.de/index-v1-nogeom.json` | | The queue's list of the states of the US and the provinces of Canada |
| Elevation | Terrain Tiles on AWS, `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/14/<x>/<y>.png` (`https://registry.opendata.aws/terrain-tiles/`) | Open data; sources at `https://github.com/tilezen/joerd/blob/master/docs/attribution.md`. In the United States the USGS 3D Elevation Program, 10 m | Contour lines |
| Letter shapes | Liberation Sans Bold, drawn at two sizes by `font.py` | SIL Open Font License | The names on the map, and their widths in the builder |

A map made from OpenStreetMap is a database derived from it and stays under the ODbL: whoever
passes a map on says "(c) OpenStreetMap contributors" and names the elevation source.

Nothing else goes in. The extracts are taken as they are on the day of the build; the date is
not written into the map.

## 4. The tools

All in `maps/vector/`.

| File | What it does |
|---|---|
| `make-map.py` | One map from sources to pieces, with your own data if wanted (section 1) |
| `regions.py` | Fetches a region's extract and outline and those of its neighbors; prints the arguments for `build.py` |
| `build.py` | Makes the map: reads the extracts, fetches elevation, makes contour lines, places names, writes the `.jmv` |
| `contours.cpp` | Contour lines of an elevation grid by marching squares; compiled and called by `build.py` |
| `split.py` | Cuts a map into pieces of at most 48 MB, with a list `<name>.json` |
| `publish.py` | Puts the pieces of a state into the repository of its region at GitHub |
| `build-all.py` | The queue: every state and province not built yet, one after the other, through all of the above |
| `build-states.sh` | `regions.py` and `build.py` for the regions named; what the queue calls |
| `font.py` | Writes `VectorMapFont.h`, the device's two map fonts |
| `dem.py` | An elevation grid for a box, as one file; used for trials, not by the build |
| `maps-readme.md` | The README of every maps repository |

## 5. What `build.py` does, step by step

`build.py <out.jmv> <font.h> <work folder> (--poly FILE | --box W S E N | --state NAME)
[--workers N] [--readers N] [--keep-tiles] <osm.pbf>... [--near <osm.pbf>...]`

1. **Reads the extract** with pyosmium and keeps what the map shows (section 6). Positions
   become Web Mercator.
2. **Settles what belongs to the map.** The outline (`--poly`), the box, or the area named
   (`--state`, an administrative area of level 4 found in the extract) is turned into the set of
   cells of zoom 13 it touches, plus one cell around: a map reaches one cell over its outline.
3. **Reads the neighbors** (`--near`), several at a time in processes of their own
   (`--readers`, default 4), and keeps of them only what lies in blocks of this map. So the
   cells along a border hold both sides, and the same cell comes out the same in the maps of
   both neighbors. Without neighbors, the cells along the border are empty beyond it.
4. **Works through the land in blocks**, a tile of zoom 10 each (about 39 x 28 km at 44 degrees
   north), `--workers` at a time. Per block:
   - the elevation tiles of zoom 14 are fetched, the block's 16 x 16 and one around;
   - `contours.cpp` makes the contour lines, every 20 feet, smoothed a little and thinned;
     they are kept as `<work>/contours/10-<x>-<y>.bin`, so a neighbor that shares the block
     does not make them again;
   - the elevation tiles no other block needs are removed (a province holds 100 GB of them
     otherwise; `--keep-tiles` keeps them);
   - the two fine levels of the map are built for the block's cells: lines clipped to cells,
     thinned, names placed.
5. **Makes the two coarse levels** in one piece each, from the state's own data alone.
6. **Writes the file:** header, a directory per level, the cells.

Things it guards against, each learned from a failed run:

- **A full disk:** it waits while less than 1.5 GB are free in the work folder, and a finished
  block gives up its contour file when less than 8 GB are free.
- **A contour file cut short** by an earlier crash: checked before it is used again, made anew
  when it is not whole.
- **Too little memory:** fewer workers, by what the data holds and what is free.
- **A tile that cannot be fetched:** five tries; a tile the source does not have (403, 404) is
  taken as land without data, and gets no contour lines.

Names are settled in the build, not on the device: for each zoom, which names have room
without running into each other (the more important first) and how they lie along their road
or stream. Long words are cut short (`Street` to `St`, `Mountain` to `Mtn`; the list `ENDINGS`
in `build.py`), and letters outside ASCII become `?`: the device's font has ASCII only.

## 6. What goes on the map

From OpenStreetMap tags (`HIGHWAY` and `Source` in `build.py`):

| On the map | Tags |
|---|---|
| Motorway | `highway=motorway`, `motorway_link` |
| Primary road | `trunk`, `primary` and their links |
| Secondary road | `secondary`, `tertiary` and their links |
| Minor road | `unclassified`, `residential`, `living_street`, `road` |
| Service road | `service` (not when `access=private`) |
| Track | `track` (not when `access=private`) |
| Path | `path`, `footway`, `cycleway`, `bridleway`, `steps`; no sidewalks and crossings; not when `access=private` |
| Rail | `railway=rail` without a `service` tag |
| River, stream | `waterway=river` or `canal`; `waterway=stream` |
| Water area | `natural=water`, `waterway=riverbank`, `landuse=reservoir` |
| Coast | `natural=coastline` |
| Town or county line | `boundary=administrative`, `admin_level` 6 or 8 |
| State line | `admin_level=4` |
| National border | `admin_level=2`, where two countries share the line |
| City, town, village | `place=` of those |
| Peak | `natural=peak`, with `ele` shown in feet |
| Names of roads, streams, lakes | `name`; road numbers from `ref` |

Contour lines come from the elevation: every 20 feet, with the 100 and 500 foot lines told
apart and heights written on them. Heights are in feet throughout; it is fixed in the build
(`INTERVAL`, `FT`).

Four levels of detail, each the same land in cells that are Web Mercator tiles:

| Level | Cells of zoom | Drawn at zooms | Holds |
|---|---|---|---|
| 0 | 13 | 13 to 17 | Everything |
| 1 | 11 | 11 to 12 | 500 foot contours, rivers, water over 4 ha, coast, roads down to minor, rail, all borders, places, peaks |
| 2 | 9 | 9 to 10 | Rivers, water over 50 ha, coast, roads down to secondary, state and national lines, cities and towns |
| 3 | 7 | 7 to 8 | Rivers, water over 400 ha, coast, motorways and primary roads, state and national lines, cities and towns |

## 7. Your own sources

**OpenStreetMap data of your own.** Any `.osm.pbf` works: a Geofabrik extract of another
country, a cut made with `osmium extract`, an export with your own edits. Give it with `--pbf`
and say what belongs to the map:

- `--poly FILE`: an outline in the `.poly` format (a name line, then per ring a name line, lines
  of longitude and latitude, `END`; a final `END`). Geofabrik has one next to every extract. The
  surest of the three.
- `--box W S E N`: degrees, west south east north.
- `--state "Name"`: the `name` of an administrative area of level 4 in the data.

`--near` takes the extracts of the land around, for whole cells along the border. Leave it out
for an island or a map that stands alone.

Data that is not OpenStreetMap must be brought into its form first (ways with the tags of
section 6, as `.osm.pbf`); the builder reads nothing else.

**Elevation of your own**, with `--elevation`:

| Value | Meaning |
|---|---|
| `terrain-tiles` | The default source of section 3 |
| `hgt:/a/folder` | SRTM `.hgt` files, a degree each, named by their south-west corner (`N44W073.hgt`, or `.hgt.gz`), 1201 or 3601 heights a side, 16-bit big endian, meters, `-32768` for no data. They are turned into tiles for the land of the map first. Needs `--poly` or `--box` |
| `/a/folder` | Elevation tiles you made yourself: `<folder>/14/<x>/<y>.png` |
| `https://.../{z}/{x}/{y}.png` | Another server of the same tiles |
| `none` | No contour lines |

The tiles are "terrarium" tiles: 256 x 256 pixel PNG, RGB, Web Mercator, zoom 14 only. A height
in meters is `red * 256 + green + blue / 256 - 32768`. A black pixel means no data, and no
contour line runs through it. A missing tile means the same.

Checked: a tile made from `N38W078.hgt` (1 arc second) agrees with the Terrain
Tiles tile of the same place to 0.35 m in the middle, 1.35 m for 95 % of the pixels, with no
shift. Lines from 1 arc second data are coarser than those from the 10 m model: the test map had
a third as many contour points.

`.hgt` files are open at, among other places,
`https://s3.amazonaws.com/elevation-tiles-prod/skadi/N44/N44W073.hgt.gz`. Elevation in another
form (GeoTIFF, a national model) has to be brought into `.hgt` files or terrarium tiles with
other tools; nothing here does that.

`build.py` takes the elevation source from the environment variable `M9_DEM_URL` (an address
with `{z}`, `{x}`, `{y}`; `file:///...` for a folder). `make-map.py` sets it, and keeps a work
folder per map so that contour lines of one source are never taken for another's.

**What is fixed in the code** and needs an edit of `build.py` to change: feet and the 20 foot
interval, the tags of section 6, the four levels, the shortening of English road names, ASCII
names.

## 8. Cutting into pieces, and the archive

**Pieces** (`split.py <map.jmv> <out folder> [--limit MB]`). A state can be hundreds of
megabytes: too much to host as one file or to send over a cable. The land is cut along cells of
zoom 9, halved across its longer side by bytes until every part is under the limit (48 MB).
Nothing is built again; cells are copied. Pieces hold the same cells along their cuts, and the
header of each names the map they are pieces of, so the device draws a shared cell once.
Output: `<name>-<n>.jmv` numbered from the north-west, or `<name>.jmv` when it fits in one, and
`<name>.json` with each piece's corners, size and SHA-256.

A map of more than 4 GB does not fit the format, whose offsets are 32 bits. `build.py` writes
such a map (a province of the far north) with 64-bit offsets and the mark "JMVW" in place of
"JMVM". That file is only for `split.py`, which reads both; the pieces are ordinary maps.

**Long builds.** Fetching elevation and making contour lines runs in `--workers` processes of
its own, ahead of the map building, which memory may limit to fewer. A build that is stopped
keeps its finished blocks in the work folder, and the next run of the same map with the same
extracts takes them again.

**The archive** (`publish.py <region>...`). One repository per region,
`<owner>/jinxmesh-maps-<region>` (`M9_MAPS_OWNER` names the owner), with the folders
`state/` and `province/` as on the card. Which state is in which repository is the table
`REGIONS` in `publish.py`; a new country gets keys with its two-letter code (`mx-`, `gb-`).
Commits are at most 1 GB. A repository that is not there yet is made, private; make it public at GitHub when you want to share it.

**The queue** (`build-all.py [--first us/vermont] [--floor GB] [--plan]`). Builds every state
and province that has no list in `vector/pieces/` yet, the nearest to `--first` first, the far
north and the islands last; cuts and publishes each; removes what the disk cannot keep
(contours after each state, extracts no later state needs). A state that failed is tried again
only by the next start of the queue. Its log is `~/.cache/m9-tiles/vector/build-all.log`.
Started detached, from `maps/vector/`:

```
setsid nohup python -I build-all.py >> ~/.cache/m9-tiles/vector/build-all.log 2>&1 < /dev/null &
```

`--plan` only prints what it would do.

Where things are kept, below `~/.cache/m9-tiles` (`M9_TILES` names another place):

| Folder | Holds |
|---|---|
| `osm/` | Extracts, outlines, the Geofabrik list |
| `vector/states/` | Whole maps as built |
| `vector/pieces/` | Pieces until they are published, and the lists that mark a state as done |
| `vector/work/all/` | Contours, elevation tiles, temporary files of the queue. Many small files: keep it on a local disk |
| `custom/` | What `make-map.py` makes |

## 9. Onto the device

- Copy the `.jmv` files to a folder below `/maps` on the card. The archive uses `/maps/state/`
  and `/maps/province/`; the folder is a map style, its name 1 to 19 characters.
- The device shows all files of the folder side by side, up to 240 files.
- Not all pieces of a state are needed, only those of the land the device is used in.
- Do not put a whole map and its pieces into the same folder.
- Pieces need device-ui `6797d75` or later to join cleanly.
- The `.json` files stay off the card.

Ways to copy: take the card out; the device's upload page over Wi-Fi (about 300 kB a second);
the cable (about 3.5 MB a minute). `REFERENCE.md` has the details.

## 10. The file format

"JMVM" version 4, little endian. The reference is the comment at the top of
`include/util/VectorMap.h` in device-ui; in short:

```
"JMVM" | u16 version | u16 levels
i32 south, west, north, east            degrees times 1e7
u8 lowest tile zoom | u8 highest | u16 group    0, or what the pieces of one map share
per level: u8 cell zoom | u8 first tile zoom | u8 last | u8 0 | u32 x of the first cell | u32 y |
           u16 columns | u16 rows | u32 offset of its directory
directories: per cell u32 offset, u32 bytes of lines and areas, u32 bytes of names
cells: lines and areas, then names
```

Within a cell positions run from 0 to 4096 and reach a little over the edge. A feature starts
with a byte for its kind; lines are steps from point to point as zigzag varints; contour lines
carry their height in feet; a name carries its place, the zooms it shows at, its turn and its
text. `make-map.py --info FILE` prints the header and the size of each level.

### Into other formats

`tools/jmv-convert.py` writes what a map holds as GeoJSON, GPX or KML, for other programs. It
needs Python 3 and nothing else:

```
python3 tools/jmv-convert.py vermont-3.jmv vermont-4.jmv out.geojson
python3 tools/jmv-convert.py area.jmv trails.gpx --layers trails,names
python3 tools/jmv-convert.py --info area.jmv
```

It takes the finest level. A road that crosses from one cell of the map to the next is two
lines in the map and two in the output. GPX and KML leave the contour lines out unless
`--layers` names them. The map browser on the web site saves any area as a `.jmv` to feed it.

## 11. When something goes wrong

| What you see | Cause | What to do |
|---|---|---|
| `STATE <name> FAILED: could not fetch its data`, many states within seconds | No name lookup or no network at that moment | Start the queue again; it takes up the failed ones |
| `NOT published: ... Could not resolve host` | The same, at the push | The next start of the queue publishes it first |
| `unpack requires a buffer of ... bytes` in a build | A contour file cut short by an earlier crash; current builds check for it | Remove `<work>/contours/` and build again |
| `less than 1.5 GB free ...: waiting` | The disk is nearly full | Free space (the contour files of finished blocks in `<work>/contours/` can go); the build goes on by itself |
| The machine swaps, or the build is killed | Too many workers for the data | `--workers` lower; a province with large neighbors needs most |
| No contour lines in part of the map | The elevation source has no data there, or `.hgt` files that do not cover it | `make-map.py` says how many tiles had no data |
| Names as `?` | Letters outside ASCII | By design of the device's font |
| The map is empty along its border | Built without `--near` | Give the neighbors' extracts |
