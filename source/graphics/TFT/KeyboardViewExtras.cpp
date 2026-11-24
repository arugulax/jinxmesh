#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardViewSecret.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "input/KeyboardLight.h"
#include "util/ILog.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(ARCH_PORTDUINO)
#include "PortduinoFS.h"
#else
#include "LittleFS.h"
#endif
#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#endif

extern fs::FS &persistentFS;

using namespace kbview;

namespace
{

const char *const c_prefsFile = "/kbview.cfg";
const char *const c_prefsTemp = "/kbview.new";

// The Wi-Fi password goes into that file sealed (see KeyboardViewSecret.h). Sealing draws a fresh
// nonce, so the line is made again only when the password changed. One view, so one of each.
std::string sealedPassword, sealedFrom;
constexpr size_t c_prefsMax = 8192;        // bytes read of the file
constexpr uint32_t c_pulseInterval = 120;  // ms between two changes of the light while it flashes
constexpr uint8_t c_pulseChanges = 8;      // four flashes
constexpr uint32_t c_lightTime = 10000;    // ms the light stays on after a key in the automatic mode
constexpr size_t c_maxReactions = 200;     // kept per conversation
constexpr uint32_t c_defaultBrightness = 153;

// rows of the control center
enum ControlRow : uint32_t { RowLight, RowAlert, RowBrightness, RowTimeout, RowGps, RowBluetooth, RowWifi, RowLock, RowScreenOff };

const char *const c_lightNames[] = {"Off", "On", "Auto"};
const char *const c_alertNames[] = {"Sound", "Flash", "Sound + flash", "Silent"};

struct ReactionChoice {
    const char *send; // what goes on air
    const char *name;
};

const ReactionChoice c_reactions[] = {
    {"\xF0\x9F\x91\x8D", "Thumbs up"}, {"\xE2\x9D\xA4\xEF\xB8\x8F", "Heart"},   {"\xF0\x9F\x98\x82", "Laugh"},
    {"\xF0\x9F\x98\xAE", "Wow"},       {"\xF0\x9F\x98\xA2", "Sad"},             {"\xF0\x9F\x99\x8F", "Thanks"},
    {"\xF0\x9F\x91\x8E", "Thumbs down"}, {"\xF0\x9F\x94\xA5", "Fire"},          {"\xE2\x9C\x85", "Done"},
    {"\xF0\x9F\x8E\x89", "Party"},
};

/**
 * Next code point of a UTF-8 string, 0 at its end. Broken sequences come out as single bytes.
 */
uint32_t nextCodePoint(const char *&text)
{
    const unsigned char *p = (const unsigned char *)text;
    if (!*p)
        return 0;
    uint32_t code = *p;
    int more = 0;
    if (code >= 0xF0 && code < 0xF8) {
        code &= 0x07;
        more = 3;
    } else if (code >= 0xE0) {
        code &= 0x0F;
        more = 2;
    } else if (code >= 0xC0) {
        code &= 0x1F;
        more = 1;
    }
    p++;
    for (; more > 0 && (*p & 0xC0) == 0x80; more--)
        code = (code << 6) | (*p++ & 0x3F);
    text = (const char *)p;
    return code;
}

// the emoji a reaction stands for: its first code point that is not a modifier
uint32_t reactionCode(const char *text)
{
    while (uint32_t code = nextCodePoint(text)) {
        if (code != 0xFE0F && code != 0x200D && code > 0x20)
            return code;
    }
    return 0;
}

void appendUtf8(std::string &out, uint32_t code)
{
    if (code < 0x80) {
        out += (char)code;
    } else if (code < 0x800) {
        out += (char)(0xC0 | (code >> 6));
        out += (char)(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += (char)(0xE0 | (code >> 12));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    } else {
        out += (char)(0xF0 | (code >> 18));
        out += (char)(0x80 | ((code >> 12) & 0x3F));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
}

/**
 * What the font can show of an emoji: itself, a stand-in, or nothing (0).
 */
uint32_t drawnCode(uint32_t code, const lv_font_t *font)
{
    if (code == 0x2764)
        code = 0x2665; // the fonts carry the card suit heart only
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, code, 0) ? code : 0;
}

std::string drawnEmoji(const char *text, const lv_font_t *font)
{
    std::string out;
    const uint32_t code = drawnCode(reactionCode(text), font);
    if (code)
        appendUtf8(out, code);
    else
        out = "+";
    return out;
}

bool wordChar(unsigned char c)
{
    return isalnum(c) || c >= 0x80;
}

// name as a whole word of text, both already folded
bool namesWord(const std::string &text, const std::string &name)
{
    if (name.empty())
        return false;
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        const bool before = at == 0 || !wordChar((unsigned char)text[at - 1]);
        const bool after = at + name.size() >= text.size() || !wordChar((unsigned char)text[at + name.size()]);
        if (before && after)
            return true;
    }
    return false;
}

std::string trimmedText(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

} // namespace

// -------- message text --------

void KeyboardView_320x240::Text::assign(const char *text, size_t length)
{
    release();
    if (!text || !length)
        return;
#if defined(ARDUINO_ARCH_ESP32)
    data = (char *)heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM);
#endif
    if (!data)
        data = (char *)malloc(length + 1);
    if (!data)
        return;
    memcpy(data, text, length);
    data[length] = '\0';
}

void KeyboardView_320x240::Text::release(void)
{
    free(data);
    data = nullptr;
}

// -------- what the view keeps for itself --------

void KeyboardView_320x240::loadPrefs(void)
{
    bool clearPassword = false;
    forgetDriverWifi();
    if (!persistentFS.exists(c_prefsFile))
        return;
    File file = persistentFS.open(c_prefsFile, FILE_READ);
    if (!file)
        return;
    std::string content;
    uint8_t buf[256];
    while (content.size() < c_prefsMax) {
        const int got = (int)file.read(buf, sizeof(buf));
        if (got <= 0)
            break;
        content.append((const char *)buf, (size_t)got);
    }
    file.close();

    waypoints.clear();
    pins.clear();
    wifiKnown.clear();
    nodeColors.clear();
    ownedNodes.clear();
    for (size_t at = 0; at < content.size();) {
        size_t end = content.find('\n', at);
        if (end == std::string::npos)
            end = content.size();
        std::string line = content.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const int number = atoi(value.c_str());
        if (key == "keyLight")
            prefs.keyLight = (uint8_t)std::min(std::max(number, 0), 2);
        else if (key == "soundMode")
            prefs.soundMode = (uint8_t)std::clamp(number, 0, (int)meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY);
        else if (key == "keyFlash")
            prefs.keyFlash = number != 0;
        else if (key == "wake")
            prefs.wake = number != 0;
        else if (key == "screenSound")
            prefs.screenSound = number != 0;
        else if (key == "longNames")
            prefs.longNames = number != 0;
        else if (key == "quickOwn")
            prefs.quickOwn = number != 0;
        else if (key == "quick") {
            if (!value.empty() && prefs.quick.size() < c_maxQuick)
                prefs.quick.push_back(value.substr(0, c_quickLength));
        }
        else if (key == "borderWarn")
            prefs.borderWarn = number != 0;
        else if (key == "light")
            prefs.light = number != 0;
        else if (key == "tilecolors") {
            // per tile, in the order of the tiles, an index into the palette
            tileColors.fill(0);
            for (size_t tile = 0, at = 0; tile < tileColors.size() && at < value.size(); tile++, at++) {
                tileColors[tile] = (uint8_t)std::min<int>(atoi(value.c_str() + at), c_paletteSize - 1);
                at = value.find(',', at);
                if (at == std::string::npos)
                    break;
            }
        } else if (key == "nodecolor" && value.size() > 10 && value[0] == '!' && nodeColors.size() < 64) {
            const uint32_t node = (uint32_t)strtoul(value.c_str() + 1, nullptr, 16);
            const int color = atoi(value.c_str() + 10);
            if (node && color > 0 && color < c_paletteSize)
                nodeColors[node] = (uint8_t)color;
        } else if (key == "owned" && value.size() > 1 && value[0] == '!' && ownedNodes.size() < 32) {
            const uint32_t node = (uint32_t)strtoul(value.c_str() + 1, nullptr, 16);
            if (node)
                ownedNodes.insert(node);
        }
        else if (key == "declinationAuto")
            prefs.declinationAuto = number != 0;
        else if (key == "lockOnSleep")
            prefs.lockOnSleep = number != 0;
        else if (key == "breadcrumbs")
            prefs.breadcrumbs = number != 0;
        else if (key == "tileDownload")
            prefs.tileDownload = number != 0;
        else if (key == "wifi")
            prefs.wifi = number != 0;
        else if (key == "ssid")
            prefs.ssid = value;
        else if (key == "password") {
            // builds before 2026-11-19 wrote it in clear: taken over, and the line goes with the save below
            prefs.password = value;
            clearPassword = true;
        } else if (key == "wifisecret") {
            if (value.empty()) {
                prefs.password.clear();
            } else if (openSecret(value, prefs.password)) {
                sealedPassword = value;
                sealedFrom = prefs.password;
            } else {
                // the key is gone (flash erased) or the file is from another device: the line stays until a
                // password is typed, so that the Password row keeps asking for it
                passwordLost = true;
                sealedPassword = value;
                ILOG_WARN("stored Wi-Fi password cannot be opened");
            }
        }
        else if (key == "net") {
            // a saved network: the sealed password, a space, the name
            const size_t space = value.find(' ');
            KnownNetwork known;
            if (space != std::string::npos)
                known.ssid = value.substr(space + 1);
            const std::string sealed = value.substr(0, std::min(space, value.size()));
            if (!sealed.empty()) {
                known.sealed = sealed;
                if (openSecret(sealed, known.password))
                    known.sealedFrom = known.password;
                else
                    known.lost = true;
            }
            if (!known.ssid.empty() && wifiKnown.size() < c_maxNetworks)
                wifiKnown.push_back(std::move(known));
        } else if (key == "mapStyle")
            prefs.mapStyle = value;
        else if (key == "toneDirect")
            prefs.toneDirect = value;
        else if (key == "toneEmergency")
            prefs.toneEmergency = value;
        else if (key == "toneTimer")
            prefs.toneTimer = value;
        else if (key.size() == 12 && key.compare(0, 11, "toneChannel") == 0 && key[11] >= '0' && key[11] < '0' + c_max_channels)
            prefs.toneChannel[key[11] - '0'] = value;
        else if (key == "nearby")
            prefs.nearbyVectors = value == "vectors";
        else if (key == "callsign")
            prefs.callSign = value;
        else if (key == "trip")
            setTripLine(value);
        else if (key == "odometer") {
            const double meters = strtod(value.c_str(), nullptr);
            odometerMeters = meters >= 0 && meters < 1e9 ? meters : 0; // also not a number: every comparison fails
        }
        else if (key == "wardrive")
            prefs.wardrive = value == "1";
        else if (key == "mapLayersOff")
            prefs.mapLayersOff = (uint8_t)(number & 0x3F);
        else if (key == "wardrivePing")
            prefs.wardrivePing = (uint16_t)std::min<long>(std::max<long>(number, 0), 3600);
        else if (key == "wardriveChannel")
            prefs.wardriveChannel = (uint8_t)number;
        else if (key == "timer")
            prefs.timerLength = (uint32_t)std::clamp(number, 1, 359999);
        else if (key == "timerRepeat")
            prefs.timerRepeat = number != 0;
        else if (key == "declination") {
            prefs.declination = (int16_t)std::clamp<long>(number, -180, 180);
            prefs.declinationSet = true;
        } else if (key == "compassTurn")
            prefs.compassTurn = (uint16_t)(((number % 360) + 360) % 360);
        else if (key == "mapview")
            prefs.mapView = value;
        else if (key == "home")
            setHomeTiles(value);
        else if (key == "pin") {
            // '#' and a channel name, '=' and the slot of a channel without one, or '!' and a node number
            const bool valid = value.size() > 1 && (value[0] == '#' || value[0] == '!' ||
                                                    (value[0] == '=' && value.size() == 2 && isdigit((unsigned char)value[1])));
            if (valid && pins.size() < c_maxPins && std::find(pins.begin(), pins.end(), value) == pins.end())
                pins.push_back(value);
        }
        else if (key == "wp" && waypoints.size() < c_maxWaypoints) {
            Waypoint point{};
            int used = 0;
            if (sscanf(value.c_str(), "%u,%u,%d,%d,%u,%n", (unsigned *)&point.id, (unsigned *)&point.from, (int *)&point.latitude,
                       (int *)&point.longitude, (unsigned *)&point.expire, &used) >= 5 &&
                used > 0) {
                point.name = value.substr((size_t)used);
                waypoints.push_back(point);
            }
        }
    }
    resolveMapView();
    setTheme(prefs.light);
    if (!passwordLost)
        rememberNetwork(); // a file from before there was a list
    ILOG_INFO("view preferences loaded, %u waypoints", (unsigned)waypoints.size());
    if (clearPassword)
        savePrefs();
}

void KeyboardView_320x240::savePrefs(void)
{
    char buf[160];
    std::string out;
    snprintf(buf, sizeof(buf), "keyLight=%u\nsoundMode=%u\nkeyFlash=%u\nwake=%u\nlockOnSleep=%u\n", (unsigned)prefs.keyLight,
             (unsigned)prefs.soundMode, (unsigned)prefs.keyFlash, (unsigned)prefs.wake, (unsigned)prefs.lockOnSleep);
    out += buf;
    snprintf(buf, sizeof(buf), "breadcrumbs=%u\ntileDownload=%u\nwifi=%u\n", (unsigned)prefs.breadcrumbs,
             (unsigned)prefs.tileDownload, (unsigned)prefs.wifi);
    out += buf;
    snprintf(buf, sizeof(buf), "screenSound=%u\ndeclinationAuto=%u\nlight=%u\n", (unsigned)prefs.screenSound,
             (unsigned)prefs.declinationAuto, (unsigned)prefs.light);
    out += buf;
    snprintf(buf, sizeof(buf), "longNames=%u\nborderWarn=%u\n", (unsigned)prefs.longNames, (unsigned)prefs.borderWarn);
    out += buf;
    out += "toneDirect=" + prefs.toneDirect + "\ntoneEmergency=" + prefs.toneEmergency + "\ntoneTimer=" + prefs.toneTimer + "\n";
    for (uint8_t i = 0; i < c_max_channels; i++) {
        if (!prefs.toneChannel[i].empty())
            out += "toneChannel" + std::to_string(i) + "=" + prefs.toneChannel[i] + "\n";
    }
    if (prefs.quickOwn) {
        out += "quickOwn=1\n";
        for (const std::string &reply : prefs.quick)
            out += "quick=" + reply + "\n";
    }
    out += "tilecolors=";
    for (size_t tile = 0; tile < tileColors.size(); tile++)
        out += (tile ? "," : "") + std::to_string(tileColors[tile]);
    out += "\n";
    for (const auto &[node, color] : nodeColors) {
        snprintf(buf, sizeof(buf), "nodecolor=!%08x:%u\n", (unsigned)node, (unsigned)color);
        out += buf;
    }
    for (uint32_t node : ownedNodes) {
        snprintf(buf, sizeof(buf), "owned=!%08x\n", (unsigned)node);
        out += buf;
    }
    if (prefs.password.empty()) {
        if (!passwordLost)
            sealedPassword.clear();
    } else if (sealedPassword.empty() || prefs.password != sealedFrom) {
        // never in clear: when it cannot be sealed it is not stored and has to be typed again after a restart
        sealedPassword = sealSecret(prefs.password);
    }
    sealedFrom = prefs.password;
    out += "ssid=" + oneLine(prefs.ssid) + "\nwifisecret=" + sealedPassword + "\nmapStyle=" + oneLine(prefs.mapStyle) + "\n";
    for (KnownNetwork &known : wifiKnown) {
        if (!known.lost) {
            if (known.password.empty()) {
                known.sealed.clear();
            } else if (known.sealed.empty() || known.password != known.sealedFrom) {
                known.sealed = sealSecret(known.password);
                known.sealedFrom = known.password;
            }
            if (!known.password.empty() && known.sealed.empty())
                continue; // it cannot be sealed, so it is not stored
        }
        out += "net=" + known.sealed + " " + oneLine(known.ssid) + "\n";
    }
    out += "mapview=" + oneLine(prefs.mapView) + "\n";
    out += std::string("nearby=") + (prefs.nearbyVectors ? "vectors" : "list") + "\n";
    if (!prefs.callSign.empty())
        out += "callsign=" + oneLine(prefs.callSign) + "\n";
    out += "trip=" + tripLine() + "\n";
    snprintf(buf, sizeof(buf), "odometer=%.0f\nwardrive=%u\ncompassTurn=%u\n", odometerMeters, (unsigned)prefs.wardrive,
             (unsigned)prefs.compassTurn);
    out += buf;
    snprintf(buf, sizeof(buf), "wardrivePing=%u\nwardriveChannel=%u\nmapLayersOff=%u\n", (unsigned)prefs.wardrivePing,
             (unsigned)prefs.wardriveChannel, (unsigned)prefs.mapLayersOff);
    out += buf;
    if (prefs.declinationSet)
        out += "declination=" + std::to_string(prefs.declination) + "\n";
    snprintf(buf, sizeof(buf), "timer=%u\ntimerRepeat=%u\n", (unsigned)prefs.timerLength, (unsigned)prefs.timerRepeat);
    out += buf;
    out += "home=" + homeLine() + "\n";
    for (const std::string &pin : pins)
        out += "pin=" + oneLine(pin) + "\n";
    for (const Waypoint &point : waypoints) {
        snprintf(buf, sizeof(buf), "wp=%u,%u,%d,%d,%u,", (unsigned)point.id, (unsigned)point.from, (int)point.latitude,
                 (int)point.longitude, (unsigned)point.expire);
        out += buf + oneLine(point.name) + "\n";
    }
    // Written beside the old file and put in its place only when all of it is there: a full file system or a
    // write that fails must not cost the settings that were stored.
    File file = persistentFS.open(c_prefsTemp, FILE_WRITE);
    if (!file) {
        ILOG_ERROR("cannot write %s", c_prefsTemp);
        return;
    }
    const size_t written = file.write((const uint8_t *)out.data(), out.size());
    file.close();
    if (written != out.size()) {
        ILOG_ERROR("%s: %u of %u bytes written, the stored settings stay", c_prefsTemp, (unsigned)written, (unsigned)out.size());
        persistentFS.remove(c_prefsTemp);
        return;
    }
    if (!persistentFS.rename(c_prefsTemp, c_prefsFile)) {
        // a file system that does not put a file over another one
        persistentFS.remove(c_prefsFile);
        if (!persistentFS.rename(c_prefsTemp, c_prefsFile))
            ILOG_ERROR("cannot put %s in place", c_prefsFile);
    }
}

// -------- keyboard light and alerts --------

/**
 * Keeps the keyboard light in step with its mode; a message alert flashes it whatever the mode.
 */
void KeyboardView_320x240::lightTask(void)
{
    if (!KeyboardLight::available())
        return;
    if (pulseLeft) {
        if (lv_tick_elaps(pulseTime) >= c_pulseInterval) {
            pulseTime = lv_tick_get();
            pulseLeft--;
            KeyboardLight::set(pulseLeft ? !KeyboardLight::isOn() : false);
        }
        if (pulseLeft)
            return;
    }
    bool on = false;
    if (!screenOff && !locked) {
        if (prefs.keyLight == 1)
            on = true;
        else if (prefs.keyLight == 2)
            on = lv_display_get_inactive_time(NULL) < c_lightTime;
    }
    if (on != KeyboardLight::isOn())
        KeyboardLight::set(on);
}

/**
 * The display went to sleep or woke up.
 */
void KeyboardView_320x240::screenSaving(bool enabled)
{
    if (screenOff != enabled && prefs.screenSound)
        screenSound(!enabled);
    screenOff = enabled;
    offDrawn = false;
    screenOffAt = lv_tick_get();
    if (!enabled && wifiParked) {
        wifiParked = false;
        applyWifi();
    }
    if (enabled && prefs.lockOnSleep && !restarting && !bluetoothMode)
        lockScreen();
    if (!enabled && inConversation && !locked)
        chatFor(convKey).unread = 0; // what arrived while it was dark is on screen now
    dirty = urgent = true;
}

/**
 * A message for the chat arrived: flash the keys and wake the screen, as far as the user wants it.
 */
void KeyboardView_320x240::alertMessage(uint32_t key, bool evenMuted)
{
    if (chatMuted(key) && !(evenMuted && alertMode() != 3))
        return;
    if (prefs.keyFlash && KeyboardLight::available() && !pulseLeft) {
        pulseLeft = c_pulseChanges;
        pulseTime = lv_tick_get() - c_pulseInterval;
    }
    if (prefs.wake && screenOff)
        displaydriver->forceWakeup();
}

// 0 sound, 1 flash, 2 both, 3 silent
uint8_t KeyboardView_320x240::alertMode(void) const
{
    const bool sound = !silent(), flash = prefs.keyFlash && KeyboardLight::available();
    return sound ? (flash ? 2 : 0) : (flash ? 1 : 3);
}

const char *KeyboardView_320x240::alertName(void) const
{
    return c_alertNames[alertMode()];
}

void KeyboardView_320x240::setAlertMode(uint8_t mode)
{
    const bool wantSound = mode == 0 || mode == 2, wantFlash = mode == 1 || mode == 2;
    if (hasDevice) {
        if (!silent())
            prefs.soundMode = (uint8_t)device.buzzer_mode; // what "sound" means to this user
        if (wantSound == silent()) {
            auto mode = (meshtastic_Config_DeviceConfig_BuzzerMode)prefs.soundMode;
            if (mode == meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED ||
                mode == meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY)
                mode = meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED;
            device.buzzer_mode = wantSound ? mode : meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
            // the radio takes this one without a restart
            controller->sendConfig(meshtastic_Config_DeviceConfig{device}, ownNode);
        }
    }
    if (wantFlash && !prefs.keyFlash) {
        // show what it will look like
        pulseLeft = c_pulseChanges;
        pulseTime = lv_tick_get() - c_pulseInterval;
    }
    prefs.keyFlash = wantFlash;
    savePrefs();
    dirty = true;
}

// -------- control center --------

/**
 * Quick switches on the CTRL key. Left and right change the focused row, OK steps it forward.
 */
void KeyboardView_320x240::openControl(size_t focus)
{
    char buf[48];
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    auto add = [&items, &values](const std::string &name, const std::string &value, uint32_t row) {
        items.push_back(value.empty() ? name : name + ": " + value);
        values.push_back(row);
    };
    add("Turn the screen off", "", RowScreenOff);
    if (KeyboardLight::available())
        add("Keyboard light", c_lightNames[std::min<uint8_t>(prefs.keyLight, 2)], RowLight);
    add("Alert", hasDevice ? c_alertNames[alertMode()] : "", RowAlert);
    const uint32_t brightness = uiConfig.screen_brightness ? uiConfig.screen_brightness : c_defaultBrightness;
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)((brightness * 100 + 127) / 255));
    add("Brightness", buf, RowBrightness);
    add("Screen off after", durationName(uiConfig.screen_timeout), RowTimeout);
    add("GPS",
        !hasPosition                                                             ? ""
        : position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED  ? "On"
        : position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_DISABLED ? "Off"
                                                                                 : "Not present",
        RowGps);
    add("Bluetooth", !hasBluetooth ? "" : bluetooth.enabled ? "On" : "Off", RowBluetooth);
    add("Wi-Fi", network.wifi_enabled ? "Radio" : !prefs.wifi ? "Off" : wifiUp ? "On" : "Connecting", RowWifi);
    add("Lock the screen", "", RowLock);

    std::vector<uint32_t> rows = values;
    openPicker(Setting::Control, "Control center", std::move(items), std::move(values), -1, "Left and Right change, OK steps");
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i] == focus)
            popup.focus = i;
    }
}

void KeyboardView_320x240::controlStep(uint32_t row, int32_t step)
{
    switch (row) {
    case RowLight:
        prefs.keyLight = (uint8_t)((prefs.keyLight + 3 + step) % 3);
        savePrefs();
        break;
    case RowAlert:
        if (!loaded(hasDevice))
            break;
        if (KeyboardLight::available())
            setAlertMode((uint8_t)((alertMode() + 4 + step) % 4));
        else
            setAlertMode(silent() ? 0 : 3);
        break;
    case RowBrightness: {
        const uint32_t brightness = uiConfig.screen_brightness ? uiConfig.screen_brightness : c_defaultBrightness;
        int32_t pct = (int32_t)((brightness * 100 + 127) / 255);
        pct = (pct + 5) / 10 * 10 + step * 10;
        applyPick(Setting::Brightness, (uint32_t)std::min<int32_t>(100, std::max<int32_t>(10, pct)), 0);
        break;
    }
    case RowTimeout: {
        const int32_t count = (int32_t)(sizeof(c_timeouts) / sizeof(c_timeouts[0]));
        int32_t at = 0;
        for (int32_t i = 0; i < count; i++) {
            if (c_timeouts[i] == uiConfig.screen_timeout)
                at = i;
        }
        applyPick(Setting::ScreenTimeout, c_timeouts[(at + count + step) % count], 0);
        break;
    }
    case RowGps:
        // these two restart the radio, so they go through their own questions
        closeMenu();
        activateSetting(SettingRow{Setting::Gps, "", ""});
        return;
    case RowBluetooth:
        closeMenu();
        activateSetting(SettingRow{Setting::Bluetooth, "", ""});
        return;
    case RowWifi:
        if (network.wifi_enabled) {
            toast("Wi-Fi is run by the radio");
            break;
        }
        if (!prefs.wifi && prefs.ssid.empty()) {
            closeMenu();
            openSettings(Page::Wifi, Setting::WifiNetwork);
            toast("Choose a network first");
            return;
        }
        prefs.wifi = !prefs.wifi;
        savePrefs();
        applyWifi();
        break;
    case RowLock:
        lockScreen();
        return;
    case RowScreenOff:
        closeMenu();
        screenOffNow();
        return;
    default:
        break;
    }
    openControl(row);
}

// -------- lock screen --------

void KeyboardView_320x240::lockScreen(void)
{
    closeMenu();
    locked = true;
    dirty = urgent = true;
}

/**
 * Screen and backlight off right now, whatever the timeout says. Any key brings them back; the radio
 * goes on working. The lock follows as it does when the screen goes off by itself.
 */
void KeyboardView_320x240::screenOffNow(void)
{
    if (screenOff)
        return;
    displaydriver->blankNow();
}

/**
 * Clock, what is waiting and how to get in. Only holding OK unlocks.
 */
void KeyboardView_320x240::renderLock(void)
{
    lv_obj_t *panel = box(overlay, 0, 0, c_width, c_height, c_ground);
    char buf[48];
    const uint32_t time = now();
    if (validTime(time)) {
        time_t tt = time;
        struct tm tm;
        localtime_r(&tt, &tm);
        clockString(tm, buf, sizeof(buf), twelveHour());
        label(panel, 0, 62, c_width, font20, c_text, buf, LV_TEXT_ALIGN_CENTER);
        dateString(tm, buf, sizeof(buf), true, true);
        label(panel, 0, 92, c_width, font12, c_muted, buf, LV_TEXT_ALIGN_CENTER);
    } else {
        label(panel, 0, 70, c_width, font20, c_text, "Locked", LV_TEXT_ALIGN_CENTER);
    }
    if (hasOwnUser)
        label(panel, 0, 16, c_width, font12, c_muted, ownUser.long_name, LV_TEXT_ALIGN_CENTER);
    const uint32_t unread = totalUnread();
    if (unread) {
        snprintf(buf, sizeof(buf), unread == 1 ? "%u new message" : "%u new messages", (unsigned)unread);
        label(panel, 0, 128, c_width, font16, c_focus, buf, LV_TEXT_ALIGN_CENTER);
    } else {
        label(panel, 0, 130, c_width, font12, c_muted, "No new messages", LV_TEXT_ALIGN_CENTER);
    }
    label(panel, 0, 200, c_width, font12, c_focusMuted, "Hold OK to unlock", LV_TEXT_ALIGN_CENTER);
}

// -------- reactions --------

/**
 * Remembers a tapback. A sender has one reaction per message; a new one replaces it.
 */
void KeyboardView_320x240::addReaction(uint32_t key, uint32_t target, uint32_t from, const char *emoji)
{
    const uint32_t code = emoji ? reactionCode(emoji) : 0;
    const auto it = conversations.find(key);
    if (!code || !target || it == conversations.end())
        return;
    Chat &chat = it->second;
    for (Reaction &reaction : chat.reactions) {
        if (reaction.target == target && reaction.from == from) {
            reaction.emoji = code;
            dirty = true;
            return;
        }
    }
    if (chat.reactions.size() >= c_maxReactions)
        chat.reactions.erase(chat.reactions.begin());
    chat.reactions.push_back(Reaction{target, from, code});
    dirty = true;
}

/**
 * The line below a message: each emoji once, with a count when several nodes sent it.
 */
std::string KeyboardView_320x240::reactionsOf(const Chat &chat, uint32_t id) const
{
    std::vector<std::pair<uint32_t, uint32_t>> counts;
    for (const Reaction &reaction : chat.reactions) {
        if (reaction.target != id)
            continue;
        auto known = std::find_if(counts.begin(), counts.end(),
                                  [&reaction](const std::pair<uint32_t, uint32_t> &c) { return c.first == reaction.emoji; });
        if (known == counts.end())
            counts.emplace_back(reaction.emoji, 1);
        else
            known->second++;
    }
    std::string out;
    char buf[12];
    for (const auto &[emoji, count] : counts) {
        if (!out.empty())
            out += "  ";
        const uint32_t code = drawnCode(emoji, font14);
        if (code)
            appendUtf8(out, code);
        else
            out += "+";
        if (count > 1) {
            snprintf(buf, sizeof(buf), " %u", (unsigned)count);
            out += buf;
        }
    }
    return out;
}

void KeyboardView_320x240::newReaction(uint32_t from, uint32_t to, uint8_t ch, const char *emoji, uint32_t &msgtime, uint32_t id,
                                       uint32_t replyId)
{
    if (to == UINT32_MAX && ch >= c_max_channels)
        return;
    const bool own = from == ownNode;
    const uint32_t key = to == UINT32_MAX ? ch : own ? to : from;
    if (replyId && findMessage(key, replyId)) {
        addReaction(key, replyId, from, emoji);
        return;
    }
    // the message it is for is not here any more: show it as what it is, a short message
    newTextMessage(from, to, ch, emoji, msgtime, id, replyId);
}

/**
 * Hold OK on a picked message.
 */
void KeyboardView_320x240::openMessageMenu(void)
{
    if (!inConversation)
        return;
    const Chat &chat = chatFor(convKey);
    if (convSelect < 0 || convSelect >= (int32_t)chat.messages.size())
        return;
    const Message &msg = chat.messages[convSelect];
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    if (msg.id) {
        items = {"Reply", "React", "Details"};
        values = {1, 2, 3};
    } else {
        items = {"Details"};
        values = {3};
    }
    mecpMessageMenu(msg, items, values);
    const std::string title = msg.own ? std::string("My message") : "Message from " + senderName(msg.from);
    openPicker(Setting::MessageMenu, title.c_str(), std::move(items), std::move(values), -1, "", (uint32_t)convSelect);
}

// -------- mentions and search --------

/**
 * A message names this node: its short name as a word, or its long name after an @.
 */
bool KeyboardView_320x240::mentionsMe(const Message &msg) const
{
    if (!hasOwnUser || msg.own || msg.text.empty())
        return false;
    const std::string text = folded(msg.text.c_str());
    const std::string shortOwn = folded(ownUser.short_name), longOwn = folded(ownUser.long_name);
    if (namesWord(text, shortOwn))
        return true;
    return !longOwn.empty() && (text.find("@" + longOwn) != std::string::npos || (longOwn.size() >= 4 && namesWord(text, longOwn)));
}

void KeyboardView_320x240::openMentions(void)
{
    jumps.clear();
    for (const auto &[key, chat] : conversations) {
        for (const Message &msg : chat.messages) {
            if (mentionsMe(msg))
                jumps.push_back(Jump{key, msg.id, msg.time});
        }
    }
    openJumps(Setting::Mentions, "Mentions of you", "Nobody has mentioned you");
}

void KeyboardView_320x240::openSearch(const std::string &text)
{
    openEditor(Setting::Search, "Search messages", text, 40, "Word or name", "Looks through every chat");
}

/**
 * List of the messages in jumps, newest first; OK opens the chat at the message.
 */
void KeyboardView_320x240::openJumps(Setting what, const char *title, const char *none)
{
    if (jumps.empty()) {
        toast(none);
        return;
    }
    std::stable_sort(jumps.begin(), jumps.end(), [](const Jump &a, const Jump &b) { return a.time > b.time; });
    if (jumps.size() > c_maxJumps)
        jumps.resize(c_maxJumps);
    std::vector<std::string> items;
    char when[16];
    for (const Jump &jump : jumps) {
        std::string item;
        const auto it = conversations.find(jump.key);
        if (it != conversations.end()) {
            for (auto msg = it->second.messages.rbegin(); msg != it->second.messages.rend(); ++msg) {
                if (msg->id != jump.id || msg->time != jump.time)
                    continue;
                chatTimeString(msg->time, now(), when, sizeof(when), twelveHour());
                item = std::string(when) + " " + (msg->own ? std::string("Me") : senderName(msg->from)) + ": " +
                       oneLine(msg->text.c_str());
                break;
            }
        }
        items.push_back(item);
    }
    char note[48];
    snprintf(note, sizeof(note), jumps.size() == 1 ? "%u message, OK opens it" : "%u messages, OK opens one", (unsigned)jumps.size());
    openPicker(what, title, std::move(items), {}, -1, note);
}

void KeyboardView_320x240::openChatAt(const Jump &to)
{
    const auto it = conversations.find(to.key);
    if (it == conversations.end())
        return;
    openChat(to.key);
    const std::vector<Message> &messages = it->second.messages;
    for (int32_t i = (int32_t)messages.size() - 1; i >= 0; i--) {
        if (messages[i].id == to.id && messages[i].time == to.time) {
            convSelect = i;
            break;
        }
    }
}

/**
 * First message to draw of the open conversation. Only a window of a long history is built;
 * it follows the newest message, or the picked one when the user walks up.
 */
int32_t KeyboardView_320x240::conversationTop(int32_t count)
{
    const int32_t shown = (int32_t)c_shownMessages;
    const int32_t last = std::max<int32_t>(0, count - shown);
    if (convSelect < 0)
        convTop = last;
    else if (convSelect < convTop)
        convTop = convSelect - shown / 2;
    else if (convSelect >= convTop + shown)
        convTop = convSelect - shown / 2 + 1;
    convTop = std::min(last, std::max<int32_t>(0, convTop));
    return convTop;
}

bool KeyboardView_320x240::chatMuted(uint32_t key) const
{
    return key < c_max_channels ? channelInfo[key].muted : mutedNodes.count(key) > 0;
}

/**
 * The last readings of a neighbor as a chart and a list, newest first.
 */
void KeyboardView_320x240::openSignalHistory(uint32_t nodeNum)
{
    const auto log = signalLog.find(nodeNum);
    if (log == signalLog.end() || log->second.empty()) {
        toast("Not heard directly yet");
        return;
    }
    const std::vector<SignalSample> samples = log->second;
    std::vector<uint8_t> bars;
    std::vector<std::string> lines;
    char buf[64], when[16];
    float low = samples.front().snr, high = low, sum = 0.0f;
    int rssiLow = samples.front().rssi, rssiHigh = rssiLow;
    for (const SignalSample &sample : samples) {
        // -20 dB is the floor of what LoRa decodes, +10 dB is as good as it gets
        const float level = (sample.snr + 20.0f) * 100.0f / 30.0f;
        bars.push_back((uint8_t)std::min(100.0f, std::max(0.0f, level)));
        low = std::min(low, sample.snr);
        high = std::max(high, sample.snr);
        sum += sample.snr;
        rssiLow = std::min<int>(rssiLow, sample.rssi);
        rssiHigh = std::max<int>(rssiHigh, sample.rssi);
    }
    snprintf(buf, sizeof(buf), "SNR %.1f to %.1f dB, average %.1f", low, high, sum / (float)samples.size());
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "RSSI %d to %d dBm · %u readings", rssiLow, rssiHigh, (unsigned)samples.size());
    lines.push_back(buf);
    for (auto sample = samples.rbegin(); sample != samples.rend(); ++sample) {
        if (validTime(sample->time))
            clockString(sample->time, when, sizeof(when), twelveHour());
        else
            snprintf(when, sizeof(when), "--:--");
        snprintf(buf, sizeof(buf), "%s   SNR %.1f dB   RSSI %d dBm", when, sample->snr, (int)sample->rssi);
        lines.push_back(buf);
    }
    openInfo(("Signal of " + chatName(nodeNum)).c_str(), std::move(lines));
    popup.bars = std::move(bars);
    popup.what = Setting::NodeMenu; // OK leads back to the actions of the node
    popup.target = nodeNum;
}

// -------- hooks of the settings and the menus --------

/**
 * Choices made in the popups of this file, the map and the files; false passes the pick on.
 */
bool KeyboardView_320x240::extraPick(Setting what, uint32_t value, uint32_t target)
{
    switch (what) {
    case Setting::QuickList:
        if (value == 1001) {
            prefs.quickOwn = false;
            prefs.quick.clear();
            savePrefs();
            toast("The built-in replies are back");
            openQuickList();
        } else if (value == 1000) {
            openEditor(Setting::QuickText, "New quick reply", "", c_quickLength, "Text", "", UINT32_MAX);
        } else if (value < quickReplies().size()) {
            std::vector<std::string> items = {"Change the text"};
            std::vector<uint32_t> values = {1};
            if (value > 0) {
                items.push_back("Move up");
                values.push_back(2);
            }
            if (value + 1 < quickReplies().size()) {
                items.push_back("Move down");
                values.push_back(3);
            }
            items.push_back("Remove");
            values.push_back(4);
            openPicker(Setting::QuickItem, quickReplies()[value].c_str(), std::move(items), std::move(values), -1, "", value);
        }
        return true;
    case Setting::QuickItem: {
        std::vector<std::string> replies = quickReplies();
        if (target >= replies.size())
            return true;
        if (value == 1) {
            openEditor(Setting::QuickText, "Quick reply", replies[target], c_quickLength, "Text", "Empty removes it", target);
            return true;
        }
        int32_t focus = (int32_t)target;
        if (value == 2 && target > 0) {
            std::swap(replies[target], replies[target - 1]);
            focus--;
        } else if (value == 3 && target + 1 < replies.size()) {
            std::swap(replies[target], replies[target + 1]);
            focus++;
        } else if (value == 4) {
            replies.erase(replies.begin() + target);
        }
        prefs.quick = std::move(replies);
        prefs.quickOwn = true;
        savePrefs();
        openQuickList(focus);
        return true;
    }
    case Setting::Control:
        controlStep(value, 1);
        return true;
    case Setting::KeyLight:
        prefs.keyLight = (uint8_t)std::min<uint32_t>(value, 2);
        savePrefs();
        return true;
    case Setting::AlertMode:
        setAlertMode((uint8_t)value);
        return true;
    case Setting::MessageMenu: {
        if (!inConversation)
            return true;
        Chat &chat = chatFor(convKey);
        if (target >= chat.messages.size())
            return true;
        const Message &msg = chat.messages[target];
        if (mecpMessagePick(value, msg))
            return true;
        if (value == 1 && msg.id) {
            chat.replyTo = msg.id;
            convSelect = -1;
        } else if (value == 2 && msg.id) {
            std::vector<std::string> items;
            for (const ReactionChoice &choice : c_reactions)
                items.push_back(drawnEmoji(choice.send, font14) + "  " + choice.name);
            openPicker(Setting::React, "React with", std::move(items), {}, -1, "", msg.id);
        } else if (value == 3) {
            char buf[64], when[32];
            std::vector<std::string> lines;
            lines.push_back(msg.own ? std::string("From me") : "From " + chatName(msg.from));
            if (validTime(msg.time)) {
                time_t tt = msg.time;
                struct tm tm;
                localtime_r(&tt, &tm);
                dateString(tm, when, sizeof(when), true);
                snprintf(buf, sizeof(buf), "%s ", when);
                if (twelveHour())
                    snprintf(when, sizeof(when), "%d:%02d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_sec,
                             tm.tm_hour < 12 ? "AM" : "PM");
                else
                    strftime(when, sizeof(when), "%H:%M:%S", &tm);
                lines.push_back(std::string(buf) + when);
            }
            if (msg.own && !messageState(convKey, msg, false).empty())
                lines.push_back(messageState(convKey, msg, false));
            if (msg.id) {
                snprintf(buf, sizeof(buf), "Packet %08x", (unsigned)msg.id);
                lines.push_back(buf);
            } else {
                lines.push_back("Stored without a packet id");
            }
            if (msg.replyId) {
                const std::string quote = quoteOf(convKey, msg.replyId, true);
                lines.push_back(quote.empty() ? std::string("Answers a message that is gone") : "Answers " + quote);
            }
            size_t reactions = 0;
            for (const Reaction &reaction : chat.reactions) {
                if (msg.id && reaction.target == msg.id) {
                    std::string who = reaction.from == ownNode ? std::string("Me") : chatName(reaction.from);
                    if (!reactions)
                        lines.push_back("Reactions:");
                    std::string mark;
                    const uint32_t code = drawnCode(reaction.emoji, font12);
                    if (code)
                        appendUtf8(mark, code);
                    else
                        mark = "+";
                    lines.push_back(mark + "  " + who);
                    reactions++;
                }
            }
            snprintf(buf, sizeof(buf), "%u bytes", (unsigned)strlen(msg.text.c_str()));
            lines.push_back(buf);
            openInfo("Message", std::move(lines));
        }
        return true;
    }
    case Setting::React: {
        if (!inConversation || value >= sizeof(c_reactions) / sizeof(c_reactions[0]) || !findMessage(convKey, target))
            return true;
        uint32_t to = UINT32_MAX;
        uint8_t ch = 0;
        const uint8_t maxHops = lora.hop_limit ? lora.hop_limit : 3;
        uint8_t hopLimit = maxHops;
        bool usePkc = false;
        if (convKey < c_max_channels) {
            ch = (uint8_t)convKey;
        } else {
            const NodeRecord *node = nodeStore.find(convKey);
            to = convKey;
            ch = node ? node->channel : 0;
            usePkc = node && node->hasKey;
            if (node && node->hopsAway >= 0)
                hopLimit = node->hopsAway < maxHops ? node->hopsAway + 1 : node->hopsAway;
        }
        const uint32_t requestId = requests.addRequest(convKey < c_max_channels ? ch : to, ResponseHandler::TextMessageRequest,
                                                       (void *)(uintptr_t)convKey);
        controller->sendReaction(to, ch, hopLimit, now(), requestId, usePkc, c_reactions[value].send, target);
        addReaction(convKey, target, ownNode, c_reactions[value].send);
        return true;
    }
    case Setting::Mentions:
    case Setting::SearchResults:
        if (value < jumps.size()) {
            const Jump to = jumps[value];
            openChatAt(to);
        }
        return true;
    default:
        return mapPick(what, value, target) || filesPick(what, value, target) || appsPick(what, value, target) ||
               mecpPick(what, value, target) || tripPick(what, value, target) ||
               compassPick(what, value, target) || clockPick(what, value, target) || routePick(what, value, target);
    }
}

/**
 * Text entered in the editors of this file, the map and the files; false passes it on.
 */
bool KeyboardView_320x240::extraEdit(Setting what, const std::string &text, uint32_t target)
{
    if (what == Setting::QuickText) {
        // one line, and nothing that the file of the preferences would read as something else
        std::string reply = trimmedText(text);
        std::replace(reply.begin(), reply.end(), '\n', ' ');
        std::vector<std::string> replies = quickReplies();
        int32_t focus = -1;
        if (target < replies.size()) {
            if (reply.empty())
                replies.erase(replies.begin() + target);
            else
                replies[target] = reply;
            focus = (int32_t)target;
        } else if (!reply.empty() && replies.size() < c_maxQuick) {
            replies.push_back(reply);
            focus = (int32_t)replies.size() - 1;
        } else {
            openQuickList();
            return true;
        }
        prefs.quick = std::move(replies);
        prefs.quickOwn = true;
        savePrefs();
        openQuickList(focus);
        return true;
    }
    if (what != Setting::Search)
        return mapEdit(what, text, target) || filesEdit(what, text, target) || mecpEdit(what, text, target) ||
               compassEdit(what, text, target) || routeEdit(what, text, target);

    const std::string needle = folded(trimmedText(text).c_str());
    if (needle.empty())
        return true;
    jumps.clear();
    for (const auto &[key, chat] : conversations) {
        for (const Message &msg : chat.messages) {
            if (folded(msg.text.c_str()).find(needle) != std::string::npos ||
                (!msg.own && folded(shortName(msg.from).c_str()) == needle))
                jumps.push_back(Jump{key, msg.id, msg.time});
        }
    }
    const std::string title = "Found \"" + trimmedText(text) + "\"";
    openJumps(Setting::SearchResults, title.c_str(), "Nothing found");
    return true;
}

/**
 * Rows of the settings that this file, the map or the files look after; false passes the row on.
 */
bool KeyboardView_320x240::extraSetting(const SettingRow &row)
{
    switch (row.id) {
    case Setting::KeyLight:
        openPicker(Setting::KeyLight, "Keyboard light", {c_lightNames[0], c_lightNames[1], c_lightNames[2]}, {}, prefs.keyLight,
                   "Auto lights the keys for 10 s after a key press");
        return true;
    case Setting::AlertMode: {
        // the row of the Sounds page: the keyboard light for a message, on or off; the sound has its own rows
        if (!loaded(hasDevice))
            return true;
        const bool sound = !silent(), flash = !prefs.keyFlash;
        setAlertMode(sound ? (flash ? 2 : 0) : (flash ? 1 : 3));
        return true;
    }
    case Setting::WakeOnMessage:
        prefs.wake = !prefs.wake;
        savePrefs();
        return true;
    case Setting::LockOnSleep:
        prefs.lockOnSleep = !prefs.lockOnSleep;
        savePrefs();
        return true;
    case Setting::ChatNames:
        prefs.longNames = !prefs.longNames;
        savePrefs();
        dirty = true;
        return true;
    case Setting::QuickList:
        openQuickList();
        return true;
    case Setting::BorderWarning:
        prefs.borderWarn = !prefs.borderWarn;
        borderStep = 0;
        savePrefs();
        dirty = true;
        return true;
    case Setting::Theme:
        prefs.light = !prefs.light;
        setTheme(prefs.light);
        mapThemeChanged();
        savePrefs();
        // what is not drawn anew with every pass takes the look when it is made again
        for (auto it = markers.begin(); it != markers.end();) {
            const uint32_t node = it->first;
            ++it;
            dropMarker(node);
            placeMarker(node);
        }
        dirty = urgent = true;
        return true;
    default:
        return filesSetting(row);
    }
}

/**
 * Rows this file adds to the pages of the settings.
 */
void KeyboardView_320x240::extraRows(Page of, std::vector<SettingRow> &rows) const
{
    switch (of) {
    case Page::Root: {
        std::string value = network.wifi_enabled ? "Wi-Fi by the radio" : !prefs.wifi ? "Wi-Fi off" : wifiUp ? wifiAddress() : "Connecting";
        if (uploading)
            value = "Receiving · " + wifiAddress();
        auto system = std::find_if(rows.begin(), rows.end(), [](const SettingRow &row) { return row.id == Setting::PageSystem; });
        rows.insert(system, SettingRow{Setting::PageWifi, "Wi-Fi and files", value});
        break;
    }
    case Page::Display:
        if (KeyboardLight::available())
            rows.push_back({Setting::KeyLight, "Keyboard light", c_lightNames[std::min<uint8_t>(prefs.keyLight, 2)]});
        rows.push_back({Setting::LockOnSleep, "Lock when the screen sleeps", prefs.lockOnSleep ? "On" : "Off"});
        rows.push_back({Setting::ChatNames, "Names in chats", prefs.longNames ? "Long" : "Short"});
        rows.push_back({Setting::QuickList, "Quick replies", std::to_string(quickReplies().size())});
        rows.insert(rows.begin(), SettingRow{Setting::Theme, "Look", prefs.light ? "Light" : "Dark"});
        break;
    case Page::Notifications: {
        std::vector<SettingRow> added;
        // behind the rows about sound, before the channels
        if (KeyboardLight::available())
            added.push_back({Setting::AlertMode, "Flash the keys for a message", prefs.keyFlash ? "On" : "Off"});
        added.push_back({Setting::WakeOnMessage, "Wake the screen for a message", prefs.wake ? "On" : "Off"});
        added.push_back({Setting::BorderWarning, "Warn near a country's border", prefs.borderWarn ? "On" : "Off"});
        auto channel = std::find_if(rows.begin(), rows.end(), [](const SettingRow &row) { return row.id == Setting::ChannelMute; });
        rows.insert(channel, added.begin(), added.end());
        break;
    }
    case Page::Wifi:
        filesRows(rows);
        break;
    default:
        break;
    }
}

#endif
