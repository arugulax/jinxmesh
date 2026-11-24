#include "configuration.h"

#if defined(USE_KEYBOARD_VIEW) && defined(ARCH_ESP32) && defined(HAS_SDCARD)

#include "JinxFiles.h"
#include "SPILock.h"
#include "concurrency/LockGuard.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <algorithm>
#include <atomic>
#include <string>

/**
 * The protocol. The host writes the line "JXFILES1\n" to the console at its usual speed; from the answer
 * on, both sides speak in frames, until the host says Exit or stays silent for 30 seconds:
 *
 *   0xA5 0x4A | type | sequence | length (2, little endian) | payload | CRC-32 (4, little endian)
 *
 * The CRC (the one of zlib) covers type to payload. The device answers every frame it could read with Ack
 * or Error (a text), carrying the same sequence number; Data that does not start where the file ends gets
 * Offset (u64, where to go on) instead; a frame with a wrong CRC gets no answer and the host sends
 * it again. What the console logs in between is not framed, the host skips it.
 *
 *   Begin   u64 size, u8 flags (1: go on with what is there), path     Ack: u64 offset to go on from
 *   Data    u64 offset, bytes                                          Ack: u64 offset after them
 *   End                                                                Ack (the file has its name now)
 *   Stat    path                                                       Ack: u8 kind (0 none, 1 file, 2 folder), u64 size
 *   Delete  path                                                       Ack
 *   Baud    u32                                                        Ack, then the speed changes
 *   Exit                                                               Ack, then the console is back
 *   List    path                                                       Ack: lines "d name" or "f size name"
 *   Crc     path                                                       Ack: u32 CRC-32, u64 size
 *   Read    u64 offset, path                                           Ack: up to 4000 bytes from there (none: the end)
 *   Tone    a tune as RTTTL text                                       Ack: u8, 1 when it sounds (a test of the buzzer)
 *
 * The host may send two Data frames ahead of the answers (the console's receive buffer holds one while the
 * other is written to the card); what does not fit where the file ends is answered with Offset and sent again.
 *
 * A file is written as <path>.part and renamed at End, so that a broken transfer leaves no half file
 * under the real name, and can be continued.
 */
extern "C" int jinxmesh_play(const char *rtttl); // JinxTones.cpp

namespace
{
constexpr char c_hello[] = "JXFILES1\n";
constexpr uint16_t c_maxPayload = 8192 + 16;
constexpr uint32_t c_idleMs = 30000;
constexpr uint32_t c_gapMs = 2000;     // within a frame
constexpr uint32_t c_syncBytes = 1 << 20; // the size on the card is brought up to date this often

enum Type : uint8_t { Begin = 1, Data, End, Stat, Delete, Baud, Exit, List, Crc, Tone, Read, Ack = 0x80, Error = 0x81, Offset = 0x82 };

std::atomic<bool> active{false};
uint8_t matched = 0;

// for the screen: the file that comes in now
std::atomic<bool> receiving{false};
std::atomic<uint32_t> receivedBytes{0}, receivingBytes{0};
char receivingName[40];

uint32_t crcTable[256];

void crcInit(void)
{
    if (crcTable[1])
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[i] = c;
    }
}

// running CRC: start with 0
uint32_t crc32(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    while (length--)
        crc = crcTable[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++, v >>= 8)
        p[i] = (uint8_t)v;
}

void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++, v >>= 8)
        p[i] = (uint8_t)v;
}

class Transfer
{
  public:
    void run(void)
    {
        crcInit();
        frame = (uint8_t *)malloc(c_maxPayload + 4);
        if (!frame) {
            answer(Error, 0, "no memory");
            return;
        }
        uint8_t hello[10] = {'J', 'X', 'F', 'I', 'L', 'E', 'S', '1', (uint8_t)(c_maxPayload & 0xFF), (uint8_t)(c_maxPayload >> 8)};
        answer(Ack, 0, hello, sizeof(hello));
        while (!done) {
            uint8_t type = 0, sequence = 0;
            uint16_t length = 0;
            if (!read(type, sequence, length))
                break; // silence
            handle(type, sequence, length);
        }
        closeFile();
        if (Serial.baudRate() != SERIAL_BAUD) {
            Serial.flush();
            delay(50);
            Serial.updateBaudRate(SERIAL_BAUD);
        }
        free(frame);
    }

  private:
    // one byte, or -1 after that many milliseconds without
    int byte(uint32_t timeout)
    {
        uint32_t start = millis();
        for (;;) {
            int c = Serial.read();
            if (c >= 0)
                return c;
            if (millis() - start >= timeout)
                return -1;
            vTaskDelay(1);
        }
    }

    bool bytes(uint8_t *to, size_t length)
    {
        uint32_t last = millis();
        while (length) {
            size_t got = Serial.available() ? Serial.read(to, length) : 0;
            if (got) {
                to += got;
                length -= got;
                last = millis();
            } else if (millis() - last >= c_gapMs) {
                return false;
            } else {
                vTaskDelay(1);
            }
        }
        return true;
    }

    // the next frame with a good CRC; false after c_idleMs without one
    bool read(uint8_t &type, uint8_t &sequence, uint16_t &length)
    {
        uint32_t start = millis();
        while (millis() - start < c_idleMs) {
            int c = byte(500);
            if (c != 0xA5)
                continue;
            if (byte(c_gapMs) != 0x4A)
                continue;
            uint8_t head[4];
            if (!bytes(head, 4))
                continue;
            length = head[2] | (head[3] << 8);
            if (length > c_maxPayload)
                continue;
            // the CRC covers the head as well: keep it in front of the payload
            memcpy(frame, head, 4);
            uint8_t tail[4];
            if (!bytes(frame + 4, length) || !bytes(tail, 4))
                continue;
            uint32_t sum = tail[0] | (tail[1] << 8) | (tail[2] << 16) | ((uint32_t)tail[3] << 24);
            if (crc32(0, frame, 4 + length) != sum)
                continue;
            type = head[0];
            sequence = head[1];
            return true;
        }
        return false;
    }

    void answer(uint8_t type, uint8_t sequence, const uint8_t *payload, uint16_t length)
    {
        static uint8_t out[6 + 4096 + 4];
        if (length > 4096)
            length = 4096;
        out[0] = 0xA5;
        out[1] = 0x4A;
        out[2] = type;
        out[3] = sequence;
        out[4] = length & 0xFF;
        out[5] = length >> 8;
        if (length)
            memcpy(out + 6, payload, length);
        put32(out + 6 + length, crc32(0, out + 2, 4 + length));
        Serial.write(out, 6 + length + 4); // one call, so that log lines cannot get in between
    }

    void answer(uint8_t type, uint8_t sequence, const char *text)
    {
        answer(type, sequence, (const uint8_t *)text, (uint16_t)strlen(text));
    }

    void ack64(uint8_t sequence, uint64_t value)
    {
        uint8_t b[8];
        put64(b, value);
        answer(Ack, sequence, b, 8);
    }

    static bool pathFits(const std::string &path)
    {
        if (path.size() < 2 || path.size() > 200 || path[0] != '/' || path.find("..") != std::string::npos)
            return false;
        for (char c : path)
            if ((uint8_t)c < 0x20 || c == '\\')
                return false;
        return true;
    }

    std::string pathAt(const uint8_t *payload, uint16_t length, uint16_t from)
    {
        return length > from ? std::string((const char *)payload + from, length - from) : std::string();
    }

    static bool cardThere(void) { return SD.cardType() != CARD_NONE; }

    void closeFile(void)
    {
        receiving = false;
        if (file) {
            concurrency::LockGuard g(spiLock);
            file.close();
        }
    }

    void handle(uint8_t type, uint8_t sequence, uint16_t length)
    {
        const uint8_t *payload = frame + 4;
        switch (type) {
        case Begin: {
            const std::string path = pathAt(payload, length, 9);
            if (length < 10 || !pathFits(path))
                return answer(Error, sequence, "that path is not allowed");
            closeFile();
            concurrency::LockGuard g(spiLock);
            if (!cardThere())
                return answer(Error, sequence, "no card");
            size = get64(payload);
            name = path;
            const std::string part = path + ".part";
            for (size_t slash = part.find('/', 1); slash != std::string::npos; slash = part.find('/', slash + 1))
                SD.mkdir(part.substr(0, slash).c_str());
            position = 0;
            if ((payload[8] & 1) && SD.exists(part.c_str())) {
                file = SD.open(part.c_str(), FILE_APPEND);
                if (file && file.size() <= size) {
                    position = file.size();
                } else if (file) {
                    file.close();
                }
            }
            if (!file)
                file = SD.open(part.c_str(), FILE_WRITE);
            if (!file)
                return answer(Error, sequence, "cannot write there");
            unsynced = 0;
            {
                const size_t slash = path.rfind('/');
                snprintf(receivingName, sizeof(receivingName), "%s", path.c_str() + (slash == std::string::npos ? 0 : slash + 1));
                receivingBytes = (uint32_t)std::min<uint64_t>(size, UINT32_MAX);
                receivedBytes = (uint32_t)position;
                receiving = true;
            }
            ack64(sequence, position);
            break;
        }
        case Data: {
            if (!file || length < 8)
                return answer(Error, sequence, "no file is open");
            const uint64_t offset = get64(payload);
            const uint16_t count = length - 8;
            if (offset + count <= position) // the answer to these got lost
                return ack64(sequence, position);
            if (offset != position || position + count > size) {
                uint8_t b[8];
                put64(b, position);
                return answer(Offset, sequence, b, 8);
            }
            concurrency::LockGuard g(spiLock);
            if (file.write(payload + 8, count) != count) {
                file.close();
                return answer(Error, sequence, "writing to the card failed");
            }
            position += count;
            receivedBytes = (uint32_t)std::min<uint64_t>(position, UINT32_MAX);
            unsynced += count;
            if (unsynced >= c_syncBytes) {
                file.flush();
                unsynced = 0;
            }
            ack64(sequence, position);
            break;
        }
        case End: {
            if (!file)
                return answer(Error, sequence, "no file is open");
            receiving = false;
            concurrency::LockGuard g(spiLock);
            file.close();
            if (position != size)
                return answer(Error, sequence, "the file is not complete");
            const std::string part = name + ".part";
            if (SD.exists(name.c_str()))
                SD.remove(name.c_str());
            if (!SD.rename(part.c_str(), name.c_str()))
                return answer(Error, sequence, "cannot give the file its name");
            LOG_INFO("serial files: %s saved, %u bytes", name.c_str(), (unsigned)size);
            answer(Ack, sequence, nullptr, 0);
            break;
        }
        case Stat: {
            const std::string path = pathAt(payload, length, 0);
            if (!pathFits(path) && path != "/")
                return answer(Error, sequence, "that path is not allowed");
            concurrency::LockGuard g(spiLock);
            if (!cardThere())
                return answer(Error, sequence, "no card");
            uint8_t b[9] = {0};
            File f = SD.exists(path.c_str()) ? SD.open(path.c_str()) : File();
            if (f) {
                b[0] = f.isDirectory() ? 2 : 1;
                put64(b + 1, f.size());
                f.close();
            }
            answer(Ack, sequence, b, 9);
            break;
        }
        case Delete: {
            const std::string path = pathAt(payload, length, 0);
            if (!pathFits(path))
                return answer(Error, sequence, "that path is not allowed");
            closeFile();
            concurrency::LockGuard g(spiLock);
            if (!cardThere())
                return answer(Error, sequence, "no card");
            if (!SD.exists(path.c_str()))
                return answer(Error, sequence, "there is no such file");
            if (!SD.remove(path.c_str()) && !SD.rmdir(path.c_str()))
                return answer(Error, sequence, "cannot delete that");
            answer(Ack, sequence, nullptr, 0);
            break;
        }
        case Baud: {
            if (length < 4)
                return answer(Error, sequence, "no speed");
            const uint32_t baud = payload[0] | (payload[1] << 8) | (payload[2] << 16) | ((uint32_t)payload[3] << 24);
            if (baud < 9600 || baud > 3000000)
                return answer(Error, sequence, "that speed is not possible");
            answer(Ack, sequence, nullptr, 0);
            Serial.flush();
            delay(50);
            Serial.updateBaudRate(baud);
            break;
        }
        case Exit:
            answer(Ack, sequence, nullptr, 0);
            done = true;
            break;
        case List: {
            const std::string path = pathAt(payload, length, 0);
            if (!pathFits(path) && path != "/")
                return answer(Error, sequence, "that path is not allowed");
            concurrency::LockGuard g(spiLock);
            if (!cardThere())
                return answer(Error, sequence, "no card");
            File dir = SD.open(path.c_str());
            if (!dir || !dir.isDirectory())
                return answer(Error, sequence, "there is no such folder");
            std::string text;
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                char line[300];
                if (f.isDirectory())
                    snprintf(line, sizeof(line), "d %s\n", f.name());
                else
                    snprintf(line, sizeof(line), "f %lu %s\n", (unsigned long)f.size(), f.name()); // no %llu in this libc
                f.close();
                if (text.size() + strlen(line) > 4000) {
                    text += "...\n";
                    break;
                }
                text += line;
            }
            dir.close();
            answer(Ack, sequence, (const uint8_t *)text.data(), (uint16_t)text.size());
            break;
        }
        case Crc: {
            const std::string path = pathAt(payload, length, 0);
            if (!pathFits(path))
                return answer(Error, sequence, "that path is not allowed");
            closeFile();
            File f;
            {
                concurrency::LockGuard g(spiLock);
                if (!cardThere())
                    return answer(Error, sequence, "no card");
                if (SD.exists(path.c_str()))
                    f = SD.open(path.c_str());
            }
            if (!f || f.isDirectory())
                return answer(Error, sequence, "there is no such file");
            uint32_t sum = 0;
            uint64_t total = 0;
            for (;;) {
                int got;
                {
                    // the bus is given back between blocks, the radio and the screen need it too
                    concurrency::LockGuard g(spiLock);
                    got = f.read(frame, c_maxPayload);
                }
                if (got <= 0)
                    break;
                sum = crc32(sum, frame, got);
                total += got;
            }
            {
                concurrency::LockGuard g(spiLock);
                f.close();
            }
            uint8_t b[12];
            put32(b, sum);
            put64(b + 4, total);
            answer(Ack, sequence, b, 12);
            break;
        }
        case Read: {
            const std::string path = pathAt(payload, length, 8);
            if (length < 9 || !pathFits(path))
                return answer(Error, sequence, "that path is not allowed");
            const uint64_t offset = get64(payload);
            static uint8_t block[4000];
            int got = 0;
            {
                concurrency::LockGuard g(spiLock);
                if (!cardThere())
                    return answer(Error, sequence, "no card");
                File f = SD.exists(path.c_str()) ? SD.open(path.c_str()) : File();
                if (!f || f.isDirectory())
                    return answer(Error, sequence, "there is no such file");
                if (offset < f.size() && f.seek((uint32_t)offset))
                    got = f.read(block, sizeof(block));
                f.close();
            }
            answer(Ack, sequence, block, (uint16_t)(got > 0 ? got : 0));
            break;
        }
        case Tone: {
            static char tune[400]; // the player reads it while it plays
            const size_t count = length < sizeof(tune) - 1 ? length : sizeof(tune) - 1;
            memcpy(tune, payload, count);
            tune[count] = 0;
            const uint8_t sounds = jinxmesh_play(tune) ? 1 : 0;
            answer(Ack, sequence, &sounds, 1);
            break;
        }
        default:
            answer(Error, sequence, "unknown request");
        }
    }

    uint8_t *frame = nullptr; // head (4) and payload of the frame at hand
    File file;
    std::string name;
    uint64_t size = 0, position = 0;
    uint32_t unsynced = 0;
    bool done = false;
};

void task(void *)
{
    LOG_INFO("serial files: started");
    {
        Transfer transfer;
        transfer.run();
    }
    LOG_INFO("serial files: ended");
    active = false;
    vTaskDelete(nullptr);
}
} // namespace

bool jinxFilesByte(uint8_t c)
{
    if (active)
        return false;
    if (c == (uint8_t)c_hello[matched]) {
        if (++matched < sizeof(c_hello) - 1)
            return false;
        matched = 0;
        active = true;
        if (xTaskCreate(task, "jinxfiles", 8192, nullptr, 3, nullptr) != pdPASS) {
            active = false;
            return false;
        }
        return true;
    }
    matched = c == (uint8_t)c_hello[0] ? 1 : 0;
    return false;
}

bool jinxFilesActive(void)
{
    return active;
}

extern "C" int jinxmesh_transfer(char *name, unsigned nameSize, uint32_t *done, uint32_t *total)
{
    if (!receiving)
        return 0;
    if (name && nameSize)
        snprintf(name, nameSize, "%s", receivingName);
    if (done)
        *done = receivedBytes;
    if (total)
        *total = receivingBytes;
    return 1;
}

#else

#include "JinxFiles.h"

bool jinxFilesByte(uint8_t)
{
    return false;
}

bool jinxFilesActive(void)
{
    return false;
}

#endif
