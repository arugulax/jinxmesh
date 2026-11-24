#include "configuration.h"

#if defined(USE_KEYBOARD_VIEW) && defined(ARCH_ESP32) && defined(HAS_SDCARD) && !MESHTASTIC_EXCLUDE_BLUETOOTH

#include "JinxBle.h"
#include "SPILock.h"
#include "concurrency/LockGuard.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <algorithm>
#include <string>

/**
 * Files of the card's /apps folder (the wardrive and trip logs) over Bluetooth, to read only: a service
 * of its own with one characteristic, with the pairing the mesh service has. It does not go through the phone
 * API, so whoever fetches a log takes no messages away from the screen.
 *
 *   write:  u32 where to start (little endian) | the path; a path that ends in / is a folder, whose list
 *           comes as text ("d name" or "f bytes name" per line)
 *   read:   u8 0 | u32 where this starts | up to 480 bytes; no bytes: that was all
 *           u8 1 | a text: it did not work
 *
 * A second characteristic, to be notified by: the device says "this file is for you" (its path, or a folder
 * with a / at its end) when that is chosen on it, and whoever listens fetches it as above. So the sending is
 * started on the device and goes to whatever is connected.
 *
 * One write and one read per piece: nothing is kept between them, and a file that is being written to
 * (today's log) is opened and closed for each.
 *
 * A third characteristic, to write to: a line of text for the screen (a destination, the stops of a route; what
 * they mean is the screen's business, KeyboardViewAppsRoute.cpp). The lines wait here until the screen asks for
 * them (jinxmesh_inbox); reading the characteristic says how many of them still have room.
 */
namespace
{
constexpr size_t c_block = 480;

bool pathFits(const std::string &path)
{
    if (path.size() < 6 || path.size() > 200 || path.compare(0, 6, "/apps/") != 0 || path.find("..") != std::string::npos)
        return false;
    for (char c : path)
        if ((uint8_t)c < 0x20 || c == '\\')
            return false;
    return true;
}

// The stack of the Bluetooth task is small (5 kB) and a look at the card needs more than is left of it: the
// callback hands the request to a task of its own and waits for the answer. One request at a time (the
// Bluetooth stack calls one callback after the other).
uint8_t request[4 + 204];
size_t requestLength = 0;
uint8_t result[5 + c_block];
size_t resultLength = 0;
SemaphoreHandle_t asked = nullptr, answered = nullptr;

void fail(const char *text)
{
    result[0] = 1;
    resultLength = 1 + std::min<size_t>(strlen(text), 60);
    memcpy(result + 1, text, resultLength - 1);
}

void answer(void)
{
    if (requestLength < 6)
        return fail("no path");
    const uint32_t offset = request[0] | request[1] << 8 | request[2] << 16 | (uint32_t)request[3] << 24;
    std::string path((const char *)request + 4, requestLength - 4);
    const bool folder = path.back() == '/';
    if (!pathFits(path))
        return fail("only files in /apps");
    if (folder)
        path.pop_back();
    result[0] = 0;
    result[1] = (uint8_t)offset;
    result[2] = (uint8_t)(offset >> 8);
    result[3] = (uint8_t)(offset >> 16);
    result[4] = (uint8_t)(offset >> 24);
    size_t got = 0;
    concurrency::LockGuard g(spiLock);
    if (SD.cardType() == CARD_NONE)
        return fail("no card");
    File f = SD.exists(path.c_str()) ? SD.open(path.c_str()) : File();
    if (!f)
        return fail(folder ? "there is no such folder" : "there is no such file");
    if (folder != f.isDirectory()) {
        f.close();
        return fail(folder ? "that is a file" : "that is a folder");
    }
    if (folder) {
        std::string text;
        for (File e = f.openNextFile(); e; e = f.openNextFile()) {
            static char line[300];
            if (e.isDirectory())
                snprintf(line, sizeof(line), "d %s\n", e.name());
            else
                snprintf(line, sizeof(line), "f %lu %s\n", (unsigned long)e.size(), e.name());
            e.close();
            text += line;
            if (text.size() > offset + c_block)
                break; // enough for this piece
        }
        if (offset < text.size()) {
            got = std::min<size_t>(text.size() - offset, c_block);
            memcpy(result + 5, text.data() + offset, got);
        }
    } else if (offset < f.size() && f.seek(offset)) {
        const int n = f.read(result + 5, c_block);
        got = n > 0 ? (size_t)n : 0;
    }
    f.close();
    resultLength = 5 + got;
}

void worker(void *)
{
    for (;;) {
        if (xSemaphoreTake(asked, portMAX_DELAY) != pdTRUE)
            continue;
        answer();
        xSemaphoreGive(answered);
    }
}

class Callbacks : public BLECharacteristicCallbacks
{
    // runs in the task of the Bluetooth stack: nothing here but handing over and waiting
    void onWrite(BLECharacteristic *c) override
    {
        static const uint8_t late[] = {1, 'b', 'u', 's', 'y'};
        const size_t length = c->getLength();
        const uint8_t *data = c->getData();
        if (!asked || !data || length > sizeof(request)) {
            c->setValue(late, sizeof(late));
            return;
        }
        xSemaphoreTake(answered, 0); // an answer nobody waited for any more
        memcpy(request, data, length);
        requestLength = length;
        xSemaphoreGive(asked);
        if (xSemaphoreTake(answered, pdMS_TO_TICKS(8000)) == pdTRUE)
            c->setValue(result, resultLength);
        else
            c->setValue(late, sizeof(late));
    }
};

// the lines for the screen: written by the task of the Bluetooth stack, taken by the task of the screen
constexpr size_t c_lines = 40, c_lineLength = 160;
char lines[c_lines][c_lineLength];
volatile size_t lineIn = 0, lineOut = 0; // each is moved by one task only
portMUX_TYPE lineLock = portMUX_INITIALIZER_UNLOCKED;

size_t linesFree(void)
{
    return c_lines - 1 - (lineIn + c_lines - lineOut) % c_lines;
}

class InboxCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *c) override
    {
        const size_t length = c->getLength();
        const uint8_t *data = c->getData();
        if (data && length && length < c_lineLength && linesFree()) {
            portENTER_CRITICAL(&lineLock);
            memcpy(lines[lineIn], data, length);
            lines[lineIn][length] = 0;
            lineIn = (lineIn + 1) % c_lines;
            portEXIT_CRITICAL(&lineLock);
        }
        const uint8_t room = (uint8_t)linesFree();
        c->setValue(&room, 1);
    }
};

BLECharacteristic *offer = nullptr;
BLEServer *host = nullptr;
volatile bool listening = false;

class OfferCallbacks : public BLECharacteristicCallbacks
{
    void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *, uint16_t subValue) override { listening = subValue != 0; }
};
} // namespace

void jinxBleGone(void)
{
    listening = false;
    offer = nullptr;
    host = nullptr;
}

extern "C" int jinxmesh_inbox(char *buffer, int size)
{
    if (!buffer || size < 2 || lineOut == lineIn)
        return 0;
    portENTER_CRITICAL(&lineLock);
    strncpy(buffer, lines[lineOut], size - 1);
    buffer[size - 1] = 0;
    lineOut = (lineOut + 1) % c_lines;
    portEXIT_CRITICAL(&lineLock);
    return (int)strlen(buffer);
}

extern "C" int jinxmesh_offer(const char *path)
{
    if (!offer || !host || !path || !listening || host->getConnectedCount() == 0)
        return 0;
    offer->setValue((const uint8_t *)path, std::min<size_t>(strlen(path), 200));
    offer->notify();
    return 1;
}

void jinxBleSetup(BLEServer *server, bool secure)
{
    if (!asked) {
        asked = xSemaphoreCreateBinary();
        answered = xSemaphoreCreateBinary();
        if (!asked || !answered || xTaskCreate(worker, "jinxble", 8192, nullptr, 3, nullptr) != pdPASS) {
            LOG_ERROR("Bluetooth files: no task");
            asked = nullptr;
            return;
        }
    }
    BLEService *service = server->createService(JINX_SERVICE_UUID);
    uint32_t properties = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
    if (secure)
        properties |= BLECharacteristic::PROPERTY_READ_AUTHEN | BLECharacteristic::PROPERTY_READ_ENC |
                      BLECharacteristic::PROPERTY_WRITE_AUTHEN | BLECharacteristic::PROPERTY_WRITE_ENC;
    BLECharacteristic *files = service->createCharacteristic(JINX_FILES_UUID, properties);
    static Callbacks callbacks; // the service is set up again when Bluetooth is turned off and on
    files->setCallbacks(&callbacks);
    const uint8_t nothing[1] = {1};
    files->setValue(nothing, 1);

    uint32_t offerProperties = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY;
    if (secure)
        offerProperties |= BLECharacteristic::PROPERTY_READ_AUTHEN | BLECharacteristic::PROPERTY_READ_ENC;
    static OfferCallbacks offerCallbacks;
    listening = false;
    host = server;
    offer = service->createCharacteristic(JINX_OFFER_UUID, offerProperties);
    offer->setCallbacks(&offerCallbacks);

    uint32_t inboxProperties = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
    if (secure)
        inboxProperties |= BLECharacteristic::PROPERTY_READ_AUTHEN | BLECharacteristic::PROPERTY_READ_ENC |
                           BLECharacteristic::PROPERTY_WRITE_AUTHEN | BLECharacteristic::PROPERTY_WRITE_ENC;
    BLECharacteristic *inbox = service->createCharacteristic(JINX_INBOX_UUID, inboxProperties);
    static InboxCallbacks inboxCallbacks;
    inbox->setCallbacks(&inboxCallbacks);
    const uint8_t room = (uint8_t)(c_lines - 1);
    inbox->setValue(&room, 1);
    service->start();
}

#endif
