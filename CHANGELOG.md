# Changelog

What changed in JinxMesh, newest first. Builds are named by their date and time, which the
device shows under Settings, System, About. The version string of a build is the Meshtastic
version followed by the JinxMesh firmware commit.

## Unreleased

Map tools:

- A map over 4 GB can be built: it is written with 64-bit offsets for `split.py` to cut into
  ordinary pieces. British Columbia and the territories needed this.
- Contour lines are made in parallel even when memory allows only one block to be built at a
  time, and elevation tiles are removed as the build moves east, so the far north no longer
  takes days or fills the disk.
- A build that was stopped goes on from the blocks it had finished.
- A state that crosses the 180th meridian (Alaska) builds.
- Elevation tiles are retried for about an hour, so a network outage no longer ends a build.
- The link can be spared: `M9_UPLOAD_KBPS` limits what `publish.py` sends, `M9_DOWNLOAD_KBPS`
  what the build takes for elevation tiles, both in kB a second.

## 2026-11-24-1246 (2.8.3.9485eff)

### Added
- Bluetooth and the screen work at the same time. Stock firmware stops feeding the screen
  while Bluetooth is on; the screen now has its own packet queues.
- Text messages and waypoints sent from a paired phone show on the device, and those sent
  from the device show on the phone.
- Vector maps: maps drawn on the device from `.jmv` files on the card, with contour lines,
  water, roads and trails, rail, borders and names. A state or province can be split into
  pieces, and only the pieces you need have to be on the card.
- Navigate: a destination by typed coordinates or from a waypoint; routes of up to 1,024
  stops, saved by name on the card; the whole route and the next turn on the map; a prompt
  to navigate to the first stop when you start away from it.
- A Bluetooth inbox that takes destinations and routes from another device.
- The build date and time under Settings, System, About.
- A memory line in the serial log every 30 seconds.

### Changed
- Turning Bluetooth on no longer takes the screen over. The pairing screen comes up when a
  phone asks for a PIN.
- Settings: Radio moved from the top of the list to just above System, so that a slip of
  the OK key no longer lands in it. On the Radio page, Region and Modem preset are now last.
- Frequency slot: the list starts with the public slot of the preset, by name. "Automatic"
  is now called "By channel name", which is what it does.
- Small memory allocations go to external RAM, which leaves about 50 kB of internal memory
  free with Bluetooth on, where there had been under 12 kB.

### Fixed
- Picking the modem preset, bandwidth or region a device already had reset its frequency
  slot. With a primary channel of a custom name that moved the device to another frequency,
  where it heard nothing.
- The task that saves chat messages could fail to start when memory was short, and then
  flooded the log. It now tries again once in ten seconds.

## 2026-11-20-1454 (2.8.3.a2c00a4)

### Added
- Files for the SD card over the USB cable: send, list, delete, read back and compare,
  without taking the card out.
- A tune per kind of notification (channel message, direct message, emergency message,
  timer), including your own tunes from `/tones` on the card.
- A setting for whether chats name people by their short or their long name.

### Fixed
- Large uploads over Wi-Fi failed after a few megabytes with a card write error. Failed card
  writes are now retried, and the card runs at a clock rate within its specification.

## 2026-11-20-0734 (2.8.3.d210c21)

### Added
- Navigate: a route by road, or on foot over roads and trails, to a node's last reported
  position, turn by turn, with the route on the map. Needs `/geo/roads.bin` on the card.
- A magnetic declination table, so the compass can show true north without typing a value.

## 2026-11-19-2300 (2.8.3.faf4443)

### Added
- Apps: MECP emergency messages in plain words, Town locator, Trip odometer, Wardrive signal
  log, Compass, Stopwatch and Timer.
- Twelve Home tiles that can be arranged, pinned chats, map views by channel, dark and light
  looks, colors for tiles and nodes.

### Changed
- Power: the processor goes back to sleep sooner behind a dark screen, and a radio packet no
  longer counts as a key press.

### Fixed
- Thirty defects of state, bounds and sleep found in a review of the whole code, among them a
  keyboard light that had stopped working.

## 2026-11-19-1727 (2.8.3.0f55cc0)

### Added
- Nearby: nodes by distance and bearing, as a list, as lines around your own position, and
  with a pointer page.
- RF monitor: channel use and own airtime with a ten-minute history, the radio's counters,
  who was heard directly.
- Trace route shows its result, or why it failed, in a window that stays.

### Security
- A Wi-Fi password typed on the device is stored sealed (ChaCha20-Poly1305, key outside the
  file system), typed behind stars and never shown again.
- Bluetooth turned on from the screen uses a new PIN for every pairing. The PIN is shown
  only while a phone pairs, and is written to no log.

## 2026-11-19-1500 (2.8.3.d1b5147)

First build: the keyboard-driven interface with Home, Chats, Nodes, Map and Settings, a
control center, a lock screen, raster maps from the SD card, and an upload page over Wi-Fi.
