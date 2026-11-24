#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "comms/UploadServer.h"
#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <ESPmDNS.h>
#include <WiFi.h>
#endif

using namespace kbview;

namespace
{

constexpr uint32_t c_wifiInterval = 1000;  // ms between two looks at the connection
constexpr uint32_t c_uploadBudget = 30;    // ms of a pass the upload page may use
constexpr uint32_t c_scanTimeout = 15000;  // ms
constexpr uint32_t c_wifiParkTime = 60000; // ms of dark screen after which an unused Wi-Fi link is dropped
constexpr size_t c_maxEntries = 200;       // of a folder in the file browser
constexpr uint32_t c_typeName = UINT32_MAX; // "type a name" in the list of networks
constexpr uint32_t c_keepIfEmpty = 1;       // target of the password editor: an empty line keeps the stored password
const char *const c_hostName = "m9";

// one view, so one of each
UploadServer *server = nullptr;
std::vector<std::string> foundNetworks;
#if defined(ARDUINO_ARCH_ESP32)
std::string activeSsid, activePassword; // what the connection was started with
bool scanning = false;
uint32_t scanStarted = 0;
bool roaming = false;         // the search is the view's own, for a saved network in reach
uint32_t tryStarted = 0;      // tick the connection attempt began at; 0: none
uint32_t roamWait = 15000;    // ms without a connection before the next search; doubles up to two minutes
constexpr uint32_t c_roamFirst = 15000, c_roamMost = 120000;
#endif

std::string sizeText(uint64_t bytes)
{
    char buf[24];
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%u kB", (unsigned)(bytes / 1024));
    else
        snprintf(buf, sizeof(buf), "%u bytes", (unsigned)bytes);
    return buf;
}

// last part of a path
std::string baseName(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string joined(const std::string &folder, const std::string &name)
{
    return folder == "/" ? "/" + name : folder + "/" + name;
}

// end of a text that is too long for its place
std::string tail(const std::string &text, size_t length)
{
    return text.size() <= length ? text : "..." + text.substr(text.size() - length + 3);
}

} // namespace

bool KeyboardView_320x240::ensureStorage(void)
{
    if (!storage)
        storage = createStorage().release();
    if (storage && !mounted)
        mounted = storage->mount() == IStorage::Status::Ok;
    return mounted;
}

// -------- Wi-Fi --------

void KeyboardView_320x240::updateNetworkConfig(const meshtastic_Config_NetworkConfig &cfg)
{
    network = cfg;
    // start from what the radio knows
    if (prefs.ssid.empty() && cfg.wifi_ssid[0]) {
        prefs.ssid = cfg.wifi_ssid;
        prefs.password = cfg.wifi_psk;
    }
    applyWifi();
}

/**
 * A password is typed and not read back: the editor starts empty and shows stars. Where one is
 * stored, an empty line keeps it.
 */
void KeyboardView_320x240::openWifiPassword(bool keepIfEmpty)
{
    openEditor(Setting::WifiPassword, ("Password of " + prefs.ssid).c_str(), "", 64, keepIfEmpty ? "New password" : "Password",
               keepIfEmpty ? "Empty keeps the stored one" : "Leave empty for an open network", keepIfEmpty ? c_keepIfEmpty : 0);
    popup.secret = true;
}

/**
 * Brings the connection in line with the preferences. A radio that runs Wi-Fi itself is left
 * alone and only used.
 */
void KeyboardView_320x240::applyWifi(void)
{
#if defined(ARDUINO_ARCH_ESP32)
    const bool want = prefs.wifi && !prefs.ssid.empty() && !network.wifi_enabled && !wifiParked;
    if (wifiStarted && (!want || prefs.ssid != activeSsid || prefs.password != activePassword)) {
        if (uploading)
            toggleUpload();
        MDNS.end();
        WiFi.disconnect(true);
        wifiStarted = false;
        ILOG_INFO("Wi-Fi off");
    }
    if (want && !wifiStarted) {
        WiFi.persistent(false); // or the driver writes the network and its password to its own storage, in clear
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(c_hostName);
        WiFi.setAutoReconnect(true);
        WiFi.begin(prefs.ssid.c_str(), prefs.password.empty() ? nullptr : prefs.password.c_str());
        activeSsid = prefs.ssid;
        activePassword = prefs.password;
        wifiStarted = true;
        tryStarted = lv_tick_get() | 1;
        ILOG_INFO("Wi-Fi connecting to %s", prefs.ssid.c_str());
    }
#else
    if (!prefs.wifi && uploading)
        toggleUpload();
#endif
    wifiChecked = 0;
    dirty = true;
}

bool KeyboardView_320x240::wifiConnected(void) const
{
#if defined(ARDUINO_ARCH_ESP32)
    return (wifiStarted || network.wifi_enabled) && WiFi.status() == WL_CONNECTED;
#elif defined(ARCH_PORTDUINO)
    return prefs.wifi; // the network of the host
#else
    return false;
#endif
}

std::string KeyboardView_320x240::wifiAddress(void) const
{
#if defined(ARDUINO_ARCH_ESP32)
    return WiFi.localIP().toString().c_str();
#else
    return "127.0.0.1";
#endif
}

/**
 * Looks after the connection once a second and serves the upload page in between.
 */
void KeyboardView_320x240::wifiTask(void)
{
    if (server) {
        server->poll(c_uploadBudget);
        // new town data is arriving: the old file must not be read on through a handle that is no longer its own
        if (server->busy() && server->file() == "/geo/towns.bin" && towns.state() != TownIndex::State::Closed) {
            towns.close();
            ownTownKnown = false;
            townCache.clear();
        }
    }
    if (wifiChecked && lv_tick_elaps(wifiChecked) < c_wifiInterval)
        return;
    wifiChecked = lv_tick_get();

#if defined(ARDUINO_ARCH_ESP32)
    // A Wi-Fi link costs more than everything else together and keeps the processor from sleeping. Behind a dark
    // screen only the upload page uses it; without that the link goes and comes back with the screen.
    if (wifiStarted && screenOff && !uploading && !scanning && lv_tick_elaps(screenOffAt) >= c_wifiParkTime) {
        wifiParked = true;
        applyWifi();
    }
#endif

    const bool up = wifiConnected();
    if (up != wifiUp) {
        wifiUp = up;
        ILOG_INFO("Wi-Fi %s", up ? "connected" : "lost");
#if defined(ARDUINO_ARCH_ESP32)
        if (up && wifiStarted) {
            if (MDNS.begin(c_hostName))
                MDNS.addService("http", "tcp", UploadServer::c_port);
        }
#endif
        applyTileDownload();
        dirty = true;
    }

#if defined(ARDUINO_ARCH_ESP32)
    if (up) {
        tryStarted = 0;
        roamWait = c_roamFirst;
    }
    // no connection for a while and more than one network saved: look which of them is in reach
    if (wifiStarted && !up && !scanning && !uploading && wifiKnown.size() > 1 && tryStarted && menu == Menu::None &&
        lv_tick_elaps(tryStarted) >= roamWait) {
        WiFi.disconnect(false); // a search does not run next to a connection attempt
        WiFi.scanNetworks(true);
        scanning = roaming = true;
        scanStarted = lv_tick_get();
        roamWait = std::min(roamWait * 2, c_roamMost);
    }
    if (scanning) {
        const int16_t found = WiFi.scanComplete();
        if (found != WIFI_SCAN_RUNNING || lv_tick_elaps(scanStarted) > c_scanTimeout) {
            scanning = false;
            foundNetworks.clear();
            std::vector<std::string> items;
            std::vector<uint32_t> values;
            char buf[16];
            const bool own = roaming;
            roaming = false;
            int best = -1000;
            size_t bestKnown = wifiKnown.size();
            for (int16_t i = 0; i < found && foundNetworks.size() < 20; i++) {
                const std::string name = WiFi.SSID(i).c_str();
                if (name.empty() || std::find(foundNetworks.begin(), foundNetworks.end(), name) != foundNetworks.end())
                    continue;
                const auto known = std::find_if(wifiKnown.begin(), wifiKnown.end(),
                                                [&name](const KnownNetwork &k) { return k.ssid == name; });
                if (known != wifiKnown.end() && !known->lost && (int)WiFi.RSSI(i) > best) {
                    best = (int)WiFi.RSSI(i);
                    bestKnown = (size_t)(known - wifiKnown.begin());
                }
                snprintf(buf, sizeof(buf), "  %d dBm", (int)WiFi.RSSI(i));
                values.push_back(foundNetworks.size());
                foundNetworks.push_back(name);
                items.push_back(name + buf + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "  open" : "") +
                                (known != wifiKnown.end() ? "  saved" : ""));
            }
            WiFi.scanDelete();
            if (own && bestKnown < wifiKnown.size() && wifiKnown[bestKnown].ssid != prefs.ssid) {
                // the strongest saved network in reach becomes the one in use
                ILOG_INFO("Wi-Fi moves to %s", wifiKnown[bestKnown].ssid.c_str());
                prefs.ssid = wifiKnown[bestKnown].ssid;
                prefs.password = wifiKnown[bestKnown].password;
                passwordLost = false;
                roamWait = c_roamFirst;
                savePrefs();
            }
            // the search took the connection attempt down, or switched the radio on
            if (wifiStarted) {
                activeSsid.clear();
                applyWifi();
            } else if (!network.wifi_enabled) {
                WiFi.mode(WIFI_OFF);
            }
            if (!own && menu == Menu::None && tab == Tab::Settings) {
                items.push_back("Type a name");
                values.push_back(c_typeName);
                openPicker(Setting::WifiNetwork, "Network", std::move(items), std::move(values), -1,
                           foundNetworks.empty() ? "No network found" : "");
            }
        }
    }
#endif

    if (uploading && menu == Menu::Info && popup.what == Setting::Upload)
        showUpload();
}

/**
 * Lists the networks in reach; one of them or a typed name becomes the network of the view.
 */
void KeyboardView_320x240::openNetworks(void)
{
    if (network.wifi_enabled) {
        toast("Wi-Fi is run by the radio");
        return;
    }
#if defined(ARDUINO_ARCH_ESP32)
    if (scanning && roaming)
        roaming = false; // the search that is running now ends in the list
    if (scanning) {
        toast("Looking for networks", 4000);
        return;
    }
    if (!wifiStarted) {
        WiFi.persistent(false); // as in applyWifi(): whichever comes first starts the driver
        WiFi.mode(WIFI_STA);
    } else if (!wifiUp)
        WiFi.disconnect(false); // a search does not run next to a connection attempt
    WiFi.scanNetworks(true);
    scanning = true;
    scanStarted = lv_tick_get();
    toast("Looking for networks", 4000);
#else
    foundNetworks = {"Home network", "Workshop", "Guest"};
    openPicker(Setting::WifiNetwork, "Network", {"Home network  -52 dBm", "Workshop  -71 dBm", "Guest  -80 dBm  open", "Type a name"},
               {0, 1, 2, c_typeName});
#endif
}

/**
 * The network in use goes into the list of saved ones, or its password there is brought up to date. A full list
 * loses its oldest entry.
 */
void KeyboardView_320x240::rememberNetwork(void)
{
    if (prefs.ssid.empty())
        return;
    for (KnownNetwork &known : wifiKnown) {
        if (known.ssid == prefs.ssid) {
            known.password = prefs.password;
            known.lost = false;
            return;
        }
    }
    if (wifiKnown.size() >= c_maxNetworks)
        wifiKnown.erase(wifiKnown.begin());
    KnownNetwork known;
    known.ssid = prefs.ssid;
    known.password = prefs.password;
    wifiKnown.push_back(std::move(known));
}

/**
 * Makes a saved network the one in use and connects to it; one whose password is lost asks for it.
 */
void KeyboardView_320x240::useNetwork(size_t index)
{
    if (index >= wifiKnown.size())
        return;
    prefs.ssid = wifiKnown[index].ssid;
    prefs.password = wifiKnown[index].password;
    passwordLost = wifiKnown[index].lost;
    if (passwordLost) {
        openWifiPassword(false);
        return;
    }
    prefs.wifi = true;
    savePrefs();
    applyWifi();
    toast(("Connecting to " + prefs.ssid).c_str());
}

void KeyboardView_320x240::openSavedNetworks(void)
{
    if (wifiKnown.empty()) {
        toast("No network saved yet");
        return;
    }
    std::vector<std::string> items;
    int32_t current = -1;
    for (size_t i = 0; i < wifiKnown.size(); i++) {
        items.push_back(wifiKnown[i].ssid + (wifiKnown[i].lost ? "  (password lost)" : ""));
        if (wifiKnown[i].ssid == prefs.ssid)
            current = (int32_t)i;
    }
    openPicker(Setting::WifiSaved, "Saved networks", std::move(items), {}, current,
               "Out of reach of one, the strongest of the others is used");
}

// -------- upload page --------

void KeyboardView_320x240::toggleUpload(void)
{
    if (uploading) {
        delete server;
        server = nullptr;
        uploading = false;
        if (menu == Menu::Info && popup.what == Setting::Upload)
            closeMenu();
        toast("No longer receiving files");
        dirty = true;
        return;
    }
    if (!wifiUp) {
        toast(prefs.wifi || network.wifi_enabled ? "Wi-Fi is not connected yet" : "Turn Wi-Fi on first");
        return;
    }
    if (!ensureStorage()) {
        toast("No SD card");
        return;
    }
    server = new UploadServer(*storage);
    uploading = true; // clockTask() keeps the radio from thinking that the screen went away
    showUpload();
}

/**
 * Where the upload page is, with the code it asks for; the QR code carries both.
 */
void KeyboardView_320x240::showUpload(void)
{
    if (!server)
        return;
    char buf[64];
    const std::string address = wifiAddress();
    std::vector<std::string> lines = {"In a browser open", address, "port " + std::to_string(UploadServer::c_port), "",
                                      std::string("Code ") + server->code(), ""};
    if (server->busy()) {
        const uint64_t expected = server->expected();
        snprintf(buf, sizeof(buf), "%u%% of %s", (unsigned)(expected ? server->received() * 100 / expected : 0),
                 sizeText(expected).c_str());
        lines.push_back(tail(baseName(server->file()), 17));
        lines.push_back(buf);
    } else if (server->saved() || server->failed()) {
        snprintf(buf, sizeof(buf), server->saved() == 1 ? "%u file saved" : "%u files saved", (unsigned)server->saved());
        lines.push_back(buf);
        if (server->failed()) {
            snprintf(buf, sizeof(buf), "%u failed", (unsigned)server->failed());
            lines.push_back(buf);
        }
    } else {
        lines.push_back(wifiUp ? "Waiting for files" : "Wi-Fi not connected");
    }
    if (menu == Menu::Info && popup.what == Setting::Upload) {
        // already on screen: only what changed
        if (popup.lines != lines) {
            popup.lines = std::move(lines);
            dirty = true;
        }
        return;
    }
    snprintf(buf, sizeof(buf), "http://%s:%u/?c=%s", address.c_str(), (unsigned)UploadServer::c_port, server->code());
    openInfo("Receiving files", std::move(lines), buf);
    popup.what = Setting::Upload;
}

// -------- file browser --------

/**
 * A folder of the card as a list: folders first, OK enters one or asks what to do with a file.
 */
void KeyboardView_320x240::openFiles(const std::string &path)
{
    if (!ensureStorage()) {
        toast("No SD card");
        return;
    }
    std::vector<std::pair<std::string, bool>> entries;
    bool more = false;
    const bool listed = storage->list(path.c_str(), [&entries, &more](const char *name, bool isDir) {
        if (entries.size() < c_maxEntries)
            entries.emplace_back(name, isDir);
        else
            more = true;
    });
    if (!listed && path != "/") {
        openFiles("/");
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const std::pair<std::string, bool> &a, const std::pair<std::string, bool> &b) {
        return a.second != b.second ? a.second : folded(a.first.c_str()) < folded(b.first.c_str());
    });

    filesPath = path;
    fileNames.clear();
    std::vector<std::string> items;
    if (path != "/") {
        items.push_back(".. (up)");
        fileNames.push_back("..");
    }
    for (const auto &[name, isDir] : entries) {
        if (name.empty() || name == "." || name == "..")
            continue;
        items.push_back(isDir ? name + "/" : name);
        fileNames.push_back(isDir ? name + "/" : name);
    }
    std::string note = tail(path, 20);
    StorageInfo info;
    if (storage->info(info) && info.statsValid && info.size)
        note += " · " + sizeText(info.free) + " free";
    if (more)
        note += " · first " + std::to_string(c_maxEntries);
    if (items.empty()) {
        items.push_back("(empty)");
        fileNames.push_back("");
    }
    openPicker(Setting::Files, "SD card", std::move(items), {}, -1, note.c_str());
}

// -------- hooks --------

bool KeyboardView_320x240::filesPick(Setting what, uint32_t value, uint32_t target)
{
    switch (what) {
    case Setting::WifiNetwork:
        if (value == c_typeName) {
            openEditor(Setting::WifiSsid, "Network name", prefs.ssid, 32, "Name of the network");
        } else if (value < foundNetworks.size()) {
            const std::string &name = foundNetworks[value];
            const auto known =
                std::find_if(wifiKnown.begin(), wifiKnown.end(), [&name](const KnownNetwork &k) { return k.ssid == name; });
            if (name != prefs.ssid && known != wifiKnown.end()) {
                useNetwork((size_t)(known - wifiKnown.begin())); // its password is known: no question
                return true;
            }
            if (name != prefs.ssid) {
                prefs.ssid = name;
                prefs.password.clear();
                passwordLost = false;
            }
            openWifiPassword(!prefs.password.empty());
        }
        return true;
    case Setting::WifiSaved:
        if (value < wifiKnown.size())
            openPicker(Setting::WifiSavedMenu, wifiKnown[value].ssid.c_str(), {"Connect", "Forget"}, {0, 1}, -1, "", value);
        return true;
    case Setting::WifiSavedMenu:
        if (target >= wifiKnown.size())
            return true;
        if (value == 0) {
            useNetwork(target);
        } else {
            const bool inUse = wifiKnown[target].ssid == prefs.ssid;
            toast((wifiKnown[target].ssid + " forgotten").c_str());
            wifiKnown.erase(wifiKnown.begin() + target);
            if (inUse) {
                // on with the next one, or without a network
                prefs.ssid = wifiKnown.empty() ? "" : wifiKnown.front().ssid;
                prefs.password = wifiKnown.empty() ? "" : wifiKnown.front().password;
                passwordLost = !wifiKnown.empty() && wifiKnown.front().lost;
                if (wifiKnown.empty())
                    prefs.wifi = false;
            }
            savePrefs();
            applyWifi();
        }
        return true;
    case Setting::Upload:
        if (value == 1)
            showUpload();
        else if (value == 2 && uploading)
            toggleUpload();
        return true;
    case Setting::Files: {
        if (value >= fileNames.size() || fileNames[value].empty())
            return true;
        const std::string name = fileNames[value];
        if (name == "..") {
            const size_t slash = filesPath.rfind('/');
            openFiles(slash == 0 || slash == std::string::npos ? "/" : filesPath.substr(0, slash));
        } else if (name.back() == '/') {
            openFiles(joined(filesPath, name.substr(0, name.size() - 1)));
        } else {
            const std::string full = joined(filesPath, name);
            std::string note = tail(full, 36);
            if (auto file = storage->open(full.c_str(), OpenMode::Read))
                note = sizeText(file->size()) + " · " + tail(full, 24);
            openPicker(Setting::FileMenu, tail(name, 28).c_str(), {"Back to the list", "Delete this file"}, {0, 1}, -1, note.c_str(),
                       value);
        }
        return true;
    }
    case Setting::FileMenu:
        if (value == 1 && target < fileNames.size() && ensureStorage()) {
            const std::string full = joined(filesPath, fileNames[target]);
            if (server && server->busy() && server->file() == full) {
                toast("This file is arriving right now"); // deleted under the upload it would damage the card's file system
            } else {
                if (full == "/geo/towns.bin")
                    towns.close(); // the town data is read through an open file
                toast(storage->remove(full.c_str()) ? "Deleted" : "Could not delete it");
            }
        }
        openFiles(filesPath);
        return true;
    default:
        return false;
    }
}

bool KeyboardView_320x240::filesEdit(Setting what, const std::string &text, uint32_t target)
{
    switch (what) {
    case Setting::WifiSsid: {
        const size_t first = text.find_first_not_of(' ');
        if (first == std::string::npos)
            return true;
        const std::string name = text.substr(first, text.find_last_not_of(' ') - first + 1);
        const auto known =
            std::find_if(wifiKnown.begin(), wifiKnown.end(), [&name](const KnownNetwork &k) { return k.ssid == name; });
        if (name != prefs.ssid && known != wifiKnown.end() && !known->lost) {
            useNetwork((size_t)(known - wifiKnown.begin()));
            return true;
        }
        if (name != prefs.ssid) {
            prefs.ssid = name;
            prefs.password.clear();
            passwordLost = false;
        }
        openWifiPassword(!prefs.password.empty());
        return true;
    }
    case Setting::WifiPassword:
        if (prefs.ssid.empty()) {
            toast("Choose a network first");
            return true;
        }
        // the stored password is never put into the editor, so an empty line there leaves it as it is
        if (!(target == c_keepIfEmpty && text.empty()))
            prefs.password = text;
        passwordLost = false;
        prefs.wifi = true;
        rememberNetwork();
        savePrefs();
        applyWifi();
        toast(("Connecting to " + prefs.ssid).c_str());
        return true;
    default:
        return false;
    }
}

bool KeyboardView_320x240::filesSetting(const SettingRow &row)
{
    const bool byRadio = network.wifi_enabled;
    switch (row.id) {
    case Setting::WifiEnabled:
        if (byRadio) {
            toast("Wi-Fi is run by the radio");
        } else if (!prefs.wifi && prefs.ssid.empty()) {
            openNetworks();
        } else {
            prefs.wifi = !prefs.wifi;
            savePrefs();
            applyWifi();
        }
        return true;
    case Setting::WifiNetwork:
        openNetworks();
        return true;
    case Setting::WifiSaved:
        openSavedNetworks();
        return true;
    case Setting::WifiPassword:
        if (byRadio)
            toast("Wi-Fi is run by the radio");
        else if (prefs.ssid.empty())
            openNetworks();
        else
            openWifiPassword(!prefs.password.empty());
        return true;
    case Setting::WifiStatus: {
        std::vector<std::string> lines;
        if (wifiUp) {
            lines.push_back("Connected to " + (byRadio ? std::string(network.wifi_ssid) : prefs.ssid));
            lines.push_back("Address " + wifiAddress());
#if defined(ARDUINO_ARCH_ESP32)
            lines.push_back("Signal " + std::to_string((int)WiFi.RSSI()) + " dBm");
            if (!byRadio)
                lines.push_back(std::string("Name ") + c_hostName + ".local");
#endif
        } else if (byRadio) {
            lines.push_back("The radio runs Wi-Fi and is not connected");
        } else if (!prefs.wifi) {
            lines.push_back("Wi-Fi is off");
        } else {
            lines.push_back("Connecting to " + prefs.ssid);
#if defined(ARDUINO_ARCH_ESP32)
            const wl_status_t status = WiFi.status();
            if (status == WL_NO_SSID_AVAIL)
                lines.push_back("That network is out of range");
            else if (status == WL_CONNECT_FAILED)
                lines.push_back("Refused, is the password right?");
#endif
        }
        lines.push_back("");
        lines.push_back(prefs.tileDownload ? "Missing map tiles are fetched while" : "Map tiles are not fetched");
        if (prefs.tileDownload)
            lines.push_back("the map is open and kept on the card");
        openInfo("Wi-Fi", std::move(lines));
        return true;
    }
    case Setting::TileDownload:
        prefs.tileDownload = !prefs.tileDownload;
        savePrefs();
        applyTileDownload();
        return true;
    case Setting::Upload:
        if (!uploading)
            toggleUpload();
        else
            openPicker(Setting::Upload, "Receiving files", {"Show the address and code", "Stop receiving"}, {1, 2});
        return true;
    case Setting::Files:
        openFiles("/");
        return true;
    default:
        return false;
    }
}

void KeyboardView_320x240::filesRows(std::vector<SettingRow> &rows) const
{
    const bool byRadio = network.wifi_enabled;
    rows.push_back({Setting::WifiEnabled, "Wi-Fi", byRadio ? "Run by the radio" : prefs.wifi ? "On" : "Off"});
    rows.push_back({Setting::WifiNetwork, "Network", byRadio ? network.wifi_ssid : prefs.ssid.empty() ? "None chosen" : prefs.ssid});
    if (!byRadio)
        rows.push_back(
            {Setting::WifiPassword, "Password", passwordLost ? "Type it again" : prefs.password.empty() ? "None" : "Set", 0, passwordLost});
    if (!byRadio)
        rows.push_back({Setting::WifiSaved, "Saved networks", std::to_string(wifiKnown.size())});
    rows.push_back({Setting::WifiStatus, "Status",
                    wifiUp ? wifiAddress() : byRadio || (prefs.wifi && !prefs.ssid.empty()) ? "Connecting" : "Off"});
    rows.push_back({Setting::TileDownload, "Fetch missing map tiles", prefs.tileDownload ? "On" : "Off"});
    rows.push_back(
        {Setting::Upload, "Receive files", uploading && server ? std::string("Code ") + server->code() : "Off"});
    rows.push_back({Setting::Files, "Browse the SD card", ""});
}

#endif
