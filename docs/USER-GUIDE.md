# JinxMesh user guide

JinxMesh is a keyboard-driven interface for Meshtastic firmware 2.8.3 on the Elecrow ThinkNode
M9. It replaces the stock touch interface with one that is worked entirely from the keys and the
d-pad. This guide is for someone who owns an M9 and has just flashed JinxMesh onto it.

JinxMesh is an unofficial hobby project; Meshtastic is a trademark of Meshtastic LLC, and this
project is not affiliated with or endorsed by it.

Related documents: [INSTALL.md](INSTALL.md) for flashing, [MAPS.md](MAPS.md) for building vector
maps, [REFERENCE.md](REFERENCE.md) for file formats and protocols, and the
[README](../README.md) for an overview.

## 1. First start

After a full install the device has no region set. Until you set one, the radio and the GPS stay
off: you see no nodes, no position, and nothing you send goes anywhere. The Radio tile on Home
says "No region".

1. Go to the Settings tab. While no region is set, **Radio** is the first row and says "Set the
   region".
2. Open it and choose **Region**. Pick where you are; this sets the legal frequency band. The
   radio restarts.
3. Under **Settings > Device**, give the device a long name and a short name (up to four
   letters). Renaming restarts the radio after a few seconds.

Other things worth knowing on day one:

- Position sharing is off on the default channel after a full install. Turn it on under
  **Settings > Position** if you want others to see you.
- On its very first start the firmware updates the radio chip's own firmware. This takes about
  8 seconds. Leave the device powered.
- A later update keeps your settings, channels and chats. A full install wipes everything.

## 2. The keys

| Key | Does |
|---|---|
| d-pad | Move |
| OK (Enter) | Open, choose, send |
| OK held | The menu for whatever has the focus |
| BACK | One step back; closes a popup |
| Home | The Home tab. Pressed on Home with nothing open, it turns the screen off |
| Chat | The Chats tab |
| Preset | In a chat: quick replies. Elsewhere: mentions and unread messages |
| Location | Sends a position ping ("Ping sent") |
| Location held | GPS on or off |
| MAP | The Map tab. On the map, switches between moving the map and the tab bar |
| Del | Backspace |
| CTRL | The control center, from anywhere; again closes it |
| `@` | The list of mentions and unread messages |

A key the interface has no use for shows a small note, "Key 0xNN", with the code it sends.

## 3. Moving around

**Tabs.** There are six: Home, Chats, Nodes, Map, Apps, Settings. On the tab bar, Left and Right
change the tab and Up or OK goes back into the screen. In lists, Left and Right move to the
neighboring tab directly. From the Home tiles, Down from the bottom row reaches the tab bar.

**The rule for OK.** OK does the obvious thing: open, choose, send. Holding OK opens a menu of
everything else you can do with the item in focus. If you are looking for a function, hold OK.

**Popups.**

- List: Up and Down move (and wrap around), Right or OK chooses, Left or BACK closes. Typing a
  letter jumps to the next entry that starts with it. Home closes the popup and goes Home.
- Text field: type, Left and Right move the caret, Del deletes, OK applies, BACK cancels.
- Info page: Up and Down scroll.

**Top bar.** It shows the device's name, battery or power, the clock, and small symbols for
Bluetooth, Wi-Fi (dim while it connects) and GPS (dim while there is no fix). Marks appear for
muted sound and for a running Wardrive log ("Log").

**Apps run on top of Home.** BACK inside an app returns to where you opened it from. Trip,
Wardrive, Stopwatch and Timer keep running when you leave their screens, with the screen dark,
and behind the lock.

## 4. Home and its tiles

Home is a grid of twelve tiles, four wide and three high. Each tile shows a live value and opens
its page with OK. The default layout is Time, Unread, Radio, GPS, Battery, Nodes, Nearby, Town,
MECP, Compass, Trip, Timer.

Available tiles: Time, Unread, Radio, GPS, Battery, Nodes, Nearby (the closest node), Airtime
(channel use and your own transmit share), Town, Trip, MECP, Compass, Wardrive, Stopwatch, Timer,
Navigate, and Empty for a place you want left free. Apart from Empty, no tile can be on Home
twice.

What OK opens: Time opens the time zone, Unread the unread list, Radio the region, GPS the map,
Battery this device's details, Nodes the node list. An app's tile opens the app. An empty place
opens the Home manager.

**OK held on a tile** offers:

- **Color of ...**: a border in one of eight colors, with the tile in a shade of it.
- **Arrange tiles**: the Home manager.
- **Apps**: the Apps tab.

**Home manager ("Arrange tiles").**

- Arrows move over the twelve places. Below the last row is "Default layout".
- OK on a place opens the catalog; the tile you pick goes there, swapping places if it is on
  Home already.
- OK held on a place picks the tile up. The arrows carry it; OK, OK held or BACK puts it down.

Every change is saved at once. You can also add or remove an app's tile from the Apps tab: hold OK on the app and choose "Add to
Home" or "Remove from Home". On a full Home it asks which tile should make room.

## 5. Chats

**The list**

- Up and Down move; OK opens the chat.
- Typing searches all messages. `@` opens mentions and unread messages.
- OK held on a channel: Mark as read, Pin to top, Quick reply, Mute, Position (precision), Show
  on the map (only the nodes heard on this channel), Channel settings, Search messages, Delete
  the messages.
- OK held on a direct chat: Mark as read, Pin to top, Quick reply, Mute, Node details, Search
  messages, Delete the chat.

**Pinned chats** lead the list in the order you put them. New messages do not reorder them. A
pinned chat offers Unpin, Move up and Move down in its menu, and the menu stays open so you can
move a chat several places. You can pin up to 16 chats. Below the pins, chats with unread
messages come first, then the most recent.

**Inside a chat**

- Type to write. Left and Right move the caret. OK sends.
- Up and Down pick a message. OK on a picked message quotes it in your next message. On an empty
  draft, Del removes a pending quote.
- OK held on a picked message: Reply, React, Details.
- OK held in the composer (or the Preset key): quick replies, sent right away; your draft stays.
  The same list ends with "Emergency message (MECP)" and "Change this list".
- Drafts are kept per chat, up to 200 bytes. Each chat keeps 1,000 messages across restarts.

**How far your message got.** Under your own messages a short status shows: "sending", "sent to
mesh" (another node relayed it), "received" (a direct message the other radio confirmed), "sent
to mesh, not confirmed", "failed to send to mesh", or "failed:" with the radio's reason (no such
channel, too long, airtime limit, key not known, and so on). The message details spell it out.

## 6. Nodes

- Up and Down move. Typing filters by name; Del edits the filter.
- OK opens the details: when it was last heard, hops, signal, battery, position, the channels it
  was heard on.
- OK held opens the node menu: Message, Details, Show on map, Arrow to last reported position,
  Navigate to last reported position, Trace route, Ping, Add to favorites, Mute, Signal history,
  Color, One of my nodes, Ignore, Remove.

**Color** sets the color of the node's chip in lists and its mark on the map. **One of my
nodes** puts a green ring around both, so your own devices stand out.

**Trace route** opens a popup that stays up and reports what was sent, how far it got, the route,
or why it failed. You can leave it with BACK; the answer shows when it comes.

## 7. Map

**Keys**

| Key | Does |
|---|---|
| Arrows | Pan (this leaves follow mode) |
| `+`, `=` or `i` | Zoom in |
| `-` or `o` | Zoom out |
| OK | Follow your own position |
| `n` | Jump to the next node that has a position |
| `v` | Next view: all nodes, favorites, then each channel |
| OK held | The map menu |
| MAP | Switch between moving the map and the tab bar |

The chip at the top left shows the zoom, "stretched 2x" or "4x" when the picture is enlarged from
a lower zoom level, Follow or Free, and Online while tiles are being fetched. The chip at the top
right shows the node you are looking at with its distance, or how many nodes are on the map.

**The map menu (OK held):** the target node, Show (the view), Map style, Layers, Drop a waypoint
here, Waypoints, Show or hide my trail, Clear my trail, Fetch missing tiles over Wi-Fi.

**Map styles.** Every folder below `/maps` on the SD card is a map style, and Map style lists
them. There are two kinds:

- **Vector maps** (`.jmv` files) are drawn on the device: contour lines, water, roads and trails,
  rails, borders and names. **Layers** in the map menu switches Contours, Water, Roads, Trails,
  Boundaries and Names on or off. Copy the pieces for the land you are in to `/maps/state/` or
  `/maps/province/`. [MAPS.md](MAPS.md) explains where maps come from and how to build your own.
- **Raster maps** are ordinary PNG tiles, loose or packed into one PMTiles archive per style.
  [MAP-DATA.md](MAP-DATA.md) explains how to make one.

Without a card or a map the screen says so; nodes, waypoints and your trail still work on a blank
background.

**Views.** A view limits which nodes the map shows: all nodes, your favorites, or the nodes heard
on one channel. The radio keeps no member list for a channel, so a channel view shows nodes that
have been *heard* on that channel. Someone who has the channel and has sent nothing does not
appear until they do. Your own position, trail and waypoints are in every view.

**Waypoints.** "Drop a waypoint here" asks for a name, then whether to keep it on this device or
share it on a channel. Waypoints sent by others, including from a paired phone, are added to the
list. From the Waypoints list you can show one on the map, navigate to it, or delete it. The
device keeps up to 32.

**Trail.** Your own track is drawn as you move. You can hide it (which also pauses it) or clear
it.

**Border warning.** With vector maps in use, the device tells you when a country's border is
500, 250 and 100 m away and when you have crossed it. Switch it under Settings > Sounds, "Warn
near a country's border".

## 8. Apps

The Apps tab lists the apps, each with a line about what it is doing right now. Up and Down
move, a letter jumps to the next app starting with it, OK opens.

### Navigate

Navigate finds a route by road, or on foot over roads and trails, and guides you turn by turn. It
needs road data for your area as `/geo/roads.bin` on the card and a GPS fix. Without the file the
screen tells you where it goes.

**Choosing where to go**

- **A node:** hold OK on it anywhere, then "Navigate to last reported position". You are offered
  a straight line (the arrow), by road, or on foot.
- **Anything else:** press OK in Navigate while no route is active. The choices are Coordinates,
  A waypoint, A saved route, and Plan a route of several stops.
- **Coordinates** are typed as decimal degrees, latitude first, for example `44.2601, -72.5754`
  or `44.2601 N 72.5754 W`.

**While a route is active**

- The screen shows the next turn with its distance and the road it leads onto, then what is left
  and an estimated time.
- OK switches between the next turn and the list of all turns.
- The route is drawn as a line on the map.
- If you stay off the route for 8 seconds (70 m by car, 40 m on foot), a new route is found.
- OK held: Show on the map, Route again, Switch between driving and on foot, Straight line,
  Next stop, Back to the stop before, Somewhere else, End navigation.
- BACK leaves the screen; the route stays until you end it.

**Routes of several stops.** A route can have up to 1,024 stops. In the planner you add stops by
coordinates, from a waypoint, or "where I am", name them, reorder or remove them, and save the
route by name on the card. On arrival at a stop the device names the next one and carries on. If
you start a route more than 50 m from its first stop, it asks whether to go to the starting point
or join the route from where you are. Saved routes are plain text files, so you can also write
them on a computer (see [REFERENCE.md](REFERENCE.md)).

Routes are worked out on the device and are estimates: the road data has no turn restrictions,
speed limits or seasonal closures. Use your own judgment.

### Nearby

Who is close, how far, and which way.

- The list shows every node with a known position, nearest first, with bearing, distance, age,
  and the signal of nodes heard directly. Typing filters by name.
- A position shared with reduced precision shows its radius ("±") and is never shown closer than
  that.
- Right switches to the line view: your device in the middle, north up, a line to each of the
  nearest 24 nodes, and a ring whose distance is named at the lower right. Left goes back to the
  list. The line view needs your own GPS fix.
- OK opens the pointer page for a node: an arrow on a ring, distance, bearing and age. OK held
  opens the node menu.
- Without a GPS fix the list is sorted by last heard and shows no distances.

"fix 5 min" is the age of the position itself. A bare "5 min" is when the node was last heard;
after a restart all ages are of that kind until the node reports again.

### RF monitor

- Two gauges at the top: "Channel" is channel utilization, "Own TX" is the share of the last hour
  your radio transmitted. Each has ten minutes of history.
- Below: the radio's counters (packets sent, received, bad, duplicates, relayed, dropped), noise
  floor, nodes online, uptime, free memory.
- Then the last 20 packets heard directly, newest first, with time, RSSI and SNR.
- OK on a counter asks the radio for fresh numbers. OK on a heard node opens its menu.

### MECP (emergency messages)

MECP is an open format for short emergency messages that any mesh client can carry. JinxMesh
lets you write one by picking plain-English phrases, and shows incoming ones as cards instead of
raw codes. Open it from Apps, or in a chat with OK held in the composer.

Writing takes four steps. BACK or Left goes one step back and keeps everything.

1. **How serious:** Routine, Safety, Urgent, or Mayday. The last row switches "This is a drill".
2. **What:** 109 phrases under their categories. Left and Right jump a category. Typing searches
   phrases, categories and everyday words such as "hurt" or "stuck". OK picks or unpicks, up to
   eight. OK held, or the row "Continue with n picked", goes on.
3. **Details,** all optional: number of people, your position, the time, a call sign (remembered
   for next time), a reference, how long until you arrive, and a note. The top line counts bytes
   out of 200; only the note is ever cut.
4. **Check and send:** the card as a receiver sees it and the exact line that goes on air. "To"
   starts on the public channel, or on the chat you came from. For Urgent and Mayday you must
   hold OK to send, so a slip cannot raise an alarm.

On a received card, OK held offers Show on the map, Point to it, Answer: Acknowledged, Answer
with other phrases, and The line as it was sent.

An Urgent or Mayday message that is not a drill alerts and wakes the screen even in a muted chat,
unless the alert mode is Silent. Nothing is ever sent without a key press; there is no automatic
beacon. Phrases are in English only; the codes on air are the same in every language.

### Town locator

Says in words where you are. It needs `/geo/towns.bin` on the card.

- The top shows the town, county and state your device is in, and "Near the ... line" when you
  are close to a town line.
- Below are the towns around you, nearest first, with distance and direction.
- Typing finds towns by the start of their name. OK on a town opens the pointer page; OK there
  shows it on the map.
- Town names also appear on MECP cards that carry a position, in node details, and on the pointer
  page.

### Trip

- OK starts, pauses and resumes. OK held: Reset the trip, Reset the odometer (asks again), Send
  the track by Bluetooth.
- Shows trip distance, elapsed time, moving time, average while moving, fastest stretch, and a
  lifetime odometer that counts whether or not a trip is running.
- It needs a GPS fix with at least four satellites and the time.
- Distance is counted from fix to fix, and fixes reach the screen about once a minute, so a
  winding path reads short. Steps under 20 m are ignored so that standing still does not add up.
- A running trip also writes its track to `/apps/trip/` on the card. The trip survives a restart.

### Wardrive

Wardrive logs where the mesh is heard and how well, as a CSV file on the card for mapping on a
computer later.

- OK starts or stops logging. It keeps logging on every screen, with the screen dark, and after a
  restart. "Log" shows in the top bar.
- One row is written for each packet heard directly (at most one per node every 5 seconds), and
  one for every quiet minute, so you also record where nothing was heard.
- By default it only listens and sends nothing. In the menu (OK held) you can choose a mode that
  also pings every 1 to 30 minutes on a channel of your choice, asking nearby nodes to answer.
  No ping is sent while the channel is busy.
- The menu also has Ping now, Mark this place (a numbered row in the log), and Send the log by
  Bluetooth.
- It needs an SD card and the time, and stops by itself below 50 MB free.

The file holds positions and node names. Treat it accordingly.

### Compass

- Shows the heading in degrees and as a direction. The mark at the top of the rose is the device;
  the letters turn around it. Hold the device flat.
- OK opens Calibrate (turn the device every way for 30 seconds), Declination, and Turn the
  reading (0, 90, 180 or 270 degrees, in case north shows up a quarter turn off).
- Declination is taken from your GPS position by default, or you can type it (west is negative).
  Until one is known the heading is labeled "Magnetic north", afterwards "True north".
- With a working compass, the pointer pages in Nearby, Town locator and MECP turn with the device
  and say "Turns with you". Otherwise they say "North is up".

### Stopwatch

- OK starts and stops. Space or Right takes a split while it runs. Up and Down scroll the splits.
- OK held: Reset, Save the splits to the card, Clear the splits.
- It measures to a tenth of a second and holds up to 99 splits. From three splits on, the fastest
  and slowest laps are named.
- A restart ends it.

### Timer

- Type digits to set a length, read from the right as seconds, minutes, hours: `500` is 5:00,
  `13000` is 1:30:00. Del corrects, OK starts.
- OK starts, pauses and resumes. OK held: Reset, Repeat on or off, Change the length (a list of
  common lengths and the last one used).
- At zero the screen wakes and shows "Time is up" until you press a key, the keys flash if Flash
  is on, and the buzzer sounds unless sound is off. With Repeat on, the next round is already
  running.
- A dark, sleeping device may notice late that a timer is nearly due. Start timers shorter than
  about five minutes with the screen left on.

## 9. Settings

Up and Down move, Right or OK opens or changes a row, Left goes back to the list of pages. The
pages, in order: Channels, Sounds, Device, Display, Position, Bluetooth, Radio, Wi-Fi and files,
System. Radio is first only while no region is set; afterwards it sits near the end so that a
slip of the OK key does not land in the settings that can take you off your mesh.

Settings for the firmware's modules (MQTT, telemetry and the rest), security keys and remote
administration are not in JinxMesh. Use a phone app or the Meshtastic command line for those.

### Channels

Lists your channels and "Add a channel". Adding asks for a name and then a key. OK held on a
channel: Open chat, Rename, Change key, Share by QR code, Mute, Position, Delete (not for the
primary). Deleting a channel moves the later ones down a slot. Changes made from a phone show up
here without a restart.

### Sounds

- **Notifications:** which messages sound: all, direct messages only, or off.
- **Notification tone,** and separate tones for a direct message, an emergency message and the
  timer. You hear each tune as you move through the list. Besides the built-in tunes, the lists
  include your own from `/tones` on the card.
- **System sounds** (start and shutdown tunes, GPS, keys) and **Screen on and off sound.**
- **Flash the keys for a message, Wake the screen for a message, Warn near a country's border.**
- One row per channel: alerts as usual, muted, or a tune of its own.

### Device and Display

**Device:** long name, short name, role (Client suits a handheld), time zone.

**Display:** look (dark or light), brightness, screen off after, units (metric or imperial), 12
or 24-hour clock, keyboard light (Off, On, Auto), lock when the screen sleeps, names in chats
(short or long), and your list of quick replies.

### Position and Bluetooth

- **GPS** on or off (the radio restarts), **Share every** (how often your position is
  broadcast), **Announce me now** (sends a ping), **My position** (coordinates, altitude,
  satellites).
- One row per channel with the precision your position is shared at. The radio broadcasts only
  on the first channel that shares; the others answer when asked. Channels with the public
  default key are limited to about 700 m precision.
- **Bluetooth** is a single row, on or off. See [Bluetooth and phones](#12-bluetooth-and-phones).

### Radio

In order: Transmit power, Hop limit, Transmit (on, or off for listening only), Frequency slot,
Override MHz, Modem preset, Region. Choosing the Custom preset adds rows for bandwidth, spread
factor and coding rate. Everyone on a mesh needs the same preset and the same frequency.

**The frequency slot needs a moment of attention.** The list starts with the public slot of your
preset by name, for example "20 · 906.875 MHz · LongFast". Then comes "By channel name (n)", then
all the numbered slots.

- The **public slot** is where the public mesh of that preset lives.
- **By channel name** makes the radio work out the slot from the name of your primary channel.
  If your primary channel still has the default name, that is the public slot. If you gave your
  primary channel a name of its own, "By channel name" lands on a *different* frequency than the
  public mesh. That is fine for a private group whose members all use the same channel name, and
  it is the usual reason a device suddenly hears nobody.
- If you want a custom primary channel *and* to stay on the public mesh's frequency, pick the
  public slot explicitly.

An override frequency, when set, wins over the slot.

### Wi-Fi and files, System

**Wi-Fi and files:** Wi-Fi on or off, Network (scan and pick, or type a name), Password, Saved
networks, Status, Fetch missing map tiles, Receive files, Browse the SD card. See
[Wi-Fi and files](#13-wi-fi-and-getting-files-onto-the-card).

**System:** Restart, Shut down, Reset the node list, Clear all chats, Factory reset, About. About
shows the firmware version, "Built yyyy-mm-dd hh:mm" for the build you are running, your node,
the radio settings, and below that the license and credits.

## 10. Control center

CTRL opens the control center from anywhere. Up and Down move, Left and Right change a value, OK
steps to the next value. The rows are Turn the screen off, Keyboard light, Alert (Sound, Flash,
Sound + flash, Silent), Brightness, Screen off after, GPS, Bluetooth, Wi-Fi, Lock the screen.
GPS and Bluetooth restart the radio, so they ask first.

## 11. Lock screen

Lock the screen from the control center, or let it lock whenever the screen sleeps (Settings >
Display). The lock screen shows the clock and what is waiting. Only holding OK unlocks. This is a
guard against keys pressed in a pocket, not a security feature: there is no code.

## 12. Bluetooth and phones

Turn Bluetooth on under Settings > Bluetooth or in the control center; the radio restarts once.

- Bluetooth and the screen work at the same time. Text messages and waypoints you send from a
  paired phone show on the M9's screen, and what you send from the M9 shows in the phone app.
- Turning Bluetooth on does not take the screen over. The pairing screen appears by itself when a
  phone asks to pair, and shows the PIN to enter on the phone.
- JinxMesh uses one-time PINs: a new one for each pairing, shown only while a phone pairs and for
  at most 60 seconds. If a fixed PIN or no PIN was set from an app, the Bluetooth screen offers to
  change to one-time PINs with OK. A fixed PIN is never displayed.
- On the lock screen the PIN stays hidden until you hold OK.
- On the Bluetooth screen, BACK closes it and keeps Bluetooth on. Holding OK turns Bluetooth off.
- The device accepts one Bluetooth connection at a time.

A web page or tool can also use Bluetooth to send a destination or a whole route to Navigate, and
to fetch the logs in `/apps` from the card. The protocol is in [REFERENCE.md](REFERENCE.md). This
route inbox has only been run in the simulator so far.

## 13. Wi-Fi and getting files onto the card

Maps and data files are large, and there are four ways to get them onto the SD card.

| Way | Speed | Good for |
|---|---|---|
| Take the card out and use a card reader | Fastest | Anything large |
| The upload page over Wi-Fi | About 300 kB a second | Maps, without opening the device |
| The USB cable, with a tool that speaks the serial file protocol | About 3.5 MB a minute | Small files, no Wi-Fi |
| Bluetooth | Slow, and from the card only | Fetching logs |

**Joining a network.** Settings > Wi-Fi and files > Network scans and lists what it finds; pick
one and type the password. The password is typed behind stars and never shown again. Up to eight
networks are remembered, and out of reach of one the device takes the strongest of the others.
If Wi-Fi was configured through a phone app instead, the page says "Run by the radio" and leaves
it alone.

**The upload page.**

1. Turn on **Receive files**. The screen shows an address, port 8080, and a six-digit code that
   is new each time.
2. Open that address in a browser on the same network, enter the code, and choose files and
   where they go.
3. Turn Receive files off when you are done. While it is on, the device does not sleep.

The page is plain HTTP protected only by the code. Use it on a network you trust. An upload
overwrites an existing file of the same name without asking.

**Browse the SD card** lists folders and files and can delete a file. **Fetch missing tiles over
Wi-Fi** in the map menu creates an online style that downloads the tiles you look at and keeps
them on the card as a raster map.

**What goes where.** Vector map pieces go to `/maps/state/` or `/maps/province/`, a raster style
to `/maps/<style>/`, road and town data to `/geo/roads.bin` and `/geo/towns.bin`, your own tunes
to `/tones/`. The device writes its own files below `/apps/`. [REFERENCE.md](REFERENCE.md) has
the full layout and [MAP-DATA.md](MAP-DATA.md) explains how to make the raster, town and road
files.

If you replace the archive of the raster style that is on screen, pick the style again in Map
style or restart; until then the map can show broken tiles.

## 14. Battery tips

Battery life has not been measured, so there are no numbers here, only what is known to cost
power, most expensive first:

1. **Wi-Fi.** Leave it off when you are not moving files or fetching tiles.
2. **The backlight.** Lower the brightness and use a short "Screen off after". Home pressed on
   Home turns the screen off at once.
3. **GPS without a view of the sky.** Indoors it keeps searching for a long time. Turn GPS off
   (hold Location) when you do not need it.
4. **The upload page left on.** It keeps the processor awake.
5. **Bluetooth.**

Behind a dark screen the processor sleeps while the radio keeps listening; a packet or a key
wakes it, so nothing is lost by letting the screen go dark. An active Navigate route keeps the
GPS receiver and the processor on, so end the navigation when you arrive. The battery percentage uses a curve for the M9's 4.35 V cell. It has not been checked against a
full measured discharge, so treat the last few percent with caution.

## 15. What has and has not been tested on real hardware

JinxMesh is developed mostly in a simulator that runs the real firmware but has no radio, no
keyboard chip, no SD card and no Wi-Fi. Much of it has since been used on real M9s, but not all
of it, and you should know which is which.

**Used on real M9s**

- Flashing (update and full install), booting, the d-pad, the tabs, Home tiles, setting the
  region and names.
- The SD card with large files: uploads of tens of megabytes over the upload page and transfers
  over the USB cable, compared against the originals. Wi-Fi with a stored password.
- Bluetooth together with the screen, with messages and waypoints passing both ways.
- Vector maps drawn from the card, sleep behind a dark screen, a tune on the buzzer.

**Only run in the simulator so far, or not confirmed on a device**

- The Bluetooth inbox for destinations and routes.
- Navigate on the device: reading the road file from the card, how long a search takes, and
  following a route with the GPS receiver.
- The compass: the sensors are found at start, but nobody has checked the heading, which way is
  forward, or whether the calibration fits.
- The keyboard light and the Flash alert.
- Trace route and ping answers from a real node. One trace route tried on a device did nothing,
  and the cause was not found.
- Wardrive writing to the real card, including the ping mode.
- The timer's sound, and the timer ringing on time out of sleep.
- Reading a large raster PMTiles archive on the device, and fetching tiles over Wi-Fi.
- Whether vector map pieces join cleanly at their seams.
- The Town locator reading its file from the real card.
- Memory with Wi-Fi and Bluetooth running together for hours, battery life, and the battery
  percentage over a full discharge.

If something in the second list works or fails for you, a report is welcome at
https://github.com/arugulax/jinxmesh. Keep a way to flash the stock firmware back.
