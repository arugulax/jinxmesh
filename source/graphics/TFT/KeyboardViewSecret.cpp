#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardViewSecret.h"
#include "util/ILog.h"
#include <ChaChaPoly.h>
#include <Crypto.h>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_random.h>
#include <nvs.h>
#else
#include <random>
#if defined(ARCH_PORTDUINO)
#include "PortduinoFS.h"
#else
#include "LittleFS.h"
#endif
extern fs::FS &persistentFS;
#endif

namespace
{

constexpr size_t c_keySize = 32;
constexpr size_t c_nonceSize = 12;
constexpr size_t c_tagSize = 16;
constexpr size_t c_secretMax = 64;              // a WPA2 passphrase has up to 63 characters, a raw key 64
constexpr size_t c_blockSize = 1 + c_secretMax; // length and the secret, filled up: every secret seals to the same size
constexpr size_t c_sealedSize = c_nonceSize + c_blockSize + c_tagSize;
const char c_prefix[] = "v1:";                  // of a sealed line
const char c_context[] = "kbview secret v1";    // authenticated with it, so a line of another kind does not open

#if defined(ARDUINO_ARCH_ESP32)
const char *const c_namespace = "kbview";       // in NVS
const char *const c_keyName = "seal";
const char *const c_cleanName = "wificlean";
const char *const c_driverNamespace = "nvs.net80211"; // where the Wi-Fi driver keeps a network it was told to remember
#else
const char *const c_keyFile = "/kbview.key";    // no NVS here: the simulator has nothing to protect
#endif

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/**
 * The key of this device, made on first use.
 */
bool deviceKey(uint8_t key[c_keySize], bool create)
{
#if defined(ARDUINO_ARCH_ESP32)
    nvs_handle_t handle;
    if (nvs_open(c_namespace, create ? NVS_READWRITE : NVS_READONLY, &handle) != ESP_OK)
        return false;
    size_t size = c_keySize;
    bool ok = nvs_get_blob(handle, c_keyName, key, &size) == ESP_OK && size == c_keySize;
    if (!ok && create) {
        kbview::randomBytes(key, c_keySize);
        ok = nvs_set_blob(handle, c_keyName, key, c_keySize) == ESP_OK && nvs_commit(handle) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
#else
    char text[2 * c_keySize + 1] = "";
    if (persistentFS.exists(c_keyFile)) {
        File file = persistentFS.open(c_keyFile, FILE_READ);
        const bool whole = file && (size_t)file.read((uint8_t *)text, 2 * c_keySize) == 2 * c_keySize;
        if (file)
            file.close();
        if (whole) {
            for (size_t i = 0; i < c_keySize; i++) {
                const int high = hexValue(text[2 * i]), low = hexValue(text[2 * i + 1]);
                if (high < 0 || low < 0)
                    return false;
                key[i] = (uint8_t)(high * 16 + low);
            }
            return true;
        }
    }
    if (!create)
        return false;
    kbview::randomBytes(key, c_keySize);
    for (size_t i = 0; i < c_keySize; i++)
        snprintf(text + 2 * i, 3, "%02x", key[i]);
    File file = persistentFS.open(c_keyFile, FILE_WRITE);
    if (!file)
        return false;
    const bool written = (size_t)file.write((const uint8_t *)text, 2 * c_keySize) == 2 * c_keySize;
    file.close();
    return written;
#endif
}

} // namespace

namespace kbview
{

void randomBytes(uint8_t *bytes, size_t size)
{
#if defined(ARDUINO_ARCH_ESP32)
    esp_fill_random(bytes, size);
#else
    std::random_device source;
    for (size_t i = 0; i < size; i++)
        bytes[i] = (uint8_t)source();
#endif
}

std::string sealSecret(const std::string &secret)
{
    uint8_t key[c_keySize], block[c_blockSize] = {0}, sealed[c_sealedSize];
    if (secret.size() > c_secretMax)
        return "";
    if (!deviceKey(key, true)) {
        ILOG_ERROR("no key to seal a secret with");
        return "";
    }
    block[0] = (uint8_t)secret.size();
    memcpy(block + 1, secret.data(), secret.size());
    randomBytes(sealed, c_nonceSize);

    ChaChaPoly cipher;
    cipher.setKey(key, c_keySize);
    cipher.setIV(sealed, c_nonceSize);
    cipher.addAuthData(c_context, sizeof(c_context) - 1);
    cipher.encrypt(sealed + c_nonceSize, block, c_blockSize);
    cipher.computeTag(sealed + c_nonceSize + c_blockSize, c_tagSize);
    cipher.clear();
    clean(key);
    clean(block);

    std::string out = c_prefix;
    char hex[3];
    for (uint8_t byte : sealed) {
        snprintf(hex, sizeof(hex), "%02x", byte);
        out += hex;
    }
    return out;
}

bool openSecret(const std::string &sealed, std::string &secret)
{
    const size_t prefix = sizeof(c_prefix) - 1;
    uint8_t key[c_keySize], block[c_blockSize], bytes[c_sealedSize];
    if (sealed.size() != prefix + 2 * c_sealedSize || sealed.compare(0, prefix, c_prefix) != 0)
        return false;
    for (size_t i = 0; i < c_sealedSize; i++) {
        const int high = hexValue(sealed[prefix + 2 * i]), low = hexValue(sealed[prefix + 2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes[i] = (uint8_t)(high * 16 + low);
    }
    if (!deviceKey(key, false))
        return false;

    ChaChaPoly cipher;
    cipher.setKey(key, c_keySize);
    cipher.setIV(bytes, c_nonceSize);
    cipher.addAuthData(c_context, sizeof(c_context) - 1);
    cipher.decrypt(block, bytes + c_nonceSize, c_blockSize);
    const bool genuine = cipher.checkTag(bytes + c_nonceSize + c_blockSize, c_tagSize) && block[0] <= c_secretMax;
    if (genuine)
        secret.assign((const char *)block + 1, block[0]);
    cipher.clear();
    clean(key);
    clean(block);
    return genuine;
}

void forgetDriverWifi(void)
{
#if defined(ARDUINO_ARCH_ESP32)
    nvs_handle_t own, driver;
    uint8_t done = 0;
    if (nvs_open(c_namespace, NVS_READWRITE, &own) != ESP_OK)
        return;
    if (nvs_get_u8(own, c_cleanName, &done) != ESP_OK || !done) {
        if (nvs_open(c_driverNamespace, NVS_READWRITE, &driver) == ESP_OK) {
            nvs_erase_all(driver);
            nvs_commit(driver);
            nvs_close(driver);
            ILOG_INFO("removed what the Wi-Fi driver had stored");
        }
        nvs_set_u8(own, c_cleanName, 1);
        nvs_commit(own);
    }
    nvs_close(own);
#endif
}

} // namespace kbview

#endif
