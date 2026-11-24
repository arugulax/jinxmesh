# JinxMesh

A keyboard-first interface for Meshtastic on the Elecrow ThinkNode M9.

The M9 has a full keyboard, a d-pad and a 320x240 screen, but no touch. The stock Meshtastic
screen was designed for touch. JinxMesh replaces it with one that is driven entirely by the
keys, and adds offline maps, turn-by-turn navigation and a set of small apps for use in the
field.

JinxMesh is an unofficial hobby project. It is built on Meshtastic firmware 2.8.3 and the
Meshtastic device-ui, is not affiliated with or endorsed by Meshtastic LLC, and comes with no
warranty. Meshtastic is a registered trademark of Meshtastic LLC.

## What you get

- **Everything from the keyboard.** Six tabs (Home, Chats, Nodes, Map, Apps, Settings), a
  control center, a lock screen. OK opens, OK held shows a menu, Back goes back.
- **Chats** with unread counts, drafts, quote replies, reactions, quick replies, mentions,
  search, pinned chats, and a status line that says how far your own message got.
- **Offline vector maps** drawn on the device from files on the SD card: contour lines, water,
  roads and trails, rail, borders and names. Ready-made maps exist for the United States and
  Canada (see [Maps](#maps)). Raster tiles and PMTiles archives work too.
- **Navigate:** a route by road or on foot to a node, a waypoint or typed coordinates, with
  the next turn, a turn list and the route on the map. Routes of several stops can be saved.
- **Apps:** Nearby (who is close, and in which direction), RF monitor, MECP emergency
  messages in plain words, Town locator, Trip odometer, Wardrive signal log, Compass,
  Stopwatch and Timer.
- **Settings on the device:** region, modem preset, channels (with QR sharing), names,
  display, position sharing, Bluetooth, Wi-Fi, sounds.
- **Works next to the phone app.** Bluetooth and the screen run at the same time, and a
  message sent from the phone shows up on the device and the other way around.
- **Files without removing the card:** an upload page over Wi-Fi, or a transfer over the USB
  cable.

## Is it ready?

It is pretty stable, but use it at your own risk. It runs on its author's two M9s, and it
is young. Flashing, the tabs, chats, maps, settings
and Bluetooth have been used on real hardware. A number of features have only been tried in
the simulator so far, and battery life has not been measured. The
[user guide](docs/USER-GUIDE.md) ends with an honest list. Keep a way to flash the stock
firmware back, and expect rough edges.

Only the ThinkNode M9 is supported. The interface is laid out for its screen and keys and is
not offered for other boards.

## Install

1. Download the latest files from the [Releases](../../releases) page.
2. Flash the M9 over USB. A first install erases the device; later updates keep your settings,
   channels and chats.
3. Set your region under Settings, Radio. Until a region is set the radio and the GPS stay off.

[docs/INSTALL.md](docs/INSTALL.md) has the exact commands and what to do when the M9 will not
enter its loader.

## Maps

Vector maps are plain files you copy to the SD card. They are published per region in
separate repositories, because together they are tens of gigabytes:

| Repository | Covers |
|---|---|
| `jinxmesh-maps-us-northeast` | CT, DC, DE, MA, MD, ME, NH, NJ, NY, PA, RI, VT |
| `jinxmesh-maps-us-southeast` | AL, AR, FL, GA, KY, LA, MS, NC, SC, TN, VA, WV, Puerto Rico, US Virgin Islands |
| `jinxmesh-maps-us-midwest` | IA, IL, IN, KS, MI, MN, MO, ND, NE, OH, SD, WI |
| `jinxmesh-maps-us-southwest` | AZ, NM, OK, TX |
| `jinxmesh-maps-us-mountain` | CO, ID, MT, NV, UT, WY |
| `jinxmesh-maps-us-pacific` | CA, HI, OR, WA |
| `jinxmesh-maps-us-alaska` | AK |
| `jinxmesh-maps-ca-east` | New Brunswick, Newfoundland and Labrador, Nova Scotia, Prince Edward Island, Quebec |
| `jinxmesh-maps-ca-central` | Manitoba, Ontario, Saskatchewan |
| `jinxmesh-maps-ca-west` | Alberta, British Columbia |
| `jinxmesh-maps-ca-north-west` | Northwest Territories, Yukon |
| `jinxmesh-maps-ca-north-east` | Nunavut |

Each lives next to this repository under the same owner. Download the pieces for the land
you are in and copy them to `/maps/state/` (or `/maps/province/`) on the card. You do not need
a whole state.

Want a map of somewhere else, or from your own data? One command builds it from an
OpenStreetMap extract: see [docs/MAPS.md](docs/MAPS.md). Raster maps, the town file and the
road file for Navigate are described in [docs/MAP-DATA.md](docs/MAP-DATA.md).

The maps are made from OpenStreetMap data, (c) OpenStreetMap contributors, under the Open
Database License, and from open elevation data. Each maps repository lists its sources.

## Documentation

| Document | For |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Flashing, updating, going back to stock |
| [docs/USER-GUIDE.md](docs/USER-GUIDE.md) | Every key, screen, app and setting |
| [docs/MAPS.md](docs/MAPS.md) | How the vector maps are made, and making your own |
| [docs/MAP-DATA.md](docs/MAP-DATA.md) | Raster map archives, town data, road data |
| [docs/REFERENCE.md](docs/REFERENCE.md) | Card layout, file formats and protocols, for people writing tools |
| [docs/BUILDING.md](docs/BUILDING.md) | Building the firmware and running the simulator |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each release |

## How the repository is laid out

The project is three pieces of source, kept as three branches of this one repository:

| Branch | What it is | Based on |
|---|---|---|
| `main` | This page, the documents, the map tools, the simulator scripts, the logo | nothing |
| `firmware-m9-kb` | The firmware | [meshtastic/firmware](https://github.com/meshtastic/firmware), 2.8.3 |
| `device-ui-kbview` | The interface | [meshtastic/device-ui](https://github.com/meshtastic/device-ui) |

The branches share no history. Built firmware is attached to releases and never committed.

New work happens on development branches and moves to the branches above when it has been
tried on a device and approved:

| Published | Development |
|---|---|
| `main` | `dev` |
| `firmware-m9-kb` | `firmware-dev` |
| `device-ui-kbview` | `device-ui-dev` |

A release is always built from the published branches. The development branches may be
ahead of the latest release, and may be broken.

On `main`:

| Folder | Holds |
|---|---|
| `docs/` | The documents above |
| `maps/` | Tools that build maps and map data for the card |
| `tools/` | Small helpers: a desktop viewer for map files, test generators, a route checker |
| `webflash/` | A browser-based flasher and serial monitor you can host yourself |
| `logo/` | The logo and the script that turns it into the start screen |
| `sim/` and the scripts in the root | Building, running and testing in the simulator |

## Contributing

Bug reports with a description of what you did, what you expected and what happened are
welcome as issues. So are pull requests; please open them against the development branches. Please do not paste serial logs without reading them
first: a Meshtastic log can contain channel keys.

If a problem also exists in stock Meshtastic, it is better reported to the Meshtastic project.

## License and credits

JinxMesh is free software under the GNU General Public License, version 3, like the two
projects it is built on. See [LICENSE](LICENSE).

- [Meshtastic firmware](https://github.com/meshtastic/firmware) and
  [device-ui](https://github.com/meshtastic/device-ui), GPL-3.0, by the Meshtastic project and
  its contributors.
- Map data (c) [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, ODbL.
  Elevation from the Terrain Tiles open dataset.
- Emergency phrases from the MECP project (texts CC BY 4.0, protocol CC0).
- Town outlines from the US Census Bureau.
- Map lettering drawn from Liberation Sans, SIL Open Font License.
- The flasher page uses esptool-js (Apache-2.0).
