#pragma once

#include "configuration.h"

#if defined(USE_KEYBOARD_VIEW) && defined(ARCH_ESP32) && defined(HAS_SDCARD) && !MESHTASTIC_EXCLUDE_BLUETOOTH
#include <BLEDevice.h>

#define JINX_SERVICE_UUID "9971f534-e514-4da7-81ce-e011c4e47254"
#define JINX_FILES_UUID "c4f2a44c-dece-403d-851b-8f4ee0c4b4ad"
#define JINX_OFFER_UUID "9c6efe0e-fe07-414d-9ee1-e7b101e01d9f"
#define JINX_INBOX_UUID "5d0b7c7e-2f0a-4c55-9d53-6a0c1f1b7e21"

// The files of the card's /apps folder, to read: a service of its own. See JinxBle.cpp.
void jinxBleSetup(BLEServer *server, bool secure);
// Bluetooth was turned off: what was set up is freed
void jinxBleGone(void);
#define JINX_BLE_FILES 1
#endif
