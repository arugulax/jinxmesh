# webflash

Flash and monitor serial devices that are plugged into the computer showing a web page,
from the machine that serves the page. Built for flashing an ESP32 board attached to a
laptop with firmware built on a headless box.

- `server.py` (aiohttp, 127.0.0.1:8377) serves the page under `/pub/` and takes commands
  under `/ctl/`. Put a web server with HTTPS in front of it and proxy only `/pub/` (for
  example as `/flash/`), so that commands can only come from the machine itself.
- `static/` is the page. It uses Web Serial (Chromium desktop browsers, HTTPS) and
  [esptool-js](https://github.com/espressif/esptool-js) 0.7.1, vendored as `static/esptool.js`.
- `webflash` is the CLI; `webflash --help` lists the commands.

Run `server.py` as a service of your choice; the page is wherever your web server puts `/pub/`.

## Use

1. Open the page on the computer with the device, press **Add a USB device**, pick it.
   The browser remembers the grant; the page stays in control through its switch
   "Let the server control these devices".
2. On the server:

       webflash status
       webflash open                 # serial monitor, RX is appended to logs/
       webflash log -n 60
       webflash chip                 # bootloader, chip, MAC, flash size
       webflash flash NAME 0x10000:/path/to/firmware.bin

   `webflash offer NAME OFFSET:FILE...` only lists the firmware on the page, with a
   Flash button for whoever sits at that computer.

While a device is being monitored its byte stream is also on `socket://127.0.0.1:4404`
for pyserial tools.

## Files and tunes for a device's SD card

For JinxMesh firmware with the file transfer on its serial console (firmware
`src/jinx/JinxFiles.cpp`, where the protocol is described):

    webflash put roads.bin /geo/roads.bin    # continues a broken transfer, compares afterwards
    webflash ls /geo
    webflash rm /geo/old.bin
    webflash get /tones/mytune.rtttl
    webflash crc /geo/roads.bin --file roads.bin
    webflash tone "test:d=8,o=6,b=180:c,e,g,c7"

`serialfiles.py` is the host side, through the raw bridge. The page has the same in two
sections, "Files for the device's SD card" (send a file, look at the card, delete) and
"Tunes for the buzzer" (an editor that plays in the browser and on the device, and saves to
`/tones`). `static/ringtones.json`, the tunes the device comes with, is made from device-ui's
`source/graphics/common/Ringtones.cpp`.

When the script of the page changes, change the `?v=` behind `app.js` in
`static/index.html` as well: browsers keep the old script otherwise.

Flashing an M9: esptool often cannot get the chip into its download mode once the device has
been plugged in for some minutes. Close the monitor, have the cable plugged in anew, flash at
once.

## Limits

- Web Serial covers what the OS exposes as a serial port (USB CDC, CP210x, CH34x, FTDI).
  Other USB classes, such as the UF2 mass-storage bootloaders of nRF52 and RP2040
  boards, are out of reach.
- Flashing is ESP only (esptool-js). Images are written as they are (`keep` for flash
  mode, frequency and size) and checked against the MD5 the chip computes.
- A board with native USB may re-enumerate when it resets; if the browser shows it as a
  new device, add it again.
- Background tabs get throttled timers; keep the tab in front while flashing.
