# JinxMesh reference

Technical details of JinxMesh on the Elecrow ThinkNode M9, for people who write tools that talk
to the device or who want to know how it works inside. For everyday use see
[USER-GUIDE.md](USER-GUIDE.md); for flashing, [INSTALL.md](INSTALL.md); for building,
[BUILDING.md](BUILDING.md); for making map and data files, [MAPS.md](MAPS.md) and
[MAP-DATA.md](MAP-DATA.md).

Where this document says a thing is untested on hardware, it has only been run in the simulator,
which uses the real firmware but has no radio, keyboard chip, SD card driver or Wi-Fi chip.

## 1. The M9 hardware the interface relies on

| Part | Fact |
|---|---|
| Processor | ESP32-S3, 16 MB flash, 8 MB PSRAM |
| USB | CH340K serial bridge, USB id `1a86:7522` |
| Screen | 320x240, no touch |
| Radio | LR1110. On first start the firmware updates the chip's own firmware, which takes about 8 seconds |
| Keyboard | Its own controller on I2C address `0x6D`, handled by the firmware's driver |
| GPS | AG3352 on GPIO 2 and 3 |
| Sensors | PCF8563 clock at `0x51`, QMI8658 motion sensor at `0x6B`, QMC6309 magnetometer at `0x7C` |
| Buzzer | GPIO 9. There is no vibration motor |
| Keyboard light | Assumed on GPIO 46. Not seen working on hardware |
| SD card | SPI. JinxMesh clocks it at 20 MHz and retries a failed write, read or status check up to three more times |
| Battery | 2300 mAh, 3.8 V nominal, 4.35 V when full |

Notes:

- **I2C address `0x7E` must never be addressed** on the sensor bus. The motion sensor answers
  there as well, and the magnetometer stops answering at `0x7C` until the next reset once
  anything talks to `0x7E`.
- **Battery curve.** Stock firmware uses a table for 4.2 V cells and shows 100 % for roughly
  the first sixth of this cell's capacity. JinxMesh uses a typical curve for a 4.35 V cell:
  100 % at 4.31 V, 50 % at 3.86 V, 0 % at 3.30 V, which is also where it shuts down. The curve
  has not been checked against a measured discharge.
- **Serial log.** The console is 115,200 baud. It can stay silent after a flash until the device
  is reset, and after any client API connection over serial until a reset.

### Key codes

The keyboard controller reports these codes; the interface sees them through the firmware.

| Key | Code |
|---|---|
| d-pad | `0xB4` to `0xB7` |
| OK (Enter) | `0x0D` |
| OK held | `0xA3` |
| Chat | `0x81` |
| Home | `0x82` |
| Preset | `0x83` |
| Location | `0x84` |
| MAP | `0x85` |
| BACK | `0x86` |
| Location held | `0x87` |
| Control center | `0x88` |
| Del | `0x08` |

Any other code from `0x80` to `0xFF` shows a note "Key 0xNN" on the screen.

## 2. Flash layout, updates and full installs

| Partition | Offset | Holds |
|---|---|---|
| `nvs` | `0x9000` | Firmware settings, Bluetooth bonds, the key that seals Wi-Fi passwords |
| `otadata` | `0xe000` | Which app slot starts |
| `app0` | `0x10000` (6.25 MB) | The firmware |
| `app1` | `0x650000` | Second app slot |
| `spiffs` | `0xc90000` | Internal file system: channels, node list, chats, `/kbview.cfg` |
| `coredump` | `0xff0000` | Crash dumps |

- **Update:** write the app image at `0x10000`. `nvs` and the file system are not touched, so
  settings, channels, keys, chats and `/kbview.cfg` are kept. With blank `otadata` the bootloader
  starts `app0`. A device that last started from `app1` after an over-the-air update would keep
  running the old image.
- **Full install:** erase the whole flash, then write the factory image at `0x0`. Everything is
  wiped, including the sealing key, so a stored Wi-Fi password has to be typed again. After a
  full install the region is unset (radio and GPS stay off until one is set), position sharing
  is off on the default channel, and the node has a default name.
- **Version string:** `2.8.3.` followed by the short commit id of the firmware branch. Settings
  > System > About also shows when the build was made, as "Built yyyy-mm-dd hh:mm".
- **Entering the loader:** esptool sometimes fails with "Failed to connect with the device",
  mostly after the M9 has been plugged in for some minutes. Unplugging, plugging in again and
  flashing at once has worked. [INSTALL.md](INSTALL.md) has the commands.

The interface is compiled in with `-D USE_KEYBOARD_VIEW`, in the PlatformIO environments
`thinknode_m9-tft-kb` (the device) and `native-tft-kb` (the simulator).

## 3. SD card layout

FAT32. No file can reach 4 GiB. Leave at least 100 MB free, because the device writes logs and
fetched tiles to the same card.

| Path | Holds | Written by |
|---|---|---|
| `/maps/<style>/<z>/<x>/<y>.png` | Raster map, loose tiles | You, or the device when fetching tiles |
| `/maps/<style>/<style>.pmtiles` | Raster map, one archive | You |
| `/maps/<style>/.url` | Tile URL that allows fetching missing tiles over Wi-Fi | You, or the device |
| `/maps/state/<name>.jmv`, or `<name>-1.jmv`, `<name>-2.jmv`, ... | Vector map of a US state, whole or in pieces | You |
| `/maps/province/...` | The same for Canada | You |
| `/geo/roads.bin` | Road graph for Navigate | You |
| `/geo/towns.bin` | Town boundaries and names | You |
| `/tones/<name>.rtttl` | Your own tunes | You |
| `/apps/routes/<name>.txt` | A saved route of stops | The device, or you |
| `/apps/wardrive/<yyyy-mm-dd>-3.csv` | Wardrive signal log, one file per local day | The device |
| `/apps/trip/<yyyy-mm-dd>.csv` | Track of a running trip | The device |
| `/apps/stopwatch/<yyyy-mm-dd>-<hhmm>.csv` | Saved splits | The device |

Every folder directly below `/maps` is listed as a map style, so put nothing else there. A style
name is the folder name: 1 to 19 characters, not starting with a dot; `A-Z a-z 0-9 _ -` is the
safe set.

For vector maps: a folder shows all its `.jmv` files side by side, up to 240 files. Do not keep
a whole `<name>.jmv` next to its pieces. The `.json` piece lists from the map archive stay off
the card. The `.jmv` format is described in [MAPS.md](MAPS.md).

## 4. Raster maps

A raster style may hold loose tiles, an archive, or both. The archive is looked up first, then
the loose tile. Tiles are 256 pixel PNG.

**PMTiles constraints**

| Rule | Value |
|---|---|
| Path | `/maps/<style>/<style>.pmtiles`: folder and file carry the same name |
| Version | PMTiles 3 |
| Tile type | PNG |
| Tile compression | None |
| Internal compression | gzip (required) |
| Directory depth | At most 4 levels including the root; leaf directories are allowed |
| Zoom | `min_zoom` at least 2, `max_zoom` at most 18 |
| Header | Should carry the bounding box; without a GPS fix the map centers on it |
| Size | Below 4,294,967,296 bytes (FAT32) |

**Behavior**

- A style that is only an archive zooms from the archive's `min_zoom` to its `max_zoom + 2`,
  never above 18. The two extra levels are the deepest tiles drawn at 2x and 4x. Any other
  style zooms from 2 to 18.
- A missing tile is drawn from its parent or grandparent when one exists, otherwise as a "no
  tile" picture.
- When no style has been picked, a style with an archive wins over styles with loose tiles.
- Reading a large archive (about 500 MB) has been tried in the simulator only.

**Fetching over Wi-Fi.** A style whose folder holds a file named `.url` (that exact name) fetches
missing tiles and saves them as loose tiles. Its first line is a URL with `{z}`, `{x}` and `{y}`.
"Fetch missing tiles over Wi-Fi" in the map menu creates the style `osm` with such a file. A
style without the file never fetches. Tiles are fetched only while the map is open and Wi-Fi is
connected. Untested on hardware.

**Changing files while the device runs**

- A new style appears the next time the Map style list is opened.
- New or replaced loose tiles show the next time they are loaded, on a pan or zoom. Failed tiles
  are retried every 10 seconds.
- Replacing the archive of the style in use is not noticed: the open file and its cached
  directory stay, and the map shows wrong tiles until the style is picked again or the device
  restarts. An uploader should say so, or upload under a new style name.

## 5. The upload page

Runs only while "Receive files" is on under Settings > Wi-Fi and files. Plain HTTP, no TLS. With
Wi-Fi up the device also answers to the mDNS name `m9.local`.

| Item | Value |
|---|---|
| Port | 8080 |
| `GET /` or `/index.html` | The browser page; `?c=<code>` prefills the code |
| `GET /check?code=<code>` | 200 `ok`, or 403 |
| `PUT` or `POST /upload?code=<code>&path=<path>` | The body is the file; `Content-Length` is required |
| Listing or download | None |
| Code | Six digits, shown on the screen, new each time receiving is switched on |
| Connections | One at a time; a second one waits. Every reply carries `Connection: close` |
| `Expect: 100-continue` | Answered |

**Replies to an upload:** 200 `saved`; 400 path not allowed; 403 wrong code; 404 or 405 for
other requests; 411 no `Content-Length`; 431 headers above 2,048 bytes; 500 cannot open the file
(card missing, or a folder cannot be made); 507 write failed (card full).

**Path rules.** URL-encoded. A leading slash is added if missing. Refused: `..`, control
characters, any of `\ : * ? " < > |`, a trailing slash, and 200 characters or more. Spaces are
allowed. Missing folders are created at every level.

**Other behavior**

- After 20 wrong codes the page accepts nothing until receiving is switched on again.
- Timeouts: 10 seconds for the request headers, 15 seconds without body data.
- No size limit of its own; the card's free space and FAT32 limit apply.
- An existing file is overwritten without asking. An upload that breaks off deletes the partial
  file, so a failed overwrite also loses the old file.
- The server stores exactly the path it is given. Only the browser page's own script turns a
  `.pmtiles` file dropped on `/maps` into `/maps/<name>/<name>.pmtiles`; another client sends the
  full path itself.

Example:

```
curl -T roads.bin "http://m9.local:8080/upload?code=123456&path=/geo/roads.bin"
```

Measured speed on hardware is about 300 kB a second.

## 6. Files over the USB serial console

For a card that cannot be taken out and where Wi-Fi is not an option. The implementation and the
authoritative description are in `src/jinx/JinxFiles.cpp` on the firmware branch.

The host writes the line `JXFILES1\n` to the console at 115,200 baud. From the answer on, both
sides speak in frames until the host says Exit or stays silent for 30 seconds:

```
0xA5 0x4A | type (1) | sequence (1) | length (2, little endian) | payload | CRC-32 (4, little endian)
```

The CRC is the zlib one and covers type through payload. The device answers every frame it could
read with Ack (`0x80`) or Error (`0x81`, a text), carrying the same sequence number. A frame with
a wrong CRC gets no answer and the host sends it again. Log output between frames is not framed
and the host skips it. While a transfer runs, the console answers no client API frames.

| Type | Request | Payload | Ack carries |
|---|---|---|---|
| 1 | Begin | u64 size, u8 flags (1: continue what is there), path | u64 offset to continue from |
| 2 | Data | u64 offset, bytes | u64 offset after them |
| 3 | End | | nothing; the file now has its name |
| 4 | Stat | path | u8 kind (0 none, 1 file, 2 folder), u64 size |
| 5 | Delete | path | |
| 6 | Baud | u32 | nothing; then the speed changes |
| 7 | Exit | | nothing; then the console is back |
| 8 | List | path | lines `d name` or `f size name` |
| 9 | Crc | path | u32 CRC-32, u64 size |
| 10 | Tone | a tune as RTTTL text | u8, 1 when it sounds |
| 11 | Read | u64 offset, path | up to 4,000 bytes from there; none means the end |

- Data that does not start where the file ends is answered with Offset (`0x82`, u64 where to go
  on) instead of Ack.
- The host may send two Data frames ahead of the answers.
- A file is written as `<path>.part` and renamed at End, so a broken transfer leaves no half
  file under the real name and can be continued with Begin and flag 1.
- Baud accepts 9,600 to 3,000,000. On hardware, 921,600 baud with two frames in flight moved
  about 58 kB a second.

## 7. Tunes

`/tones/<name>.rtttl`, one tune per file as RTTTL text, for example
`name:d=4,o=5,b=120:c,e,g`. The file name without its ending is the name shown in the lists under
Settings > Sounds. A tune may be at most 399 characters, and at most 230 if it is to be used as
the "Notification tone", which the radio stores itself. Up to 40 of your own tunes are listed.

The Tone request of the serial protocol plays a tune once without storing it.

## 8. Bluetooth: the JinxMesh service

Besides the standard Meshtastic service, the device offers a service of its own. It uses the
same pairing as the mesh service, and it does not go through the phone API, so using it takes no
messages away from the screen. The device accepts one Bluetooth connection at a time: a phone app
has to disconnect before a browser or tool can connect. Source: `src/jinx/JinxBle.cpp` on the
firmware branch.

| What | UUID |
|---|---|
| Advertised service (the Meshtastic service; filter on this) | `6ba1b218-15a8-461f-9fa8-5dcae273eafd` |
| JinxMesh service | `9971f534-e514-4da7-81ce-e011c4e47254` |
| Inbox characteristic (read, write) | `5d0b7c7e-2f0a-4c55-9d53-6a0c1f1b7e21` |
| Files characteristic (read, write) | `c4f2a44c-dece-403d-851b-8f4ee0c4b4ad` |
| Offer characteristic (read, notify) | `9c6efe0e-fe07-414d-9ee1-e7b101e01d9f` |

A device without the JinxMesh service or the inbox characteristic runs firmware that is too old.

### The inbox: destinations and routes for Navigate

The inbox has only been run in the simulator so far.

- One write is one line of text: UTF-8, no line end, at most 150 bytes. Longer writes are
  dropped. Use write with response.
- The device holds up to 39 lines and works through them a few at a time. After a write, read
  the characteristic: byte 0 is the number of lines that still fit. Below 5, wait 300 ms and
  read again before the next write. A write that arrives when nothing fits is dropped silently.
- Commas separate fields, so a name must not contain one.
- Latitude and longitude are decimal degrees with a point, south and west negative. Names are
  cut to 24 characters.

| Line | Meaning |
|---|---|
| `D lat,lon,m,name` | Go to this one place now. `m` is `c` (by road, driving) or `f` (on foot) |
| `R name` | A route of stops begins; stops sent before are forgotten. An empty name means it is not saved |
| `S lat,lon,name` | The next stop, in order. At most 1,024 |
| `G m` | End of the stops: save if named, open Navigate, start. `m` as above |
| `K` | End of the stops: save only. The route is then under Navigate, "A saved route" |

Example: `S 44.1970,-72.5020,Barre`.

After `G`, a device within 50 m of stop 1 leads on to stop 2 at once. Anywhere else it asks on
its screen whether to navigate to the starting point or join the route from where it is. The
device finds its own way between stops from `/geo/roads.bin`, so its line can differ from the
one a planner drew.

A Meshtastic waypoint sent by any client is also listed under Navigate, "A waypoint", and can be
a destination. That needs nothing from this service.

### Files: reading `/apps` from the card

Read only, and only paths below `/apps/` (the wardrive and trip logs, splits, routes).

- **Write:** a u32 offset (little endian) followed by the path. A path ending in `/` asks for
  the list of that folder, as text lines `d name` or `f bytes name`.
- **Read:** byte 0, the u32 offset this piece starts at, and up to 480 bytes; no bytes means
  that was all. Or byte 1 and an error text.
- Nothing is kept between pieces, so each write and read stands alone.

The **offer** characteristic notifies a path (or a folder ending in `/`) when the person chooses
"Send the log by Bluetooth" or "Send the track by Bluetooth" on the device. Whoever listens then
fetches it as above.

## 9. Routes on the card

`/apps/routes/<name>.txt`, plain text, one stop per line:

```
44.260100,-72.575400,Start
44.197000,-72.502000,Barre
```

- `lat,lon,name` in decimal degrees. The name is optional and cut to 24 characters; a stop
  without one is called "Stop n".
- Empty lines and lines starting with `#` are skipped, as are lines whose coordinates cannot be
  read.
- At most 1,024 stops; further lines are ignored. The device lists up to 40 route files.
- The device writes the same format when a route is saved in the planner, so files can be edited
  on a computer and copied back.

## 10. CSV files the device writes

All three have a header line. The day in a file name is the device's local day; `time_utc` is
UTC, as `2026-11-19T17:03:22Z`.

### Wardrive: `/apps/wardrive/<yyyy-mm-dd>-3.csv`

The `-3` names this set of columns. One row per packet heard without a relay (at most one per
node every 5 seconds, and no more than four rows a second in total), one after each quiet
minute, and one for each ping, start and mark. Text columns are quoted.

| Column | Content |
|---|---|
| `time_utc` | When the row was written |
| `event` | `heard`, `quiet`, `ping`, `ping_not_sent`, `start` or `mark` |
| `mode` | `passive` (listen only) or `active` (pings are on) |
| `lat`, `lon`, `alt_m` | Own position, 6 decimal places; empty without a fix |
| `fix_age_s` | Seconds since that fix, when known |
| `node` | Sender id as `!` and eight hex digits; empty on rows without a node |
| `short_name`, `long_name` | Of the sender |
| `role` | The sender's role in words, such as `client` or `router` |
| `hw_model` | The sender's hardware model number |
| `rssi_dbm`, `snr_db` | Of the packet |
| `channel` | Channel slot the sender's node is known on |
| `node_lat`, `node_lon`, `node_alt_m` | Where the sender says it is |
| `distance_m`, `bearing_deg` | From here to there, when both positions are known |
| `node_battery_pct`, `node_voltage` | From the sender's device metrics |
| `node_ch_util_pct`, `node_air_util_tx_pct` | From the sender's device metrics |
| `via_mqtt`, `has_key` | 1 or 0 |
| `since_ping_s` | Seconds since this device last pinged; empty if it never did |
| `ch_util_pct`, `air_util_tx_pct` | Own channel utilization and transmit airtime |
| `preset` | Number of the modem preset in use, -1 for custom values |
| `slot` | Frequency slot in use |
| `town` | Own town, when town data is on the card |
| `mark` | Number of the mark, on `mark` rows |

Logging stops by itself below 50 MB free, or after three failed writes in a row.

### Trip: `/apps/trip/<yyyy-mm-dd>.csv`

Columns `time_utc,lat,lon,alt_m,speed_mps,trip_m`. One row per fix that counted toward a running
trip, at most one in five seconds.

### Stopwatch: `/apps/stopwatch/<yyyy-mm-dd>-<hhmm>.csv`

Columns `split,lap_s,total_s,clock,lat,lon,trip_m`: the split number, lap and total time in
seconds, the clock time, and the position and trip distance at that moment when available.

## 11. `/kbview.cfg`

The interface keeps its own settings in `/kbview.cfg` on the internal file system (not on the
card), as `key=value` lines. An update keeps the file; a full install wipes it. At most 8,192
bytes are read. It is written to a temporary file and renamed, so a failed write keeps the old
settings. Everything else (region, channels, names, Bluetooth, and so on) is ordinary Meshtastic
configuration held by the firmware.

| Key | Value |
|---|---|
| `keyLight` | Keyboard light: 0 off, 1 on, 2 automatic |
| `keyFlash` | 1: flash the keys for a message |
| `soundMode` | The radio's buzzer mode to return to when sound is switched back on |
| `wake` | 1: wake the screen for a message |
| `lockOnSleep` | 1: lock when the screen sleeps |
| `screenSound` | 1: a sound when the screen goes on or off |
| `light` | 1: the light look |
| `longNames` | 1: chats name senders by long name |
| `borderWarn` | 1: warn near a country's border |
| `breadcrumbs` | 1: record and show the own trail |
| `tileDownload` | 1: fetch missing map tiles over Wi-Fi |
| `mapStyle` | Name of the map style in use |
| `mapLayersOff` | Bits for vector map layers switched off: contours, water, roads, trails, boundaries, names |
| `mapview` | `all`, `fav`, or `#` and a channel's name |
| `wifi` | 1: Wi-Fi on |
| `ssid` | Network name |
| `wifisecret` | The sealed Wi-Fi password (section 12) |
| `net` | One line per saved network: the sealed password, a space, the network name. Up to 8 |
| `toneDirect`, `toneEmergency`, `toneTimer` | Tune names; empty means the default |
| `toneChannel0` ... | Tune name per channel slot |
| `quickOwn`, `quick` | 1 once the quick replies were changed, then one `quick=` line per reply (up to 16, 60 characters each) |
| `tilecolors` | Per Home tile kind, a color index, comma separated |
| `nodecolor` | One line per colored node: `!` and eight hex digits, `:`, a color index. Up to 64 |
| `owned` | One line per node marked "One of my nodes": `!` and eight hex digits. Up to 32 |
| `home` | Twelve tile names in the order of the places, left to right and top to bottom |
| `pin` | One line per pinned chat, in order (up to 16): `#` and a channel's name, `=` and the slot digit of a channel without a name, or `!` and a node id in hex |
| `wp` | One line per waypoint (up to 32): `id,from,latitude,longitude,expire,name`, coordinates in 1e-7 degrees |
| `nearby` | `list` or `vectors`: the view Nearby opens with |
| `callsign` | Call sign for emergency messages |
| `trip` | `running,meters,elapsed seconds,epoch the running stretch began,moving seconds,fastest stretch in mm/s` |
| `odometer` | Lifetime meters |
| `wardrive` | 1 while the signal log is on |
| `wardrivePing` | Seconds between pings; 0 listens only |
| `wardriveChannel` | Channel slot for pings |
| `declination` | Typed-in declination in degrees, east positive, -180 to 180 |
| `declinationAuto` | 1: take the declination from the GPS position |
| `compassTurn` | 0, 90, 180 or 270 |
| `timer`, `timerRepeat` | Timer length in seconds; 1 to repeat |

Tile names for `home`: `time`, `unread`, `radio`, `gps`, `battery`, `nodes`, `nearby`, `airtime`,
`town`, `trip`, `mecp`, `compass`, `wardrive`, `stopwatch`, `timer`, `navigate`, `empty`.

A pinned channel is kept by name because deleting a channel moves the later ones down a slot.

## 12. How secrets are stored

- **Wi-Fi passwords** are sealed in `/kbview.cfg` with ChaCha20-Poly1305. A sealed value is `v1:`
  and 186 hex digits: a 12-byte nonce, a 65-byte block (a length byte and the password padded to
  64 bytes) and a 16-byte tag. The length of the line says nothing about the length of the
  password.
- **The sealing key** is 32 random bytes the device makes at first use and keeps in NVS
  (namespace `kbview`, entry `seal`), outside the file system. The settings file alone, a copy of
  it, or a file moved to another device does not give the password. When the key is gone (after
  a full install) the Password row says "Type it again".
- **The Wi-Fi driver's own storage** of network name and password is turned off.
- **On the screen,** a stored password is never put back into an editor or onto a page.
- **Bluetooth PINs.** JinxMesh sets one-time PINs, so no PIN is stored. The firmware draws a new
  PIN for every pairing and hands it only to the screen, as a client notification whose text is
  `pairing:` and six digits (`pairing:` alone when the pairing has ended). The screen shows it
  for at most 60 seconds and keeps it nowhere.
- **Logs.** No log line of the M9 build carries a Wi-Fi password, a pairing PIN or the upload
  code. The build lowers the Arduino core's log level because its Bluetooth library would
  otherwise print the passkey.

**What this does not protect against.** The flash chip is not encrypted. Someone who can read
the whole chip gets the sealing key together with the sealed password. The same is true of what
the firmware itself keeps in clear there: channel keys, the node's private key, the bonding keys
of paired phones, and a Wi-Fi password or fixed PIN set through a phone app. Only ESP32 flash
encryption would close that; it burns eFuses, cannot be undone, changes how the device is
flashed, and is not used. The upload page is plain HTTP with a six-digit code and is meant for a
network you trust. The lock screen has no code.

## 13. `towns.bin` and `roads.bin`

Both are built on a computer; [MAP-DATA.md](MAP-DATA.md) has the builders, the sources and the
full `towns.bin` field tables. Neither file is ever loaded whole on the device.

### `/geo/towns.bin`

Format version 1, little endian, magic `KBTN`. The device refuses other versions and says so.

The file is a 64-byte header (accuracy in meters, the box of all records, the grid's cell size
and dimensions, the record count and the offsets of the parts), a grid of cells that list the
records touching them, 40-byte records (box, label point, name, county and state, rings, kind),
a name index sorted for search, shared strings, and the boundary rings as a first point plus
int16 steps in 1e-5 degrees. The exact field tables are in [MAP-DATA.md](MAP-DATA.md).

A point is looked up through its grid cell and an even-odd crossing test over the rings of each
candidate record. Within the header's accuracy of another town's line, the device says "Near the
... line".

### `/geo/roads.bin`

Format version 1, little endian, magic `JMRD`. Coordinates are int32 in 1e-7 degrees unless
noted.

- **Header, 68 bytes:** magic, u16 version, u16 zero, min lat, min lon, max lat, max lon, u32
  cell size (1e-7 degrees; cells are 0.01 degrees), u32 columns, u32 rows, u32 vertex count, u32
  directed edge count, u32 offsets of cells, vertices, edges, shapes and names, u32 file size.
- **Cells:** per cell, row by row from the south-west: u32 first vertex, u32 count.
- **Vertices,** plus one closing record: i32 lat, i32 lon, u32 first edge.
- **Edges,** grouped by source vertex: u32 target, u32 shape offset, u32 meta, u32 name offset.
  Meta holds the length in meters (bits 0 to 19), the class (bits 20 to 23), car allowed
  (bit 24), foot allowed (bit 25), and "shape is stored from the target" (bit 26).
- **Shapes:** u16 n, then n pairs of i16 dlat, dlon in 1e-6 degrees, each from the point before;
  the first from the vertex the shape starts at. The end vertex is not included.
- **Names:** zero-terminated UTF-8; offset 0 is the empty name.

Classes, in order from 0: motorway, trunk, primary, secondary, tertiary, unclassified,
residential, service, track, path, cycleway, steps, ferry. Vertices are sorted by cell in Z order
so that what is near on the ground is near in the file; the device reads 4 kB blocks and caches
256 of them. The reference for the layout is the head of `maps/roads.py` and
`include/util/RouteGraph.h` in the interface's source.

The data comes from OpenStreetMap (ODbL 1.0). A `roads.bin` is a derived database: whoever
passes one on says so and offers it under the same license.

## 14. Known limits by design

- **One layout.** 320x240 only. The interface is not offered for other boards.
- **Language.** English only, dates as yyyy-mm-dd. Map and town names outside the screen font's
  Latin letters do not display.
- **Not in the interface.** The firmware's module settings (MQTT, telemetry, serial and the
  rest), security keys, and remote administration of other nodes. Use a phone app or the
  command line.
- **Channel views** show nodes heard on a channel, not its members. The radio has no member
  list.
- **Position sharing** is broadcast only on the lowest-numbered channel that shares; other
  channels answer requests. Channels with the default key are limited to about 700 m. There is
  no setting per direct chat.
- **Routing.** No turn restrictions, speed limits, seasonal closures or surface. Times are
  estimates. Very long routes can be refused as too far to work out.
- **Trip** counts from fix to fix, about once a minute, so a winding path reads short.
- **Replaced archive.** The map does not notice that its open PMTiles archive was replaced.
- **Online tiles.** If Wi-Fi drops in the middle of a tile fetch, the screen waits until that
  request gives up.
- **Large folders.** Opening a folder with thousands of files in the card browser holds the
  card's bus for the whole listing.
- **Stopwatch.** Wraps around after 49 days. A restart ends it.
- **Timer.** A sleeping device may notice late that a timer is nearly due; it stays awake only
  for the timer's last three minutes.
- **Upload page.** Plain HTTP, no listing, no download.
- **Flash encryption is off** (section 12).
