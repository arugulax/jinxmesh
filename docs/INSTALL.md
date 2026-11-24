# Installing JinxMesh on a ThinkNode M9

JinxMesh is flashed over the M9's USB port like any ESP32 firmware. This page covers a first
install, later updates, and the way back to stock Meshtastic.

## Before you start

- **This replaces the firmware on the device.** A first install erases everything on it:
  settings, channels, keys and chats. If the node already has a configuration you care about,
  export it with the Meshtastic app or command line first.
- **Only the Elecrow ThinkNode M9.** Do not flash these files onto another board.
- **A data cable.** Many USB-C cables only charge. If the computer does not see a new serial
  port when you plug the M9 in, try another cable first.
- The M9 shows up as a CH340 serial port (USB id `1a86:7522`). Windows may need the CH340
  driver; Linux and macOS have it built in.

## The files

Each release on the [Releases](../../../releases) page carries:

| File | Use |
|---|---|
| `firmware-thinknode_m9-tft-kb-<version>.factory.bin` | First install. Written at address `0x0` after erasing the flash. |
| `firmware-thinknode_m9-tft-kb-<version>.bin` | Update. Written at address `0x10000`. Keeps settings, channels, keys and chats. |
| `SHA256SUMS` | Checksums of the two files |

`<version>` is the Meshtastic version followed by the JinxMesh commit, for example
`2.8.3.9485eff`. The release notes give the build date, which the device also shows under
Settings, System, About.

## First install

With [esptool](https://docs.espressif.com/projects/esptool/) (`pip install esptool`):

```
esptool.py --chip esp32s3 --port PORT --baud 230400 erase_flash
esptool.py --chip esp32s3 --port PORT --baud 230400 write_flash 0x0 firmware-thinknode_m9-tft-kb-<version>.factory.bin
```

`PORT` is the serial port, such as `/dev/ttyUSB0`, `/dev/cu.wchusbserial10` or `COM5`.

Or from a browser, with no software to install: open
[esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge, connect, erase the
flash, then program the factory file at address `0x0`.

Unplug and plug the M9 back in, or press its reset button. On the very first start the radio
chip updates its own firmware, which takes about eight seconds. Leave the device powered.

Then set the region: **Settings, Radio, Region**. Until a region is set the radio and the GPS
stay off, and the device shows no nodes and no position.

## Updating

```
esptool.py --chip esp32s3 --port PORT --baud 230400 write_flash 0x10000 firmware-thinknode_m9-tft-kb-<version>.bin
```

Nothing is erased. Settings, channels, keys, chats and the files on the SD card stay as they
are. If the device stays dark after an update, reset it once more.

## When the M9 will not enter its loader

esptool sometimes fails with "Failed to connect" or "Serial data stream stopped". Things that
have helped, in this order:

1. Unplug the cable, plug it in again, and start the flash right away. An M9 that has been
   plugged in for several minutes is the usual cause.
2. Use a USB port directly on the computer, not a hub or a dock.
3. Try another cable.
4. Lower the speed: `--baud 115200`.

A flash that breaks off partway leaves the device without working firmware, but it cannot
damage it. Flash again and it starts.

## Maps and data for the card

The firmware works without an SD card. Maps, the town file and the road file for Navigate go
on a card (FAT32 or exFAT). See the README for where to get maps, and
[MAP-DATA.md](MAP-DATA.md) for the other files. You can copy files with a card reader, over
Wi-Fi from the device's upload page, or over the USB cable; the
[user guide](USER-GUIDE.md) describes each way.

## Going back to stock Meshtastic

Flash the stock Meshtastic firmware for the ThinkNode M9 the same way, as a full install:
erase the flash, then write the stock factory image at `0x0`. The
[Meshtastic web flasher](https://flasher.meshtastic.org) does this for you when it lists the
board. JinxMesh changes nothing on the device that outlives a full erase.

## What JinxMesh stores on the device

- Its own preferences are in `/kbview.cfg` on the internal file system. An update keeps the
  file; a full install removes it.
- A Wi-Fi password you type on the device is stored sealed, not in clear text. See
  [REFERENCE.md](REFERENCE.md) for what that does and does not protect against.
