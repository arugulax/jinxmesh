#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/common/LoRaPresets.h"
#include "graphics/common/Ringtones.h"
#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardViewSecret.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "mesh-pb-constants.h"
#include "meshtastic/apponly.pb.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

const char *keyboardViewBuildStamp(); // KeyboardViewBuildStamp.cpp

using namespace kbview;

namespace
{

const char c_homePage[] = "jinxmesh.com";

// What the GNU General Public License asks an interactive program to show (version 3, sections 0 and 5),
// and whose work this is built on. Lines as they stand on a 320 pixel screen.
const char *const c_legal[] = {
    "",
    "LICENSE",
    "JinxMesh is free software under the GNU",
    "General Public License, version 3. You may",
    "use, change and pass it on under that",
    "license. It comes with NO WARRANTY, to the",
    "extent the law permits.",
    "License text: gnu.org/licenses/gpl-3.0",
    "Source code for this build: jinxmesh.com",
    "",
    "BASED ON",
    "Meshtastic firmware 2.8.3, GPL-3.0",
    "  (c) Meshtastic contributors",
    "  github.com/meshtastic/firmware",
    "Meshtastic device-ui, GPL-3.0",
    "  (c) Meshtastic contributors",
    "  github.com/meshtastic/device-ui",
    "Changed by JinxMesh, 2026: the screen and",
    "what the firmware does for it.",
    "",
    "ALSO INSIDE",
    "LVGL, MIT, (c) LVGL Kft",
    "LovyanGFX, FreeBSD and MIT, (c) lovyan03",
    "RadioLib, MIT, (c) Jan Gromes",
    "NimBLE and ESP-IDF, Apache-2.0",
    "Arduino core for ESP32, LGPL-2.1",
    "Crypto library, MIT, (c) Rhys Weatherley",
    "nanopb, zlib license, (c) Petteri Aimonen",
    "PNGdec, Apache-2.0, (c) BitBank Software",
    "TinyGPSPlus, LGPL-2.1, (c) Mikal Hart",
    "Montserrat font, SIL Open Font License",
    "and further libraries, each under its own",
    "license, named in the source code.",
    "",
    "Meshtastic is a registered trademark of",
    "Meshtastic LLC. JinxMesh is not affiliated",
    "with or endorsed by the Meshtastic project.",
};

constexpr uint32_t c_stockPin = 123456;   // what the firmware puts where a fixed PIN would be; unused with one-time PINs
const char c_pairingTag[] = "pairing:";   // starts the note the firmware of this fork sends while a phone pairs

struct TimeZone {
    const char *name;
    const char *tz;
};

const TimeZone c_timeZones[] = {{"UTC", "UTC0"},
                                {"US Hawaii", "HST10"},
                                {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
                                {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
                                {"US Arizona", "MST7"},
                                {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
                                {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
                                {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
                                {"Brasilia", "BRT3"},
                                {"Europe Western", "GMT0BST,M3.5.0/1,M10.5.0"},
                                {"Europe Central", "CET-1CEST,M3.5.0,M10.5.0/3"},
                                {"Europe Eastern", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
                                {"India", "IST-5:30"},
                                {"Hong Kong", "HKT-8"},
                                {"Australia Western", "AWST-8"},
                                {"Australia Central", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
                                {"Australia Eastern", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
                                {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"}};

struct Role {
    meshtastic_Config_DeviceConfig_Role role;
    const char *name;
};

// roles a handheld is sensibly put into; the deprecated ones are left out
const Role c_roles[] = {{meshtastic_Config_DeviceConfig_Role_CLIENT, "Client"},
                        {meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE, "Client mute"},
                        {meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN, "Client hidden"},
                        {meshtastic_Config_DeviceConfig_Role_CLIENT_BASE, "Client base"},
                        {meshtastic_Config_DeviceConfig_Role_TRACKER, "Tracker"},
                        {meshtastic_Config_DeviceConfig_Role_SENSOR, "Sensor"},
                        {meshtastic_Config_DeviceConfig_Role_TAK, "TAK"},
                        {meshtastic_Config_DeviceConfig_Role_TAK_TRACKER, "TAK tracker"},
                        {meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND, "Lost and found"},
                        {meshtastic_Config_DeviceConfig_Role_ROUTER, "Router"},
                        {meshtastic_Config_DeviceConfig_Role_ROUTER_LATE, "Router late"}};

struct SoundMode {
    meshtastic_Config_DeviceConfig_BuzzerMode mode;
    const char *name;
};
const SoundMode c_soundModes[] = {{meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED, "All sounds"},
                                  {meshtastic_Config_DeviceConfig_BuzzerMode_NOTIFICATIONS_ONLY, "Messages only"},
                                  {meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY, "Direct messages only"},
                                  {meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY, "System sounds only"},
                                  {meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED, "Silent"}};

// the one sound setting of the radio as the two questions it answers: which messages sound (0 all, 1 direct
// messages only, 2 none), and whether the system does (start, shutdown, GPS and key sounds)
uint8_t notifyOf(meshtastic_Config_DeviceConfig_BuzzerMode mode)
{
    switch (mode) {
    case meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY:
        return 1;
    case meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY:
    case meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED:
        return 2;
    default:
        return 0;
    }
}

bool systemOf(meshtastic_Config_DeviceConfig_BuzzerMode mode)
{
    return mode == meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED ||
           mode == meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY;
}

const char *const c_notifyNames[] = {"All messages", "Direct messages only", "Off"};

// how far a shared position can be off, by the number of bits that are sent
struct Precision {
    uint8_t bits;
    const char *metric;
    const char *imperial;
};
const Precision c_precisions[] = {{10, "23 km", "15 mi"},   {11, "12 km", "7.3 mi"},  {12, "5.8 km", "3.6 mi"}, {13, "2.9 km", "1.8 mi"},
                                  {14, "1.5 km", "0.9 mi"}, {15, "700 m", "0.5 mi"},  {16, "350 m", "0.2 mi"},  {17, "200 m", "600 ft"},
                                  {18, "90 m", "300 ft"},   {19, "50 m", "150 ft"},   {32, "Exact", "Exact"}};
constexpr uint8_t c_publicPrecision = 15; // the radio sends nothing finer on a channel anyone can read
// all but the last byte of the key every radio ships with
const uint8_t c_defaultKey[] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69};

const uint32_t c_txPowers[] = {0, 1, 2, 5, 7, 10, 12, 14, 17, 20, 22, 24, 27, 30};
const uint32_t c_intervals[] = {60, 300, 900, 1800, 3600, 7200, 21600, 43200};
const uint32_t c_bandwidths[] = {31, 62, 125, 250, 500};
const uint32_t c_bandwidths24[] = {200, 400, 800, 1600};

constexpr size_t c_settingRows = 7;       // rows of a settings page on screen
constexpr uint32_t c_slotPickerMax = 128; // more slots than this are typed as a number
constexpr uint32_t c_customPreset = 1000; // picker value for a hand-set modem

const char *soundName(meshtastic_Config_DeviceConfig_BuzzerMode mode)
{
    for (const SoundMode &m : c_soundModes) {
        if (m.mode == mode)
            return m.name;
    }
    return "All sounds";
}

const char *roleName(meshtastic_Config_DeviceConfig_Role role)
{
    for (const Role &r : c_roles) {
        if (r.role == role)
            return r.name;
    }
    return role == meshtastic_Config_DeviceConfig_Role_REPEATER ? "Repeater" : "Other";
}

meshtastic_Config_LoRaConfig_ModemPreset safePreset(meshtastic_Config_LoRaConfig_ModemPreset preset)
{
    return preset >= 0 && preset <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX
               ? preset
               : meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
}

/**
 * Bandwidth in MHz; the hand-set codes are short for the real widths.
 */
float bandwidthOf(const meshtastic_Config_LoRaConfig &lora)
{
    if (lora.use_preset)
        return LoRaPresets::getBandwidth(safePreset(lora.modem_preset));
    switch (lora.bandwidth) {
    case 0:
        return 0.25f;
    case 31:
        return 0.03125f;
    case 62:
        return 0.0625f;
    case 200:
        return 0.203125f;
    case 400:
        return 0.40625f;
    case 800:
        return 0.8125f;
    case 1600:
        return 1.625f;
    default:
        return lora.bandwidth / 1000.0f;
    }
}

std::string bandwidthName(uint32_t code)
{
    char buf[20];
    meshtastic_Config_LoRaConfig custom{};
    custom.bandwidth = (uint16_t)code;
    snprintf(buf, sizeof(buf), "%.4g kHz", bandwidthOf(custom) * 1000.0f);
    return buf;
}

std::string presetName(const meshtastic_Config_LoRaConfig &lora)
{
    return lora.use_preset ? LoRaPresets::modemPresetToString(safePreset(lora.modem_preset)) : "Custom";
}

std::string trimmed(const std::string &text)
{
    const size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

std::vector<std::string> confirmItems(const char *action)
{
    return {"Cancel", action};
}

} // namespace

// -------- settings pages --------

std::vector<KeyboardView_320x240::SettingRow> KeyboardView_320x240::settingRows(Page of) const
{
    std::vector<SettingRow> rows;
    char buf[64];
    const bool noRegion = hasLora && lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET;

    switch (of) {
    case Page::Root: {
        // Radio is first only while the region has to be set. Afterwards it sits near the end: a slip of
        // the OK key at the top of the list must not land in the settings that take a unit off its mesh.
        if (noRegion)
            rows.push_back({Setting::PageRadio, "Radio", "Set the region", 0, true});
        size_t used = 0;
        for (const meshtastic_Channel &ch : channels)
            used += ch.role != meshtastic_Channel_Role_DISABLED;
        snprintf(buf, sizeof(buf), "%u of %u", (unsigned)used, (unsigned)c_max_channels);
        rows.push_back({Setting::PageChannels, "Channels", buf});
        {
            size_t muted = 0;
            for (const ChannelInfo &info : channelInfo)
                muted += info.muted;
            std::string value = !hasDevice ? "" : silent() ? alertName() : soundName(device.buzzer_mode);
            if (muted && !silent()) {
                snprintf(buf, sizeof(buf), "%s%u muted", value.empty() ? "" : " · ", (unsigned)muted);
                value += buf;
            }
            rows.push_back({Setting::PageNotifications, "Sounds", value});
        }
        rows.push_back({Setting::PageDevice, "Device", hasOwnUser ? ownUser.long_name : ""});
        snprintf(buf, sizeof(buf), "%u%% · off after %s", (unsigned)((uiConfig.screen_brightness * 100 + 127) / 255),
                 durationName(uiConfig.screen_timeout).c_str());
        if (uiConfig.screen_timeout == 0)
            snprintf(buf, sizeof(buf), "%u%% · always on", (unsigned)((uiConfig.screen_brightness * 100 + 127) / 255));
        rows.push_back({Setting::PageDisplay, "Display", buf});
        if (!hasPosition) {
            rows.push_back({Setting::PagePosition, "Position", ""});
        } else if (position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
            rows.push_back({Setting::PagePosition, "Position",
                            position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_DISABLED ? "GPS off" : "No GPS"});
        } else {
            const int shared = positionChannel();
            rows.push_back({Setting::PagePosition, "Position",
                            shared < 0 ? std::string("GPS on · not shared") : "GPS on · # " + chatName((uint32_t)shared)});
        }
        rows.push_back({Setting::Bluetooth, "Bluetooth", !hasBluetooth ? "" : bluetooth.enabled ? "On" : "Off"});
        if (!hasLora)
            rows.push_back({Setting::PageRadio, "Radio", "Loading"});
        else if (!noRegion)
            rows.push_back({Setting::PageRadio, "Radio",
                            std::string(LoRaPresets::loRaRegionToString(lora.region)) + " · " + presetName(lora)});
        rows.push_back({Setting::PageSystem, "System", firmwareVersion});
        break;
    }
    case Page::Radio: {
        if (!hasLora)
            break;
        // Region and modem come last once a region is set: picking either can move the unit to another
        // frequency, and the first rows of a page are the ones hit by accident.
        if (noRegion)
            rows.push_back({Setting::Region, "Region", "Not set", 0, true});
        if (lora.tx_power > 0)
            snprintf(buf, sizeof(buf), "%d dBm", (int)lora.tx_power);
        else
            snprintf(buf, sizeof(buf), "Region maximum");
        rows.push_back({Setting::TxPower, "Transmit power", buf});
        snprintf(buf, sizeof(buf), "%u", (unsigned)(lora.hop_limit ? lora.hop_limit : 3));
        rows.push_back({Setting::HopLimit, "Hop limit", buf});
        rows.push_back({Setting::TxEnabled, "Transmit", lora.tx_enabled ? "On" : "Off, listening only", 0, !lora.tx_enabled});
        if (noRegion) {
            rows.push_back({Setting::Slot, "Frequency slot", "Needs a region"});
        } else {
            const uint32_t slot = activeSlot();
            if (lora.channel_num)
                snprintf(buf, sizeof(buf), "%u of %u · %.3f MHz", (unsigned)slot, (unsigned)slotCount(), slotFrequency(slot));
            else
                snprintf(buf, sizeof(buf), "By name (%u) · %.3f MHz", (unsigned)slot, slotFrequency(slot));
            rows.push_back({Setting::Slot, "Frequency slot", buf});
        }
        if (lora.override_frequency > 0.0f)
            snprintf(buf, sizeof(buf), "%.3f MHz", lora.override_frequency);
        else
            snprintf(buf, sizeof(buf), "Off");
        rows.push_back({Setting::OverrideFrequency, "Override MHz", buf});
        snprintf(buf, sizeof(buf), "%s · %.4g kHz", presetName(lora).c_str(), bandwidthOf(lora) * 1000.0f);
        rows.push_back({Setting::Preset, "Modem preset", buf});
        if (!lora.use_preset) {
            rows.push_back({Setting::Bandwidth, "Bandwidth", bandwidthName(lora.bandwidth)});
            snprintf(buf, sizeof(buf), "SF%u", (unsigned)lora.spread_factor);
            rows.push_back({Setting::SpreadFactor, "Spread factor", buf});
            snprintf(buf, sizeof(buf), "4/%u", (unsigned)lora.coding_rate);
            rows.push_back({Setting::CodingRate, "Coding rate", buf});
        }
        if (!noRegion) {
            snprintf(buf, sizeof(buf), "%s · %.4g-%.4g MHz", LoRaPresets::loRaRegionToString(lora.region),
                     LoRaPresets::getFrequencyStart(lora.region), LoRaPresets::getFrequencyEnd(lora.region));
            rows.push_back({Setting::Region, "Region", buf});
        }
        break;
    }
    case Page::Channels: {
        size_t used = 0;
        for (uint8_t i = 0; i < c_max_channels; i++) {
            const meshtastic_Channel &ch = channels[i];
            if (ch.role == meshtastic_Channel_Role_DISABLED)
                continue;
            used++;
            snprintf(buf, sizeof(buf), "%s%s%s", channelInfo[i].muted ? "Muted · " : "", channelInfo[i].precision ? "Pos · " : "",
                     keyName(ch).c_str());
            rows.push_back({Setting::Channel, (ch.role == meshtastic_Channel_Role_PRIMARY ? "# " : "+ ") + chatName(i), buf, i});
        }
        if (used < c_max_channels) {
            snprintf(buf, sizeof(buf), "%u free", (unsigned)(c_max_channels - used));
            rows.push_back({Setting::AddChannel, "Add a channel", buf});
        }
        break;
    }
    case Page::Device:
        rows.push_back({Setting::LongName, "Long name", hasOwnUser ? ownUser.long_name : ""});
        rows.push_back({Setting::ShortName, "Short name", hasOwnUser ? ownUser.short_name : ""});
        rows.push_back({Setting::Role, "Role", hasDevice ? roleName(device.role) : ""});
        {
            std::string zone = hasDevice ? device.tzdef : "";
            for (const TimeZone &tz : c_timeZones) {
                if (zone == tz.tz)
                    zone = tz.name;
            }
            rows.push_back({Setting::TimeZone, "Time zone", zone.empty() ? "UTC" : zone});
        }
        break;
    case Page::Display:
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)((uiConfig.screen_brightness * 100 + 127) / 255));
        rows.push_back({Setting::Brightness, "Brightness", buf});
        rows.push_back({Setting::ScreenTimeout, "Screen off after", durationName(uiConfig.screen_timeout)});
        rows.push_back({Setting::Units, "Units",
                        !hasDisplay                                                              ? ""
                        : display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL ? "Imperial"
                                                                                                 : "Metric"});
        rows.push_back({Setting::Clock, "Clock", !hasDisplay ? "" : display.use_12h_clock ? "12-hour" : "24-hour"});
        break;
    case Page::Position: {
        rows.push_back({Setting::Gps, "GPS",
                        !hasPosition                                                             ? ""
                        : position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED  ? "On"
                        : position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_DISABLED ? "Off"
                                                                                                 : "Not present"});
        rows.push_back(
            {Setting::PositionInterval, "Share every",
             hasPosition ? durationName(position.position_broadcast_secs ? position.position_broadcast_secs : 900) : ""});
        rows.push_back({Setting::SendPosition, "Announce me now", "Sends a ping"});
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates())
            snprintf(buf, sizeof(buf), "%.5f, %.5f", own->position.latitude * 1e-7, own->position.longitude * 1e-7);
        else
            snprintf(buf, sizeof(buf), "No fix");
        rows.push_back({Setting::OwnPosition, "My position", buf});
        // the radio broadcasts on the first channel that shares; the others only answer when asked
        const int shared = positionChannel();
        for (uint8_t i = 0; i < c_max_channels; i++) {
            if (!channelInfo[i].enabled)
                continue;
            std::string value = precisionName(channelInfo[i].precision);
            if (channelInfo[i].precision) {
                if (value.rfind("within ", 0) == 0)
                    value.erase(0, 7);
                value = (i == shared ? "Broadcast · " : "On request · ") + value;
            }
            rows.push_back({Setting::ChannelPosition, "# " + chatName(i), value, i});
        }
        break;
    }
    case Page::Notifications:
        rows.push_back({Setting::NotifySound, "Notifications", hasDevice ? c_notifyNames[notifyOf(device.buzzer_mode)] : ""});
        rows.push_back({Setting::AlertTone, "Notification tone", alertTone.empty() ? "" : [this]() -> std::string {
                            for (int i = 0; i < numRingtones; i++) {
                                if (alertTone == ringtone[i].rtttl)
                                    return ringtone[i].name;
                            }
                            // a tune of its own: its name stands before the first colon
                            return alertTone.substr(0, std::min<size_t>(alertTone.find(':'), 16));
                        }()});
        rows.push_back({Setting::ToneDirect, "Tone for a direct message", prefs.toneDirect.empty() ? "Same" : prefs.toneDirect});
        rows.push_back({Setting::ToneEmergency, "Tone for an emergency message", prefs.toneEmergency.empty() ? "Same" : prefs.toneEmergency});
        rows.push_back({Setting::ToneTimer, "Tone for the timer", prefs.toneTimer.empty() ? "Beeps" : prefs.toneTimer});
        rows.push_back({Setting::SystemSounds, "System sounds", !hasDevice ? "" : systemOf(device.buzzer_mode) ? "On" : "Off"});
        rows.push_back({Setting::ScreenSound, "Screen on and off sound", prefs.screenSound ? "On" : "Off"});
        for (uint8_t i = 0; i < c_max_channels; i++) {
            if (channelInfo[i].enabled)
                rows.push_back({Setting::ChannelMute, "# " + chatName(i),
                                channelInfo[i].muted ? "Muted" : prefs.toneChannel[i].empty() ? "Alerts" : prefs.toneChannel[i], i});
        }
        break;
    case Page::System:
        rows.push_back({Setting::Reboot, "Restart", ""});
        if (canShutdown)
            rows.push_back({Setting::Shutdown, "Shut down", ""});
        snprintf(buf, sizeof(buf), nodeStore.size() == 1 ? "%u node" : "%u nodes", (unsigned)nodeStore.size());
        rows.push_back({Setting::ResetNodes, "Reset the node list", buf});
        rows.push_back({Setting::ClearChats, "Clear all chats", ""});
        rows.push_back({Setting::FactoryReset, "Factory reset", ""});
        rows.push_back({Setting::About, "About", firmwareVersion});
        break;
    case Page::Wifi:
        break;
    }
    extraRows(of, rows);
    return rows;
}

/**
 * Settings tab on a page, optionally on one of its rows; used by the tiles and menus that lead here.
 */
void KeyboardView_320x240::openSettings(Page to, Setting focus)
{
    switchTab(Tab::Settings);
    page = to;
    settingFocus = settingTop = 0;
    // the row of the root list that leads to the page
    static const Setting pageRows[] = {Setting::None,        Setting::PageRadio,    Setting::PageChannels, Setting::PageDevice,
                                       Setting::PageDisplay, Setting::PagePosition, Setting::PageSystem,   Setting::PageNotifications,
                                       Setting::PageWifi};
    rootFocus = 0;
    const std::vector<SettingRow> rootRows = settingRows(Page::Root);
    for (size_t i = 0; i < rootRows.size(); i++) {
        if (rootRows[i].id == pageRows[(uint8_t)to])
            rootFocus = i;
    }
    const std::vector<SettingRow> rows = settingRows(to);
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].id == focus)
            settingFocus = i;
    }
    if (to == Page::Root)
        settingFocus = rootFocus;
}

void KeyboardView_320x240::handleSettingsKey(uint32_t key)
{
    const size_t count = settingRows(page).size();
    if (key == LV_KEY_UP) {
        if (settingFocus > 0)
            settingFocus--;
    } else if (key == LV_KEY_DOWN) {
        if (settingFocus + 1 < count)
            settingFocus++;
    } else if (key == LV_KEY_LEFT) {
        // a page is one step right of the list it came from
        if (page == Page::Root) {
            walkTabs(-1);
        } else {
            page = Page::Root;
            settingFocus = rootFocus;
            settingTop = 0;
        }
    } else if (key == LV_KEY_RIGHT) {
        activate();
    }
}

void KeyboardView_320x240::renderSettings(void)
{
    const std::vector<SettingRow> rows = settingRows(page);
    if (rows.empty()) {
        emptyNote(content, "Not loaded from the radio yet");
        return;
    }
    settingFocus = std::min(settingFocus, rows.size() - 1);
    settingTop = scrollTop(settingTop, settingFocus, rows.size(), c_settingRows);
    for (size_t i = settingTop; i < rows.size() && i < settingTop + c_settingRows; i++)
        settingRow(content, 2 + (int32_t)(i - settingTop) * c_settingHeight, i == settingFocus, rows[i].title.c_str(),
                   rows[i].value.c_str(), rows[i].alert);
    listScrollbar(content, 2, settingTop, rows.size(), c_settingRows, c_settingHeight);
}

/**
 * Screen shown instead of everything else while the radio restarts or serves a phone over Bluetooth.
 */
void KeyboardView_320x240::renderTakeover(void)
{
    if (splash) {
        lv_obj_t *panel = box(overlay, 0, 0, c_width, c_height, c_ground);
        lv_obj_t *logo = lv_image_create(panel);
        lv_image_set_src(logo, &kbview_logo);
        lv_obj_set_pos(logo, (c_width - (int32_t)kbview_logo.header.w) / 2, (c_height - (int32_t)kbview_logo.header.h) / 2);
        return;
    }
    if (locked && !restarting) {
        renderLock();
        return;
    }
    lv_obj_t *panel = box(overlay, 0, 0, c_width, c_height, c_ground);
    if (restarting) {
        label(panel, 0, 88, c_width, font20, c_text, "Restarting", LV_TEXT_ALIGN_CENTER);
        label(panel, 0, 122, c_width, font12, c_muted, "The radio is applying the change", LV_TEXT_ALIGN_CENTER);
        return;
    }
    // A PIN is on this screen only while a phone is pairing, and it is good for that one attempt. A PIN
    // that is stored in the radio (fixed PIN) is known to whoever set it and is not shown.
    char buf[16];
    const bool oneTime = bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN;
    label(panel, 0, 30, c_width, font16, c_text, "Bluetooth is on", LV_TEXT_ALIGN_CENTER);
    if (pairing) {
        snprintf(buf, sizeof(buf), "%.3s %.3s", pairingPin, pairingPin + 3);
        label(panel, 0, 64, c_width, font20, c_focus, buf, LV_TEXT_ALIGN_CENTER);
        label(panel, 0, 96, c_width, font12, c_muted, "A phone is pairing: enter this PIN there", LV_TEXT_ALIGN_CENTER);
    } else if (oneTime) {
        label(panel, 0, 64, c_width, font12, c_muted, "Pair in the Meshtastic app", LV_TEXT_ALIGN_CENTER);
        label(panel, 0, 82, c_width, font12, c_muted, "The PIN shows here while a phone pairs", LV_TEXT_ALIGN_CENTER);
    } else if (bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        label(panel, 0, 64, c_width, font12, c_muted, "Any phone can pair: no PIN is asked for", LV_TEXT_ALIGN_CENTER);
        label(panel, 0, 130, c_width, font12, c_focusMuted, "OK: ask for a new PIN at every pairing", LV_TEXT_ALIGN_CENTER);
    } else {
        label(panel, 0, 64, c_width, font12, c_muted, "Pairing asks for a fixed PIN", LV_TEXT_ALIGN_CENTER);
        label(panel, 0, 82, c_width, font12, c_muted, "It is stored in the radio and not shown here", LV_TEXT_ALIGN_CENTER);
        label(panel, 0, 130, c_width, font12, c_focusMuted, "OK: a new PIN at every pairing instead", LV_TEXT_ALIGN_CENTER);
    }
    label(panel, 0, 150, c_width, font12, c_focusMuted, "Hold OK: turn Bluetooth off", LV_TEXT_ALIGN_CENTER);
    label(panel, 0, 170, c_width, font12, c_focusMuted, "Back: close", LV_TEXT_ALIGN_CENTER);
}

// -------- opening a setting --------

bool KeyboardView_320x240::loaded(bool has)
{
    if (!has)
        toast("Not loaded from the radio yet");
    return has;
}

/**
 * Simulator runs can skip everything that restarts the radio (KBVIEW_SAFE=1).
 */
bool KeyboardView_320x240::blocked(const char *what)
{
    if (!safeMode)
        return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: skipped in safe mode", what);
    toast(buf);
    return true;
}

void KeyboardView_320x240::activateSetting(const SettingRow &row)
{
    char buf[64];
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    int32_t current = -1;
    auto add = [&items, &values, &current](const std::string &name, uint32_t value, bool isCurrent = false) {
        if (isCurrent)
            current = (int32_t)items.size();
        items.push_back(name);
        values.push_back(value);
    };
    auto enterPage = [this](Page to) {
        rootFocus = settingFocus;
        page = to;
        settingFocus = settingTop = 0;
    };

    if (extraSetting(row))
        return;
    switch (row.id) {
    case Setting::PageWifi:
        enterPage(Page::Wifi);
        break;
    case Setting::PageRadio:
        enterPage(Page::Radio);
        break;
    case Setting::PageChannels:
        enterPage(Page::Channels);
        break;
    case Setting::PageDevice:
        enterPage(Page::Device);
        break;
    case Setting::PageDisplay:
        enterPage(Page::Display);
        break;
    case Setting::PagePosition:
        enterPage(Page::Position);
        break;
    case Setting::PageSystem:
        enterPage(Page::System);
        break;
    case Setting::PageNotifications:
        enterPage(Page::Notifications);
        break;

    case Setting::NotifySound:
        if (!loaded(hasDevice))
            break;
        openPicker(Setting::NotifySound, "Sound for a new message", {c_notifyNames[0], c_notifyNames[1], c_notifyNames[2]}, {},
                   notifyOf(device.buzzer_mode), "Muted channels stay quiet in every mode");
        break;
    case Setting::SystemSounds:
        if (!loaded(hasDevice))
            break;
        openPicker(Setting::SystemSounds, "System sounds", {"On", "Off"}, {1, 0}, systemOf(device.buzzer_mode) ? 0 : 1,
                   "The start and shutdown tunes, GPS on and off, key sounds");
        break;
    case Setting::AlertTone: {
        for (int i = 0; i < numRingtones; i++)
            add(ringtone[i].name, (uint32_t)i, alertTone == ringtone[i].rtttl);
        pickTones = ownTones();
        for (size_t i = 0; i < pickTones.size(); i++)
            add(pickTones[i], (uint32_t)(numRingtones + i), alertTone == tuneText(pickTones[i]));
        openPicker(Setting::AlertTone, "Notification tone", std::move(items), std::move(values), current,
                   "For a new message. You hear each as you move");
        break;
    }
    case Setting::ToneDirect:
    case Setting::ToneEmergency:
    case Setting::ToneTimer: {
        const std::string &now = toneOf(row.id);
        add(row.id == Setting::ToneTimer ? "Beeps" : "Same as other messages", 0, now.empty());
        for (int i = 0; i < numRingtones; i++)
            add(ringtone[i].name, (uint32_t)i + 1, now == ringtone[i].name);
        // the user's own, from /tones on the card
        pickTones = ownTones();
        for (size_t i = 0; i < pickTones.size(); i++)
            add(pickTones[i], (uint32_t)(numRingtones + 1 + i), now == pickTones[i]);
        openPicker(row.id, row.title.c_str(), std::move(items), std::move(values), current, "You hear each as you move");
        break;
    }
    case Setting::ScreenSound:
        prefs.screenSound = !prefs.screenSound;
        savePrefs();
        screenSound(true); // what it sounds like
        break;
    case Setting::ChannelMute:
        openChannelTone((uint8_t)row.arg);
        break;
    case Setting::ChannelPosition:
        openPositionPicker((uint8_t)row.arg);
        break;

    case Setting::Region:
        if (!loaded(hasLora))
            break;
        for (int r = 1; r <= _meshtastic_Config_LoRaConfig_RegionCode_MAX; r++) {
            const auto region = (meshtastic_Config_LoRaConfig_RegionCode)r;
            // a radio that lists its regions only takes those, and the amateur bands only from a licensed operator
            const meshtastic_LoRaPresetGroup *group = presetGroup(region);
            if (regionPresets.region_groups_count && (!group || (group->licensed_only && !ownUser.is_licensed)))
                continue;
            snprintf(buf, sizeof(buf), "%s · %.4g-%.4g MHz", LoRaPresets::loRaRegionToString(region),
                     LoRaPresets::getFrequencyStart(region), LoRaPresets::getFrequencyEnd(region));
            add(buf, r, region == lora.region);
        }
        openPicker(Setting::Region, "Region", std::move(items), std::move(values), current,
                   "Pick where you are: it sets the legal band");
        break;
    case Setting::Preset: {
        if (!loaded(hasLora))
            break;
        if (lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            toast("Set the region first");
            break;
        }
        for (int p = 0; p <= _meshtastic_Config_LoRaConfig_ModemPreset_MAX; p++) {
            const auto preset = (meshtastic_Config_LoRaConfig_ModemPreset)p;
            if (!presetAllowed(lora.region, preset))
                continue;
            snprintf(buf, sizeof(buf), "%s · %s kHz", LoRaPresets::modemPresetToString(preset),
                     LoRaPresets::getBandwidthString(preset));
            add(buf, p, lora.use_preset && preset == lora.modem_preset);
        }
        add("Custom", c_customPreset, !lora.use_preset);
        openPicker(Setting::Preset, "Modem preset", std::move(items), std::move(values), current,
                   "Everyone on the mesh needs the same one");
        break;
    }
    case Setting::Bandwidth:
        if (!loaded(hasLora))
            break;
        if (lora.region == meshtastic_Config_LoRaConfig_RegionCode_LORA_24) {
            for (uint32_t code : c_bandwidths24)
                add(bandwidthName(code), code, code == lora.bandwidth);
        }
        for (uint32_t code : c_bandwidths)
            add(bandwidthName(code), code, code == lora.bandwidth);
        openPicker(Setting::Bandwidth, "Bandwidth", std::move(items), std::move(values), current);
        break;
    case Setting::SpreadFactor:
        if (!loaded(hasLora))
            break;
        for (uint32_t sf = 7; sf <= 12; sf++) {
            snprintf(buf, sizeof(buf), "SF%u", (unsigned)sf);
            add(buf, sf, sf == lora.spread_factor);
        }
        openPicker(Setting::SpreadFactor, "Spread factor", std::move(items), std::move(values), current,
                   "Higher reaches further and is slower");
        break;
    case Setting::CodingRate:
        if (!loaded(hasLora))
            break;
        for (uint32_t cr = 5; cr <= 8; cr++) {
            snprintf(buf, sizeof(buf), "4/%u", (unsigned)cr);
            add(buf, cr, cr == lora.coding_rate);
        }
        openPicker(Setting::CodingRate, "Coding rate", std::move(items), std::move(values), current);
        break;
    case Setting::Slot: {
        if (!loaded(hasLora))
            break;
        const uint32_t count = slotCount();
        if (count == 0) {
            toast("Set the region first");
            break;
        }
        if (count > c_slotPickerMax) {
            snprintf(buf, sizeof(buf), "1 to %u, 0 picks it automatically", (unsigned)count);
            openEditor(Setting::Slot, "Frequency slot", lora.channel_num ? std::to_string(lora.channel_num) : "", 4, "0", buf);
            break;
        }
        meshtastic_Config_LoRaConfig automatic = lora;
        automatic.channel_num = 0;
        std::swap(automatic, lora);
        const uint32_t autoSlot = activeSlot();
        std::swap(automatic, lora);
        // the slot of the public mesh of this preset: where a primary channel without a name of its own lands
        const std::string publicName = presetName(lora);
        uint32_t publicSlot = 5381;
        for (char c : publicName)
            publicSlot += (publicSlot << 5) + (unsigned char)c;
        publicSlot = publicSlot % count + 1;
        // First the two a user most often wants: the public slot, then the one that follows the channel name
        // (named for what it does: with a primary channel of another name it is not the public slot).
        const float start = LoRaPresets::getFrequencyStart(lora.region), bw = bandwidthOf(lora);
        snprintf(buf, sizeof(buf), "%u · %.3f MHz · %s", (unsigned)publicSlot, start + bw / 2 + (publicSlot - 1) * bw,
                 publicName.c_str());
        add(buf, publicSlot, publicSlot == lora.channel_num);
        snprintf(buf, sizeof(buf), "By channel name (%u)", (unsigned)autoSlot);
        add(buf, 0, lora.channel_num == 0);
        for (uint32_t slot = 1; slot <= count; slot++) {
            if (slot == publicSlot)
                continue;
            snprintf(buf, sizeof(buf), "%u · %.3f MHz", (unsigned)slot, start + bw / 2 + (slot - 1) * bw);
            add(buf, slot, slot == lora.channel_num);
        }
        openPicker(Setting::Slot, "Frequency slot", std::move(items), std::move(values), current,
                   lora.override_frequency > 0.0f ? "The override frequency wins over the slot"
                                                  : "By channel name: the slot follows the primary channel");
        break;
    }
    case Setting::OverrideFrequency:
        if (!loaded(hasLora))
            break;
        if (lora.override_frequency > 0.0f)
            snprintf(buf, sizeof(buf), "%.3f", lora.override_frequency);
        else
            buf[0] = '\0';
        openEditor(Setting::OverrideFrequency, "Override frequency in MHz", buf, 9, "Off", "Empty turns it off");
        break;
    case Setting::TxPower:
        if (!loaded(hasLora))
            break;
        for (uint32_t power : c_txPowers) {
            if (power)
                snprintf(buf, sizeof(buf), "%u dBm", (unsigned)power);
            else
                snprintf(buf, sizeof(buf), "Region maximum");
            add(buf, power, (int)power == lora.tx_power);
        }
        openPicker(Setting::TxPower, "Transmit power", std::move(items), std::move(values), current,
                   "The radio caps it at what the region allows");
        break;
    case Setting::HopLimit:
        if (!loaded(hasLora))
            break;
        for (uint32_t hops = 1; hops <= 7; hops++)
            add(std::to_string(hops), hops, hops == (lora.hop_limit ? lora.hop_limit : 3));
        openPicker(Setting::HopLimit, "Hop limit", std::move(items), std::move(values), current, "3 suits most meshes");
        break;
    case Setting::TxEnabled:
        if (!loaded(hasLora))
            break;
        openPicker(Setting::TxEnabled, "Transmit", {"On", "Off, listening only"}, {1, 0}, lora.tx_enabled ? 0 : 1);
        break;

    case Setting::Channel:
        openChannelMenu((uint8_t)row.arg);
        break;
    case Setting::AddChannel:
        // not before the radio has sent its channels: the first free slot would be one that is only not here yet
        if (std::none_of(channelInfo.begin(), channelInfo.end(), [](const ChannelInfo &info) { return info.primary; })) {
            toast("The radio has not sent its channels yet");
            break;
        }
        openEditor(Setting::ChannelName, "New channel", "", sizeof(channels[0].settings.name) - 1, "Name", "Next: pick its key",
                   UINT32_MAX);
        break;

    case Setting::LongName:
        if (loaded(hasOwnUser))
            openEditor(Setting::LongName, "Long name", ownUser.long_name, sizeof(ownUser.long_name) - 1, "Name");
        break;
    case Setting::ShortName:
        if (loaded(hasOwnUser))
            openEditor(Setting::ShortName, "Short name", ownUser.short_name, sizeof(ownUser.short_name) - 1, "Abcd",
                       "Up to 4 letters, shown on maps and lists");
        break;
    case Setting::Role:
        if (!loaded(hasDevice))
            break;
        for (const Role &r : c_roles)
            add(r.name, r.role, r.role == device.role);
        openPicker(Setting::Role, "Role", std::move(items), std::move(values), current, "Client suits a handheld. Restarts");
        break;
    case Setting::TimeZone:
        if (!loaded(hasDevice))
            break;
        for (uint32_t i = 0; i < sizeof(c_timeZones) / sizeof(c_timeZones[0]); i++)
            add(c_timeZones[i].name, i, strcmp(device.tzdef, c_timeZones[i].tz) == 0);
        openPicker(Setting::TimeZone, "Time zone", std::move(items), std::move(values), current);
        break;

    case Setting::Brightness: {
        const uint32_t now = (uiConfig.screen_brightness * 100 + 127) / 255;
        for (uint32_t pct = 10; pct <= 100; pct += 10) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
            add(buf, pct, pct == (now + 5) / 10 * 10);
        }
        openPicker(Setting::Brightness, "Brightness", std::move(items), std::move(values), current);
        break;
    }
    case Setting::ScreenTimeout:
        for (uint32_t secs : c_timeouts)
            add(durationName(secs), secs, secs == uiConfig.screen_timeout);
        openPicker(Setting::ScreenTimeout, "Screen off after", std::move(items), std::move(values), current,
                   "Any key turns it back on");
        break;
    case Setting::Units:
        if (loaded(hasDisplay))
            openPicker(
                Setting::Units, "Units", {"Metric", "Imperial"},
                {meshtastic_Config_DisplayConfig_DisplayUnits_METRIC, meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL},
                display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL ? 1 : 0);
        break;
    case Setting::Clock:
        if (loaded(hasDisplay))
            openPicker(Setting::Clock, "Clock", {"12-hour", "24-hour"}, {1, 0}, display.use_12h_clock ? 0 : 1);
        break;

    case Setting::Gps:
        if (!loaded(hasPosition))
            break;
        if (position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT) {
            toast("This device reports no GPS");
            break;
        }
        openPicker(Setting::Gps, "GPS",
                   confirmItems(position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED ? "Turn GPS off"
                                                                                                      : "Turn GPS on"),
                   {0, 1}, -1, "The radio restarts to apply it");
        break;
    case Setting::PositionInterval:
        if (!loaded(hasPosition))
            break;
        for (uint32_t secs : c_intervals)
            add(durationName(secs), secs, secs == (position.position_broadcast_secs ? position.position_broadcast_secs : 900));
        openPicker(Setting::PositionInterval, "Share position every", std::move(items), std::move(values), current,
                   "The radio restarts to apply it");
        break;
    case Setting::SendPosition:
        controller->sendPing();
        toast("Ping sent");
        break;
    case Setting::OwnPosition: {
        const NodeRecord *own = nodeStore.find(ownNode);
        std::vector<std::string> lines;
        if (own && own->position.hasCoordinates()) {
            snprintf(buf, sizeof(buf), "Latitude %.6f", own->position.latitude * 1e-7);
            lines.push_back(buf);
            snprintf(buf, sizeof(buf), "Longitude %.6f", own->position.longitude * 1e-7);
            lines.push_back(buf);
            if (hasDisplay && display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL)
                snprintf(buf, sizeof(buf), "Altitude %d ft", (int)lround(own->position.altitude * 3.28084));
            else
                snprintf(buf, sizeof(buf), "Altitude %d m", (int)own->position.altitude);
            lines.push_back(buf);
            if (own->position.satellites) {
                snprintf(buf, sizeof(buf), "%u satellites", (unsigned)own->position.satellites);
                lines.push_back(buf);
            }
        } else {
            lines.push_back("No fix yet.");
            lines.push_back("A first fix needs open sky and can take");
            lines.push_back("a few minutes.");
            if (hasLora && lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
                lines.push_back("The GPS stays off until a region is set.");
        }
        openInfo("My position", std::move(lines));
        break;
    }

    case Setting::Bluetooth:
        if (!loaded(hasBluetooth))
            break;
        if (bluetooth.enabled) {
            bluetoothMode = true;
            break;
        }
        openPicker(Setting::Bluetooth, "Bluetooth", confirmItems("Turn Bluetooth on"), {0, 1}, -1,
                   "For the phone app. The radio restarts");
        break;

    case Setting::Reboot:
        openPicker(Setting::Reboot, "Restart the radio?", confirmItems("Restart"), {0, 1});
        break;
    case Setting::Shutdown:
        openPicker(Setting::Shutdown, "Shut down?", confirmItems("Shut down"), {0, 1});
        break;
    case Setting::ResetNodes:
        openPicker(Setting::ResetNodes, "Forget all nodes?", confirmItems("Reset the node list"), {0, 1}, -1,
                   "Nodes come back as they are heard. Restarts");
        break;
    case Setting::ClearChats:
        openPicker(Setting::ClearChats, "Delete all messages?", confirmItems("Clear all chats"), {0, 1});
        break;
    case Setting::FactoryReset:
        openPicker(Setting::FactoryReset, "Erase all settings?", confirmItems("Factory reset"), {0, 1}, -1,
                   "Region, channels and keys are lost");
        break;
    case Setting::About: {
        std::vector<std::string> lines;
        lines.push_back("JinxMesh: Meshtastic, keyboard first");
        lines.push_back(c_homePage);
        lines.push_back("Firmware " + (firmwareVersion.empty() ? std::string("unknown") : firmwareVersion));
        lines.push_back(std::string("Built ") + keyboardViewBuildStamp());
        snprintf(buf, sizeof(buf), "Node !%08x", (unsigned)ownNode);
        lines.push_back(buf);
        if (hasOwnUser)
            lines.push_back(std::string(ownUser.long_name) + " (" + ownUser.short_name + ")");
        if (hasLora) {
            snprintf(buf, sizeof(buf), "%s · %s · slot %u", LoRaPresets::loRaRegionToString(lora.region),
                     presetName(lora).c_str(), (unsigned)activeSlot());
            lines.push_back(buf);
        }
        snprintf(buf, sizeof(buf), "%u nodes, %u chats", (unsigned)nodeStore.size(), (unsigned)conversations.size());
        lines.push_back(buf);
        lines.push_back("Down: license and credits");
        for (const char *line : c_legal)
            lines.push_back(line);
        openInfo("About", std::move(lines));
        break;
    }
    default:
        break;
    }
}

// -------- applying a choice --------

void KeyboardView_320x240::applyPick(Setting what, uint32_t value, uint32_t target)
{
    if (applyMenuPick(what, value, target))
        return;

    char buf[64];
    switch (what) {
    case Setting::Region:
        setRegion((meshtastic_Config_LoRaConfig_RegionCode)value);
        break;
    case Setting::Preset: {
        const float bandwidthBefore = bandwidthOf(lora);
        if (value == c_customPreset) {
            lora.use_preset = false;
            if (!lora.bandwidth) {
                lora.bandwidth = 250;
                lora.spread_factor = 11;
                lora.coding_rate = 5;
            }
        } else {
            lora.use_preset = true;
            lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)value;
        }
        // slots are counted in the bandwidth: a chosen slot stays only while the bandwidth does
        if (bandwidthOf(lora) != bandwidthBefore)
            lora.channel_num = 0;
        sendLora();
        snprintf(buf, sizeof(buf), "%s · slot %u · %.3f MHz", presetName(lora).c_str(), (unsigned)activeSlot(),
                 slotFrequency(activeSlot()));
        toast(buf, 3000);
        break;
    }
    case Setting::Bandwidth:
        if (lora.bandwidth != (uint16_t)value)
            lora.channel_num = 0;
        lora.bandwidth = (uint16_t)value;
        sendLora();
        break;
    case Setting::SpreadFactor:
        lora.spread_factor = value;
        sendLora();
        break;
    case Setting::CodingRate:
        lora.coding_rate = (uint8_t)value;
        sendLora();
        break;
    case Setting::Slot:
        lora.channel_num = (uint16_t)value;
        sendLora();
        snprintf(buf, sizeof(buf), "Slot %u · %.3f MHz", (unsigned)activeSlot(), slotFrequency(activeSlot()));
        toast(buf, 3000);
        break;
    case Setting::TxPower:
        lora.tx_power = (int8_t)value;
        sendLora();
        break;
    case Setting::HopLimit:
        lora.hop_limit = value;
        sendLora();
        break;
    case Setting::TxEnabled:
        lora.tx_enabled = value != 0;
        sendLora();
        toast(value ? "Transmitting again" : "Listening only");
        break;

    case Setting::ChannelMenu: {
        if (target >= c_max_channels)
            break;
        meshtastic_Channel &ch = channels[target];
        switch (value) {
        case 1:
            openChat(target);
            break;
        case 2:
            openEditor(Setting::ChannelName, "Channel name", ch.settings.name, sizeof(ch.settings.name) - 1, "Name",
                       ch.role == meshtastic_Channel_Role_PRIMARY ? "The name also picks the automatic slot"
                                                                  : "Others only hear it under the same name and key",
                       target);
            break;
        case 3:
            openPicker(Setting::ChannelKey, ("Key for # " + chatName(target)).c_str(),
                       {"Default key", "New random key", "Type a key", "No encryption"}, {1, 2, 3, 0}, -1,
                       "Everyone on the channel needs the same key", target);
            break;
        case 4:
            shareChannel((uint8_t)target);
            break;
        case 5:
            toggleMute((uint8_t)target);
            break;
        case 8:
            openChannelTone((uint8_t)target);
            break;
        case 7:
            openPositionPicker((uint8_t)target);
            break;
        case 6:
            openPicker(Setting::ChannelDelete, ("Delete # " + chatName(target) + "?").c_str(), confirmItems("Delete the channel"),
                       {0, 1}, -1, "Its messages are deleted too", target);
            break;
        default:
            break;
        }
        break;
    }
    case Setting::ChannelKey: {
        uint8_t key[32] = {1};
        if (value == 3) {
            openEditor(Setting::ChannelKeyText, "Channel key", "", 44, "Base64", "16 or 32 bytes as base64 text", target);
        } else if (value == 2) {
            randomBytes(key, sizeof(key));
            saveChannelKey(target, key, sizeof(key));
        } else {
            // 1: the well-known default key; 0: none, said in a byte as well: a channel after the first one
            // with no key at all is encrypted with the key of the first, and "no encryption" would not be so
            key[0] = (uint8_t)value;
            saveChannelKey(target, key, 1);
        }
        break;
    }
    case Setting::ChannelDelete: {
        if (value != 1 || target >= c_max_channels || channels[target].role == meshtastic_Channel_Role_PRIMARY)
            break;
        const std::string name = chatName(target);
        meshtastic_Channel &ch = channels[target];
        ch.role = meshtastic_Channel_Role_DISABLED;
        ch.has_settings = false;
        ch.settings = meshtastic_ChannelSettings{};
        sendChannel((uint8_t)target);
        controller->removeTextMessages(ownNode, UINT32_MAX, (uint8_t)target);
        conversations.erase(target);
        if (inConversation && convKey == target)
            inConversation = false;
        prefs.toneChannel[target].clear();
        // the phone apps count channels without gaps, so the ones behind move up
        for (uint32_t from = target + 1, to = target; from < c_max_channels; from++) {
            if (channels[from].role == meshtastic_Channel_Role_DISABLED)
                continue;
            channels[to] = channels[from];
            sendChannel((uint8_t)to);
            channels[from].role = meshtastic_Channel_Role_DISABLED;
            channels[from].has_settings = false;
            channels[from].settings = meshtastic_ChannelSettings{};
            sendChannel((uint8_t)from);
            // the stored history goes by channel number and would turn up in the wrong chat after a restart
            controller->removeTextMessages(ownNode, UINT32_MAX, (uint8_t)from);
            auto chat = conversations.find(from);
            if (chat != conversations.end()) {
                conversations[to] = std::move(chat->second);
                conversations.erase(from);
            }
            if (inConversation && convKey == from)
                convKey = to;
            prefs.toneChannel[to] = prefs.toneChannel[from]; // its tune goes with it
            prefs.toneChannel[from].clear();
            to++;
        }
        savePrefs();
        applyTones();
        toast(("# " + name + " deleted").c_str());
        break;
    }

    case Setting::NotifySound:
        setSounds((uint8_t)value, systemOf(device.buzzer_mode));
        break;
    case Setting::SystemSounds:
        setSounds(notifyOf(device.buzzer_mode), value != 0);
        break;
    case Setting::ToneDirect:
    case Setting::ToneEmergency:
    case Setting::ToneTimer:
        if (value <= (uint32_t)numRingtones + pickTones.size()) {
            const std::string name = toneNameAt(what, value);
            toneOf(what) = name;
            savePrefs();
            applyTones();
            playTone(name);
        }
        break;
    case Setting::ChannelTone:
        if (target >= c_max_channels || channels[target].role == meshtastic_Channel_Role_DISABLED)
            break;
        if (value == c_pickMuted) {
            if (!channelInfo[target].muted)
                toggleMute((uint8_t)target);
        } else if (value <= (uint32_t)numRingtones + pickTones.size()) {
            const std::string name = toneNameAt(what, value);
            prefs.toneChannel[target] = name;
            savePrefs();
            applyTones();
            if (channelInfo[target].muted)
                toggleMute((uint8_t)target); // a tune was picked: it is meant to be heard
            playTone(name);
        }
        break;
    case Setting::AlertTone:
        if (value < (uint32_t)numRingtones) {
            alertTone = ringtone[value].rtttl;
            controller->sendConfig(ringtone[value].rtttl, ownNode);
            toast(ringtone[value].name);
            playTone(ringtone[value].name); // what it sounds like
        } else if (value - numRingtones < pickTones.size()) {
            // one of the user's own; the radio keeps 230 characters
            const std::string &name = pickTones[value - numRingtones];
            const std::string text = tuneText(name);
            if (text.empty() || text.size() > 230) {
                toast(text.empty() ? "That file is no tune" : "Too long for the radio (230)");
            } else {
                alertTone = text;
                controller->sendConfig(text.c_str(), ownNode);
                toast(name.c_str());
                playTone(name);
            }
        }
        break;
    case Setting::ChannelPosition: {
        if (target >= c_max_channels || channels[target].role == meshtastic_Channel_Role_DISABLED)
            break;
        meshtastic_Channel &ch = channels[target];
        ch.settings.has_module_settings = true;
        ch.settings.module_settings.position_precision = value;
        sendChannel((uint8_t)target);
        const int shared = positionChannel();
        if (value == 0)
            toast(shared < 0 ? "Position is not shared anywhere now" : ("Not shared on # " + chatName(target)).c_str(), 3000);
        else if (shared != (int)target)
            toast(("Broadcasts stay on # " + chatName((uint32_t)shared)).c_str(), 3000);
        else
            toast(("# " + chatName(target) + ": " + precisionName(value)).c_str(), 3000);
        break;
    }

    case Setting::Role:
        if (blocked("Role") || (meshtastic_Config_DeviceConfig_Role)value == device.role)
            break;
        device.role = (meshtastic_Config_DeviceConfig_Role)value;
        controller->sendConfig(meshtastic_Config_DeviceConfig{device}, ownNode);
        notifyReboot(true);
        break;
    case Setting::TimeZone:
        if (value >= sizeof(c_timeZones) / sizeof(c_timeZones[0]))
            break;
        strncpy(device.tzdef, c_timeZones[value].tz, sizeof(device.tzdef) - 1);
        device.tzdef[sizeof(device.tzdef) - 1] = '\0';
        controller->sendConfig(meshtastic_Config_DeviceConfig{device}, ownNode);
        applyTimeZone();
        toast(c_timeZones[value].name);
        break;

    case Setting::Brightness:
        uiConfig.screen_brightness = (uint8_t)(value * 255 / 100);
        if (displaydriver->hasLight())
            displaydriver->setBrightness((uint8_t)uiConfig.screen_brightness);
        controller->storeUIConfig(uiConfig);
        break;
    case Setting::ScreenTimeout:
        uiConfig.screen_timeout = (uint16_t)value;
        displaydriver->setScreenTimeout((uint16_t)value);
        controller->storeUIConfig(uiConfig);
        break;
    case Setting::Units:
        display.units = (meshtastic_Config_DisplayConfig_DisplayUnits)value;
        controller->sendConfig(meshtastic_Config_DisplayConfig{display}, ownNode);
        break;
    case Setting::Clock:
        display.use_12h_clock = value == 1;
        controller->sendConfig(meshtastic_Config_DisplayConfig{display}, ownNode);
        break;

    case Setting::Gps:
        if (value == 1)
            toggleGps();
        break;
    case Setting::PositionInterval:
        if (blocked("Position interval") || value == position.position_broadcast_secs)
            break;
        position.position_broadcast_secs = value;
        controller->sendConfig(meshtastic_Config_PositionConfig{position}, ownNode);
        notifyReboot(true);
        break;

    case Setting::Bluetooth:
        if (value != 1 || blocked("Bluetooth"))
            break;
        // nothing to store: the radio makes a PIN for each pairing and tells it to this screen
        bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN;
        bluetooth.fixed_pin = c_stockPin;
        bluetooth.enabled = true;
        controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, ownNode);
        notifyReboot(true);
        break;

    case Setting::Reboot:
        if (value != 1 || blocked("Restart"))
            break;
        controller->requestReboot(5, ownNode);
        notifyReboot(true);
        break;
    case Setting::Shutdown:
        if (value != 1 || blocked("Shut down"))
            break;
        controller->requestShutdown(5, ownNode);
        notifyShutdown();
        break;
    case Setting::ResetNodes:
        if (value != 1 || blocked("Node list reset"))
            break;
        controller->requestReset(false, ownNode);
        notifyReboot(true);
        break;
    case Setting::ClearChats:
        if (value != 1)
            break;
        controller->removeTextMessages(0, 0, 0);
        conversations.clear();
        inConversation = false;
        toast("All chats cleared");
        break;
    case Setting::FactoryReset:
        if (value != 1 || blocked("Factory reset"))
            break;
        controller->requestReset(true, ownNode);
        notifyReboot(true);
        break;
    default:
        break;
    }
    dirty = urgent = true;
}

void KeyboardView_320x240::applyEdit(Setting what, const std::string &text, uint32_t target)
{
    char buf[64];
    const std::string value = trimmed(text);

    if (extraEdit(what, text, target))
        return;
    switch (what) {
    case Setting::Slot: {
        const uint32_t count = slotCount();
        char *end = nullptr;
        const unsigned long slot = strtoul(value.c_str(), &end, 10);
        if ((end && *end) || slot > count) {
            snprintf(buf, sizeof(buf), "Slots go from 1 to %u", (unsigned)count);
            toast(buf);
            break;
        }
        applyPick(Setting::Slot, (uint32_t)slot, 0);
        break;
    }
    case Setting::OverrideFrequency: {
        char *end = nullptr;
        const float mhz = value.empty() ? 0.0f : strtof(value.c_str(), &end);
        if ((end && *end) || !(mhz >= 0.0f)) { // also "nan", which is neither below nor above anything
            toast("Type a frequency in MHz, such as 906.875");
            break;
        }
        const float start = LoRaPresets::getFrequencyStart(lora.region), stop = LoRaPresets::getFrequencyEnd(lora.region);
        if (mhz > 0.0f && (lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET || mhz < start || mhz > stop)) {
            if (lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
                snprintf(buf, sizeof(buf), "Set the region first");
            else
                snprintf(buf, sizeof(buf), "%s allows %.4g to %.4g MHz", LoRaPresets::loRaRegionToString(lora.region), start,
                         stop);
            toast(buf, 3000);
            break;
        }
        lora.override_frequency = mhz;
        sendLora();
        if (mhz > 0.0f)
            snprintf(buf, sizeof(buf), "Fixed on %.3f MHz", mhz);
        else
            snprintf(buf, sizeof(buf), "Slot %u · %.3f MHz", (unsigned)activeSlot(), slotFrequency(activeSlot()));
        toast(buf, 3000);
        break;
    }

    case Setting::ChannelName:
        if (value.empty()) {
            toast("Type a name first");
            break;
        }
        if (target == UINT32_MAX) {
            // a new channel is only created once its key is chosen
            pendingName = value;
            openPicker(Setting::ChannelKey, ("Key for # " + value).c_str(),
                       {"New random key", "Default key", "Type a key", "No encryption"}, {2, 1, 3, 0}, -1,
                       "Random is private; share it by QR code", UINT32_MAX);
        } else if (target < c_max_channels && channels[target].role != meshtastic_Channel_Role_DISABLED) {
            meshtastic_Channel &ch = channels[target];
            memset(ch.settings.name, 0, sizeof(ch.settings.name));
            strncpy(ch.settings.name, value.c_str(), sizeof(ch.settings.name) - 1);
            sendChannel((uint8_t)target);
            const std::string warning = channelWarning((uint8_t)target);
            toast(warning.empty() ? ("Renamed to # " + value).c_str() : warning.c_str(), warning.empty() ? 2000 : 5000);
        }
        break;
    case Setting::ChannelKeyText: {
        std::string base64 = value;
        std::replace(base64.begin(), base64.end(), '-', '+');
        std::replace(base64.begin(), base64.end(), '_', '/');
        while (base64.size() % 4)
            base64.push_back('=');
        uint8_t key[64] = {};
        uint16_t size = 0;
        if (base64.empty() || base64.size() > 44 || !base64ToPsk(base64, key, size) || (size != 1 && size != 16 && size != 32)) {
            toast("That is not a 16 or 32 byte base64 key", 3000);
            break;
        }
        saveChannelKey(target, key, size);
        break;
    }

    case Setting::LongName:
    case Setting::ShortName: {
        if (value.empty()) {
            toast("Type a name first");
            break;
        }
        meshtastic_User user = ownUser;
        if (blocked("Rename"))
            break;
        if (what == Setting::LongName) {
            memset(user.long_name, 0, sizeof(user.long_name));
            strncpy(user.long_name, value.c_str(), sizeof(user.long_name) - 1);
        } else {
            memset(user.short_name, 0, sizeof(user.short_name));
            strncpy(user.short_name, value.c_str(), sizeof(user.short_name) - 1);
        }
        controller->sendConfig(user, ownNode);
        ownUser = user;
        const NodeRecord *own = nodeStore.find(ownNode);
        nodeStore.upsertUser(ownNode, own ? own->channel : 0, own ? own->lastHeard : now(), user, false);
        toast("Name saved · the radio restarts", 4000);
        break;
    }
    default:
        break;
    }
    dirty = urgent = true;
}

// -------- radio --------

void KeyboardView_320x240::sendLora(void)
{
    if (!hasLora)
        return;
    controller->sendConfig(meshtastic_Config_LoRaConfig{lora}, ownNode);
    // the radio corrects what it does not accept, so show what it really took
    controller->requestLoRaConfig(ownNode);
    dirty = true;
}

const meshtastic_LoRaPresetGroup *KeyboardView_320x240::presetGroup(meshtastic_Config_LoRaConfig_RegionCode region) const
{
    for (pb_size_t i = 0; i < regionPresets.region_groups_count; i++) {
        const meshtastic_LoRaRegionPresets &entry = regionPresets.region_groups[i];
        if (entry.region == region && entry.group_index < regionPresets.groups_count)
            return &regionPresets.groups[entry.group_index];
    }
    return nullptr;
}

bool KeyboardView_320x240::presetAllowed(meshtastic_Config_LoRaConfig_RegionCode region,
                                         meshtastic_Config_LoRaConfig_ModemPreset preset) const
{
    if (const meshtastic_LoRaPresetGroup *group = presetGroup(region)) {
        for (pb_size_t i = 0; i < group->presets_count; i++) {
            if (group->presets[i] == preset)
                return true;
        }
        return false;
    }
    // older firmware does not say: go by the band, a narrow one only takes presets as narrow as its default
    if (region < 0 || region > _meshtastic_Config_LoRaConfig_RegionCode_MAX)
        return false; // a region newer than the tables in here
    if (preset < 0 || preset > _meshtastic_Config_LoRaConfig_ModemPreset_MAX ||
        preset == meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW || LoRaPresets::getNumChannels(region, preset) == 0)
        return false;
    const float regionWidth = LoRaPresets::getBandwidth(LoRaPresets::getDefaultPreset(region));
    const float width = LoRaPresets::getBandwidth(preset);
    if (regionWidth < 0.25f)
        return width <= regionWidth;
    return width >= 0.0625f || preset < meshtastic_Config_LoRaConfig_ModemPreset_TINY_FAST;
}

void KeyboardView_320x240::updateRegionPresets(const meshtastic_LoRaRegionPresetMap &map)
{
    regionPresets = map;
}

/**
 * What the radio has to say about a request, most of all when it refused or corrected one.
 */
void KeyboardView_320x240::notifyClient(const meshtastic_ClientNotification &notification)
{
    // Not for reading: the firmware of this fork announces the PIN of a pairing that is under way, and
    // with no digits that the pairing is over. Kept in memory for the takeover screen and nowhere else.
    const size_t tagSize = sizeof(c_pairingTag) - 1;
    if (strncmp(notification.message, c_pairingTag, tagSize) == 0) {
        const char *digits = notification.message + tagSize;
        if (strlen(digits) == 6 && strspn(digits, "0123456789") == 6) {
            memcpy(pairingPin, digits, 7);
            pairing = true;
            pairingStart = lv_tick_get();
            bluetoothMode = true; // whatever is on the screen makes room
            closeMenu();
            if (screenOff)
                displaydriver->forceWakeup();
        } else {
            endPairing();
        }
        dirty = urgent = true;
        return;
    }
    if (notification.level < meshtastic_LogRecord_Level_WARNING || !notification.message[0])
        return;
    toast(notification.message, 5000);
}

/**
 * A new region can make the preset illegal or pointless, so the modem follows the region where it has to.
 */
void KeyboardView_320x240::setRegion(meshtastic_Config_LoRaConfig_RegionCode region)
{
    if (!loaded(hasLora))
        return;
    char buf[64];
    const bool sameRegion = lora.region == region;
    lora.region = region;
    if (lora.use_preset && !presetAllowed(region, lora.modem_preset)) {
        const meshtastic_LoRaPresetGroup *group = presetGroup(region);
        lora.modem_preset = group ? group->default_preset : LoRaPresets::getDefaultPreset(region);
    }
    // picking the region the unit already has must not move it off its frequency slot
    if (!sameRegion) {
        lora.channel_num = 0;
        lora.override_frequency = 0.0f;
    }
    lora.tx_enabled = true;
    sendLora();
    snprintf(buf, sizeof(buf), "%s · %s · %.3f MHz", LoRaPresets::loRaRegionToString(region), presetName(lora).c_str(),
             slotFrequency(activeSlot()));
    toast(buf, 3000);
}

uint32_t KeyboardView_320x240::slotCount(void) const
{
    if (lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        return 0;
    const float range = LoRaPresets::getFrequencyEnd(lora.region) - LoRaPresets::getFrequencyStart(lora.region);
    return (uint32_t)std::max(0.0f, floorf(range / bandwidthOf(lora)));
}

/**
 * Slot in use: the configured one, else the one the firmware derives from the primary channel name.
 */
uint32_t KeyboardView_320x240::activeSlot(void) const
{
    if (lora.channel_num)
        return lora.channel_num;
    const uint32_t count = slotCount();
    if (count == 0)
        return lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ? 0 : 1;
    const char *name = channels[0].settings.name;
    if (!name[0])
        name = lora.use_preset ? LoRaPresets::modemPresetToString(safePreset(lora.modem_preset)) : "Custom";
    uint32_t hash = 5381;
    for (const char *c = name; *c; c++)
        hash += (hash << 5) + (unsigned char)*c;
    return hash % count + 1;
}

float KeyboardView_320x240::slotFrequency(uint32_t slot) const
{
    if (lora.override_frequency > 0.0f)
        return lora.override_frequency;
    if (slot == 0)
        return 0.0f;
    const float bw = bandwidthOf(lora);
    return LoRaPresets::getFrequencyStart(lora.region) + bw / 2 + (slot - 1) * bw + lora.frequency_offset;
}

void KeyboardView_320x240::toggleGps(void)
{
    if (!loaded(hasPosition) || blocked("GPS"))
        return;
    position.gps_mode = position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED
                            ? meshtastic_Config_PositionConfig_GpsMode_DISABLED
                            : meshtastic_Config_PositionConfig_GpsMode_ENABLED;
    controller->sendConfig(meshtastic_Config_PositionConfig{position}, ownNode);
    notifyReboot(true);
}

/**
 * The firmware only reads the time zone at boot; the clock on screen follows at once.
 */
void KeyboardView_320x240::applyTimeZone(void)
{
    if (!device.tzdef[0])
        return;
    setenv("TZ", device.tzdef, 1);
    tzset();
    shownMinute = 0;
    dirty = true;
}

void KeyboardView_320x240::leaveBluetoothMode(void)
{
    bluetoothMode = false;
    endPairing();
    if (!hasBluetooth || blocked("Bluetooth"))
        return;
    bluetooth.enabled = false;
    controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, ownNode);
    notifyReboot(true);
}

/**
 * From a PIN that is stored in the radio, or from none, to a PIN the radio makes for each pairing.
 */
void KeyboardView_320x240::useOneTimePins(void)
{
    if (!hasBluetooth || bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN || blocked("Bluetooth"))
        return;
    bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN;
    bluetooth.fixed_pin = c_stockPin; // the old one goes from the radio's settings
    controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, ownNode);
    notifyReboot(true);
}

void KeyboardView_320x240::endPairing(void)
{
    if (pairing)
        dirty = urgent = true;
    pairing = false;
    memset(pairingPin, 0, sizeof(pairingPin));
}

// -------- channels --------

void KeyboardView_320x240::storeChannel(const meshtastic_Channel &ch)
{
    if (ch.index < 0 || ch.index >= c_max_channels)
        return;
    // who was heard on a channel belongs to its name and key: in another slot they move along, changed they are forgotten
    auto same = [](const meshtastic_Channel &a, const meshtastic_Channel &b) {
        return !strncmp(a.settings.name, b.settings.name, sizeof(a.settings.name)) && a.settings.psk.size == b.settings.psk.size &&
               !memcmp(a.settings.psk.bytes, b.settings.psk.bytes, a.settings.psk.size);
    };
    const bool enabled = ch.role != meshtastic_Channel_Role_DISABLED;
    bool moved = false;
    if (channelsStored[ch.index].role != meshtastic_Channel_Role_DISABLED && (!enabled || !same(channelsStored[ch.index], ch))) {
        int from = -1;
        for (int i = 0; enabled && i < c_max_channels; i++) {
            if (i != ch.index && channelsStored[i].role != meshtastic_Channel_Role_DISABLED && same(channelsStored[i], ch))
                from = i;
        }
        const uint8_t bit = (uint8_t)(1u << ch.index);
        for (auto &[nodeNum, mask] : heardOn)
            mask = (uint8_t)((mask & ~bit) | (from >= 0 && (mask & (1u << from)) ? bit : 0));
        moved = true;
    }
    const meshtastic_ChannelSettings before = channelsStored[ch.index].settings;
    const bool sameKey =
        before.psk.size == ch.settings.psk.size && !memcmp(before.psk.bytes, ch.settings.psk.bytes, before.psk.size);
    channelsStored[ch.index] = ch;
    channels[ch.index] = ch;
    ChannelInfo &info = channelInfo[ch.index];
    const bool wasEnabled = info.enabled;
    const std::string oldName = info.name;
    info.enabled = ch.role != meshtastic_Channel_Role_DISABLED;
    info.primary = ch.role == meshtastic_Channel_Role_PRIMARY;
    info.name.assign(ch.settings.name, strnlen(ch.settings.name, sizeof(ch.settings.name)));
    info.muted = info.enabled && ch.settings.has_module_settings && ch.settings.module_settings.is_muted;
    info.precision =
        info.enabled && ch.settings.has_module_settings ? (uint8_t)std::min<uint32_t>(ch.settings.module_settings.position_precision, 32) : 0;
    // a pinned channel is kept by name: the pin follows a new name of the same channel and goes when the channel does
    if (wasEnabled && (!info.enabled || info.name != oldName || !sameKey))
        channelChanged(ch.index, oldName, info.enabled && sameKey);
    // the map view is kept by channel name
    resolveMapView();
    if (moved && mapView == ch.index)
        refreshMarkers();
    dirty = true;
}

void KeyboardView_320x240::sendChannel(uint8_t index)
{
    if (index >= c_max_channels)
        return;
    channels[index].index = index;
    controller->sendConfig(channels[index], ownNode);
    storeChannel(channels[index]);
}

void KeyboardView_320x240::openChannelMenu(uint8_t index)
{
    if (index >= c_max_channels || channels[index].role == meshtastic_Channel_Role_DISABLED)
        return;
    std::vector<std::string> items = {"Open chat",
                                      "Rename",
                                      "Change key",
                                      "Share by QR code",
                                      channelInfo[index].muted ? "Unmute" : "Mute",
                                      "Tone: " + (prefs.toneChannel[index].empty() ? std::string("same as others") : prefs.toneChannel[index]),
                                      "Position: " + precisionName(channelInfo[index].precision)};
    std::vector<uint32_t> values = {1, 2, 3, 4, 5, 8, 7};
    if (channels[index].role != meshtastic_Channel_Role_PRIMARY) {
        items.push_back("Delete");
        values.push_back(6);
    }
    openPicker(Setting::ChannelMenu, ("# " + chatName(index)).c_str(), std::move(items), std::move(values), -1,
               channelWarning(index).c_str(), index);
}

/**
 * What is likely a mistake in a channel, in a line; empty: nothing. A channel is its name and its key
 * together: one with the key everyone has only is the public channel under the name of the preset.
 */
std::string KeyboardView_320x240::channelWarning(uint8_t index) const
{
    if (index >= c_max_channels || channels[index].role == meshtastic_Channel_Role_DISABLED)
        return "";
    const meshtastic_Channel &ch = channels[index];
    const pb_size_t size = ch.settings.psk.size;
    if ((size == 0 && ch.role == meshtastic_Channel_Role_PRIMARY) || (size == 1 && ch.settings.psk.bytes[0] == 0))
        return "No key: anyone can read this channel";
    if (size == 0) // not the first channel
        return "No key of its own: uses the key of the first channel";
    const bool defaultKey = (size == 1 && ch.settings.psk.bytes[0] == 1) ||
                            (size == sizeof(c_defaultKey) + 1 && memcmp(ch.settings.psk.bytes, c_defaultKey, sizeof(c_defaultKey)) == 0);
    if (defaultKey && hasLora && lora.use_preset && ch.settings.name[0] && presetName(lora) != ch.settings.name)
        return "Default key, but not named " + presetName(lora) + ": not the public channel";
    return "";
}

void KeyboardView_320x240::openChannelTone(uint8_t index)
{
    if (index >= c_max_channels || channels[index].role == meshtastic_Channel_Role_DISABLED)
        return;
    const bool muted = channelInfo[index].muted;
    const std::string &now = prefs.toneChannel[index];
    std::vector<std::string> items = {"Same as other messages", "Muted"};
    std::vector<uint32_t> values = {0, c_pickMuted};
    int current = muted ? 1 : now.empty() ? 0 : -1;
    auto add = [&](const std::string &name, uint32_t value) {
        if (!muted && now == name)
            current = (int)items.size();
        items.push_back(name);
        values.push_back(value);
    };
    for (int i = 0; i < numRingtones; i++)
        add(ringtone[i].name, (uint32_t)i + 1);
    pickTones = ownTones();
    for (size_t i = 0; i < pickTones.size(); i++)
        add(pickTones[i], (uint32_t)(numRingtones + 1 + i));
    openPicker(Setting::ChannelTone, ("Sound for # " + chatName(index)).c_str(), std::move(items), std::move(values), current,
               "You hear each as you move", index);
}

void KeyboardView_320x240::toggleMute(uint8_t index)
{
    if (index >= c_max_channels || channels[index].role == meshtastic_Channel_Role_DISABLED)
        return;
    meshtastic_Channel &ch = channels[index];
    ch.settings.has_module_settings = true;
    ch.settings.module_settings.is_muted = !ch.settings.module_settings.is_muted;
    sendChannel(index);
    toast(("# " + chatName(index) + (ch.settings.module_settings.is_muted ? " muted" : " unmuted")).c_str());
}

/**
 * Which messages sound and whether the system does, as the one setting the radio has for both. It has none for
 * "direct messages only, and system sounds": there the choice made last wins and the other one gives way.
 */
void KeyboardView_320x240::setSounds(uint8_t notify, bool system)
{
    if (!hasDevice)
        return;
    const uint8_t hadNotify = notifyOf(device.buzzer_mode);
    const bool hadSystem = systemOf(device.buzzer_mode);
    const char *note = nullptr;
    if (notify == 1 && system) {
        if (hadNotify != 1) {
            system = false;
            note = "With direct messages only the radio plays no system sounds";
        } else {
            notify = 0;
            note = "With system sounds the radio sounds for all messages";
        }
    }
    const auto mode = notify == 1   ? meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY
                      : notify == 0 ? (system ? meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED
                                              : meshtastic_Config_DeviceConfig_BuzzerMode_NOTIFICATIONS_ONLY)
                                    : (system ? meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY
                                              : meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED);
    (void)hadSystem;
    if (mode != device.buzzer_mode) {
        // the radio takes this one without a restart
        device.buzzer_mode = mode;
        controller->sendConfig(meshtastic_Config_DeviceConfig{device}, ownNode);
    }
    toast(note ? note : soundName(device.buzzer_mode), note ? 4000 : 2000);
    dirty = true;
}

void KeyboardView_320x240::updateRingtone(const char rtttl[231])
{
    alertTone.assign(rtttl, strnlen(rtttl, 230));
    dirty = true;
}

/**
 * No sound for any message, whatever the channels say.
 */
bool KeyboardView_320x240::silent(void) const
{
    return hasDevice && (device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED ||
                         device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY);
}

/**
 * A key that anyone has: none, one of the one-byte defaults, or the default key written out.
 * A secondary channel without a key of its own uses the primary one.
 */
bool KeyboardView_320x240::publicKey(uint8_t index) const
{
    if (index >= c_max_channels)
        return true;
    const meshtastic_Channel &ch = channels[index];
    if (ch.settings.psk.size == 0 && ch.role == meshtastic_Channel_Role_SECONDARY) {
        for (uint8_t i = 0; i < c_max_channels; i++) {
            if (i != index && channels[i].role == meshtastic_Channel_Role_PRIMARY)
                return publicKey(i);
        }
    }
    if (ch.settings.psk.size <= 1)
        return true;
    return ch.settings.psk.size == sizeof(c_defaultKey) + 1 && memcmp(ch.settings.psk.bytes, c_defaultKey, sizeof(c_defaultKey)) == 0;
}

/**
 * Channel the radio broadcasts the position on: the first one that shares it, -1 for none.
 */
int KeyboardView_320x240::positionChannel(void) const
{
    for (uint8_t i = 0; i < c_max_channels; i++) {
        if (channelInfo[i].enabled && channelInfo[i].precision)
            return i;
    }
    return -1;
}

std::string KeyboardView_320x240::precisionName(uint32_t bits) const
{
    if (bits == 0)
        return "Off";
    const bool imperial = hasDisplay && display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
    for (const Precision &p : c_precisions) {
        if (p.bits >= bits)
            return p.bits == 32 ? p.metric : std::string("within ") + (imperial ? p.imperial : p.metric);
    }
    return "Exact";
}

/**
 * How exactly the own position goes out on a channel, or not at all.
 */
void KeyboardView_320x240::openPositionPicker(uint8_t index)
{
    if (index >= c_max_channels || channels[index].role == meshtastic_Channel_Role_DISABLED)
        return;
    const bool open = publicKey(index);
    const uint32_t now = channelInfo[index].precision;
    std::vector<std::string> items = {"Off"};
    std::vector<uint32_t> values = {0};
    int32_t current = now == 0 ? 0 : -1;
    for (const Precision &p : c_precisions) {
        if (open && p.bits > c_publicPrecision)
            break;
        // a value from another app that is not in the list marks the next coarser step
        if (current < 0 && p.bits >= now)
            current = (int32_t)items.size();
        items.push_back(precisionName(p.bits));
        values.push_back(p.bits);
    }
    if (current < 0)
        current = (int32_t)items.size() - 1;
    const int shared = positionChannel();
    std::string note = open ? "Anyone can read this channel: no finer step" : "";
    if (shared >= 0 && shared < index)
        note = "Broadcasts go to # " + chatName((uint32_t)shared) + ", this one answers requests";
    openPicker(Setting::ChannelPosition, ("My position on # " + chatName(index)).c_str(), std::move(items), std::move(values),
               current, note.c_str(), index);
}

/**
 * Creates the channel named last (target UINT32_MAX) or replaces the key of an existing one.
 */
void KeyboardView_320x240::saveChannelKey(uint32_t target, const uint8_t *key, size_t size)
{
    const bool isNew = target == UINT32_MAX;
    if (isNew && std::none_of(channelInfo.begin(), channelInfo.end(), [](const ChannelInfo &info) { return info.primary; })) {
        toast("The radio has not sent its channels yet");
        return;
    }
    // the channel a key was being picked for may have gone meanwhile (a phone, the radio)
    if (!isNew && (target >= c_max_channels || channels[target].role == meshtastic_Channel_Role_DISABLED)) {
        toast("That channel is gone");
        return;
    }
    if (isNew) {
        target = c_max_channels;
        for (uint8_t i = 0; i < c_max_channels; i++) {
            if (channels[i].role == meshtastic_Channel_Role_DISABLED) {
                target = i;
                break;
            }
        }
        if (target == c_max_channels) {
            toast("All 8 channels are in use");
            return;
        }
        meshtastic_Channel &ch = channels[target];
        ch = meshtastic_Channel{};
        ch.index = (int8_t)target;
        ch.has_settings = true;
        ch.role = target == 0 ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
        strncpy(ch.settings.name, pendingName.c_str(), sizeof(ch.settings.name) - 1);
        pendingName.clear();
    }
    if (target >= c_max_channels || size > sizeof(channels[0].settings.psk.bytes))
        return;

    meshtastic_Channel &ch = channels[target];
    memset(ch.settings.psk.bytes, 0, sizeof(ch.settings.psk.bytes));
    memcpy(ch.settings.psk.bytes, key, size);
    ch.settings.psk.size = (pb_size_t)size;
    ch.has_settings = true;
    sendChannel((uint8_t)target);

    if (isNew) {
        // land on the new row so that sharing it is one press away
        if (tab == Tab::Settings && page == Page::Channels) {
            const std::vector<SettingRow> rows = settingRows(Page::Channels);
            for (size_t i = 0; i < rows.size(); i++) {
                if (rows[i].id == Setting::Channel && rows[i].arg == target)
                    settingFocus = i;
            }
        }
        toast(("# " + chatName(target) + " added · " + keyName(ch)).c_str(), 3000);
    } else {
        toast(("Key changed · " + keyName(ch)).c_str());
    }
}

/**
 * Channel as the QR code and link the phone apps import.
 */
void KeyboardView_320x240::shareChannel(uint8_t index)
{
    if (index >= c_max_channels || channels[index].role == meshtastic_Channel_Role_DISABLED)
        return;
    const meshtastic_Channel &ch = channels[index];
    const bool primary = ch.role == meshtastic_Channel_Role_PRIMARY;

    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    set.settings_count = 1;
    set.settings[0] = ch.settings;
    if (primary && hasLora) {
        // a primary channel replaces the phone's radio setup, so it carries ours
        set.has_lora_config = true;
        set.lora_config = lora;
    }
    uint8_t encoded[meshtastic_ChannelSet_size];
    const size_t size = pb_encode_to_bytes(encoded, sizeof(encoded), &meshtastic_ChannelSet_msg, &set);
    if (size == 0) {
        toast("Could not encode the channel");
        return;
    }
    std::string base64 = pskToBase64(encoded, (uint32_t)size);
    std::replace(base64.begin(), base64.end(), '+', '-');
    std::replace(base64.begin(), base64.end(), '/', '_');
    while (!base64.empty() && base64.back() == '=')
        base64.pop_back();
    const std::string url = std::string("https://meshtastic.org/e/") + (primary ? "#" : "?add=true#") + base64;

    std::vector<std::string> lines = {"# " + chatName(index), keyName(ch), "", "Scan it with the", "Meshtastic app."};
    if (primary) {
        lines.push_back("");
        lines.push_back("Replaces the");
        lines.push_back("phone's channels");
        lines.push_back("and radio setup.");
    } else {
        lines.push_back("");
        lines.push_back("Adds the channel.");
    }
    openInfo("Share channel", std::move(lines), url.c_str());
}

std::string KeyboardView_320x240::keyName(const meshtastic_Channel &ch) const
{
    switch (ch.settings.psk.size) {
    case 0:
        // the firmware encrypts a secondary channel without a key with the primary's
        return ch.role == meshtastic_Channel_Role_SECONDARY ? "Primary's key" : "No key";
    case 1:
        return ch.settings.psk.bytes[0] == 1 ? "Default key" : ch.settings.psk.bytes[0] == 0 ? "No key" : "Simple key";
    case 16:
        return "AES-128";
    case 32:
        return "AES-256";
    default:
        return "Unknown key";
    }
}

// -------- config from the radio --------

void KeyboardView_320x240::setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                                             bool can_shutdown)
{
    firmwareVersion = version ? version : "";
    canShutdown = can_shutdown;
    dirty = true;
}

void KeyboardView_320x240::updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg)
{
    device = cfg;
    hasDevice = true;
    applyTimeZone();
    dirty = true;
}

void KeyboardView_320x240::updatePositionConfig(const meshtastic_Config_PositionConfig &cfg)
{
    position = cfg;
    hasPosition = true;
    dirty = true;
}

void KeyboardView_320x240::updateDisplayConfig(const meshtastic_Config_DisplayConfig &cfg)
{
    display = cfg;
    hasDisplay = true;
    dirty = true;
}

/**
 * The firmware of this fork feeds this screen next to a phone (its PacketAPI has packets of its own), so
 * Bluetooth being on takes nothing away. The Bluetooth screen comes up for a pairing or from Settings.
 */
void KeyboardView_320x240::updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id)
{
    if (ownNode == 0 && id)
        ownNode = id;
    if (!cfg.enabled)
        bluetoothMode = false;
    if (!cfg.enabled)
        endPairing();
    bluetooth = cfg;
    hasBluetooth = true;
    dirty = true;
}

/**
 * With no region the radio neither transmits nor receives and the GPS stays off, so ask for it once.
 */
void KeyboardView_320x240::configCompleted(void)
{
    MeshtasticView::configCompleted();
    positionsLive = true;
    restarting = false;
    if (!regionPrompted && hasLora && lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET && !demoData &&
        menu == Menu::None && !bluetoothMode) {
        regionPrompted = true;
        activateSetting(SettingRow{Setting::Region, "", ""});
    }
    dirty = true;
}

void KeyboardView_320x240::notifyReboot(bool show)
{
    restarting = show;
    restartStart = lv_tick_get();
    closeMenu();
    dirty = urgent = true;
}

void KeyboardView_320x240::notifyShutdown(void)
{
    toast("Shutting down", 5000);
}

#endif
