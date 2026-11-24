#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include "util/GeoMath.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace kbview;

/**
 * Trip: distance and time of this outing and a lifetime odometer, from the fixes of the own GPS as the radio
 * passes them on (about one a minute, so a winding path is measured short).
 *
 * Wardrive: a line in a file on the card for every packet heard without a relay, with where this device was
 * and all that is known of the node. As it comes it only listens. With pings switched on it also asks, every
 * so often and without hops, who can hear it: the radio sends the own position to everyone around and asks
 * for theirs (see jinxmesh_ping() of the firmware), and the answers are rows like any other.
 */

namespace
{

constexpr double c_minStep = 20.0;       // m: less than this is the GPS wandering, it waits for more
constexpr double c_maxSpeed = 200 / 3.6; // m/s: a faster step is a jump of the GPS and is dropped
constexpr uint32_t c_saveInterval = 5 * 60 * 1000; // ms between two saves of a trip that moves
constexpr uint32_t c_satellites = 4;
constexpr uint32_t c_maxGap = 15 * 60;   // s between two fixes; more is a pause or the clock being set, not a step
constexpr uint32_t c_maxStretch = 30 * 86400; // s a trip can run in one go before its start is taken for a wrong clock

const char *const c_logFolder = "/apps/wardrive";
constexpr uint32_t c_nodeInterval = 5000;   // ms between two rows of one node
constexpr uint32_t c_quietInterval = 60000; // ms without a row after which one says that nothing was heard
constexpr uint32_t c_spaceInterval = 60000; // ms between two looks at the free space
constexpr uint32_t c_minFreeMb = 50;
constexpr size_t c_lastKept = 5;
constexpr uint32_t c_rowInterval = 250;  // ms between two rows, whoever they are of
constexpr uint8_t c_maxFailures = 3;     // writes that may fail in a row before logging stops
constexpr uint32_t c_answerWindow = 60000; // ms after a ping in which who is heard counts as having answered
constexpr float c_busyChannel = 25.0f;   // percent of channel use above which no ping goes out
constexpr uint16_t c_pingChoices[] = {0, 60, 120, 300, 600, 900, 1800};

const char *const c_roles[] = {"client",        "client_mute",    "router",      "router_client", "repeater",
                               "tracker",       "sensor",         "tak",         "client_hidden", "lost_and_found",
                               "tak_tracker",   "router_late",    "client_base"};

std::string pingName(uint16_t seconds)
{
    if (!seconds)
        return "Listen only";
    return "Ping every " + (seconds < 60 ? std::to_string(seconds) + " s" : std::to_string(seconds / 60) + " min");
}

std::string durationClock(uint32_t seconds)
{
    char buf[24];
    if (seconds >= 3600)
        snprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)(seconds / 3600), (unsigned)(seconds % 3600 / 60),
                 (unsigned)(seconds % 60));
    else
        snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    return buf;
}

std::string speedText(double metersPerSecond, bool imperial)
{
    char buf[24];
    if (imperial)
        snprintf(buf, sizeof(buf), "%.1f mph", metersPerSecond * 2.236936);
    else
        snprintf(buf, sizeof(buf), "%.1f km/h", metersPerSecond * 3.6);
    return buf;
}

/**
 * A distance for an odometer: always the larger unit, "0.42 km", "12.3 mi".
 */
std::string tripDistance(double meters, bool imperial)
{
    char buf[24];
    const double value = imperial ? meters / 1609.344 : meters / 1000.0;
    snprintf(buf, sizeof(buf), value < 100 ? "%.2f %s" : "%.1f %s", value, imperial ? "mi" : "km");
    return buf;
}

std::string quoted(const std::string &text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"')
            out += '"';
        if ((unsigned char)c >= 0x20)
            out += c;
    }
    return out + "\"";
}

} // namespace

// -------- Trip --------

/**
 * A fix of the own GPS. The odometer always counts; the trip only while it runs.
 */
void KeyboardView_320x240::tripFix(int32_t latitude, int32_t longitude, uint32_t satellites)
{
    if ((satellites && satellites < c_satellites) || !validTime(now()) || (latitude == 0 && longitude == 0))
        return;
    const uint32_t time = now();
    const uint32_t seconds = time > tripFixTime ? time - tripFixTime : 0;
    const bool first = !tripAnchored;
    tripFixTime = time;
    if (first) {
        tripAnchored = true;
        tripLatitude = latitude;
        tripLongitude = longitude;
        return;
    }
    const double meters = geo::leg(tripLatitude, tripLongitude, latitude, longitude).meters;
    if (meters < c_minStep)
        return; // stays where it measures from; standing still does not add up
    tripLatitude = latitude;
    tripLongitude = longitude;
    if (!seconds || seconds > c_maxGap || meters / seconds > c_maxSpeed)
        return; // a jump, a long gap, or the clock was set in between: measured from here on
    odometerMeters += meters;
    if (tripRunning) {
        tripMeters += meters;
        tripMoving += seconds;
        tripTop = std::max(tripTop, (float)(meters / seconds));
        tripTrack(time, latitude, longitude, meters / seconds);
    }
    tripUnsaved = true;
    if (inApp() && app == App::Trip)
        dirty = true;
}

/**
 * The way of a running trip on the card: /apps/trip/<yyyy-mm-dd>.csv (the day by the local clock), a row per
 * fix that counted, at most one in five seconds. A card that is not there costs nothing but the track.
 */
void KeyboardView_320x240::tripTrack(uint32_t time, int32_t latitude, int32_t longitude, double speed)
{
    if (demoData || (tripTracked && time < tripTracked + 5) || !ensureStorage())
        return;
    tripTracked = time;
    char buf[160];
    const time_t t = time;
    struct tm local, utc;
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
    snprintf(buf, sizeof(buf), "/apps/trip/%04d-%02d-%02d.csv", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    const std::string path = buf;
    const bool fresh = !storage->exists(path.c_str());
    if (fresh) {
        storage->mkdir("/apps");
        storage->mkdir("/apps/trip");
    }
    std::unique_ptr<IFile> file = storage->open(path.c_str(), OpenMode::Append);
    if (!file)
        return;
    std::string row = fresh ? "time_utc,lat,lon,alt_m,speed_mps,trip_m\n" : "";
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    row += buf;
    const NodeRecord *own = nodeStore.find(ownNode);
    snprintf(buf, sizeof(buf), ",%.6f,%.6f,%d,%.1f,%.0f\n", latitude * 1e-7, longitude * 1e-7,
             own && own->position.hasCoordinates() ? (int)own->position.altitude : 0, speed, tripMeters);
    row += buf;
    if (file->write(row.data(), (uint32_t)row.size()) == (int32_t)row.size())
        tripFile = path;
}

/**
 * A trip that moves is saved every few minutes, so that a flat battery costs little of it.
 */
void KeyboardView_320x240::tripTask(void)
{
    if (tripUnsaved && lv_tick_elaps(tripSaved) >= c_saveInterval) {
        tripUnsaved = false;
        tripSaved = lv_tick_get();
        savePrefs();
    }
    // the clock of a running trip
    if (tripRunning && inApp() && app == App::Trip && !screenOff && lv_tick_elaps(lastRender) >= 1000)
        dirty = true;
}

uint32_t KeyboardView_320x240::tripElapsedNow(void) const
{
    const uint32_t time = now();
    // a stretch of more than a month is the clock having been set while it ran, not a trip
    const uint32_t stretch = tripRunning && validTime(time) && time > tripResumed ? time - tripResumed : 0;
    return tripElapsed + (stretch <= c_maxStretch ? stretch : 0);
}

/**
 * OK: start, pause, go on.
 */
void KeyboardView_320x240::tripToggle(void)
{
    if (!validTime(now())) {
        toast("The trip needs the time: wait for GPS");
        return;
    }
    if (tripRunning) {
        tripElapsed = tripElapsedNow();
        tripRunning = false;
    } else {
        tripResumed = now();
        tripRunning = true;
    }
    tripUnsaved = false;
    tripSaved = lv_tick_get();
    savePrefs();
}

void KeyboardView_320x240::tripKey(uint32_t key)
{
    if (key == LV_KEY_LEFT)
        closeApp();
    else if (key == LV_KEY_RIGHT)
        activateApp();
}

void KeyboardView_320x240::tripHold(void)
{
    openPicker(Setting::TripMenu, "Trip", {"Reset the trip", "Reset the odometer ...", "Send the track by Bluetooth"}, {0, 1, 2}, -1,
               "A running trip writes its way to /apps/trip on the card");
}

extern "C" {
// of the firmware; stays null where it has none (the simulator, other firmware)
int jinxmesh_ping(int channel) __attribute__((weak));
// tells the device that is connected by Bluetooth that a file of the card is there to fetch; 0: none listens
int jinxmesh_offer(const char *path) __attribute__((weak));
}

/**
 * A file of the card (or a folder, with a / at its end, to choose from) to whatever is connected by Bluetooth.
 */
void KeyboardView_320x240::offerFile(const char *path)
{
    if (!jinxmesh_offer)
        toast("This radio cannot send files");
    else if (jinxmesh_offer(path))
        toast("Sent to the connected device");
    else
        toast("Nothing connected by Bluetooth that takes files", 3000);
}

bool KeyboardView_320x240::tripPick(Setting what, uint32_t value, uint32_t)
{
    if (what == Setting::WardriveMenu) {
        if (value == 0) {
            wardriveToggle();
        } else if (value == 1) {
            std::vector<std::string> items;
            std::vector<uint32_t> values;
            int32_t current = 0;
            for (uint16_t seconds : c_pingChoices) {
                if (seconds == prefs.wardrivePing)
                    current = (int32_t)items.size();
                items.push_back(pingName(seconds));
                values.push_back(seconds);
            }
            openPicker(Setting::WardriveMode, "Wardrive mode", std::move(items), std::move(values), current,
                       "Nodes answer a ping at most every 3 minutes");
        } else if (value == 3) {
            // the log of now, or the folder for the other side to choose from
            offerFile(wardriveFile.empty() ? (std::string(c_logFolder) + "/").c_str() : wardriveFile.c_str());
        } else if (value == 4) {
            wardrivePingNow();
        } else if (value == 5) {
            if (!prefs.wardrive) {
                toast("A mark needs the log: start logging first");
            } else if (wardriveWrite(0, 0, 0, 5)) {
                char text[24];
                snprintf(text, sizeof(text), "Mark %u", (unsigned)wardriveMarks);
                toast(text);
            } else {
                toast(wardriveNote.c_str());
            }
        } else {
            std::vector<std::string> items;
            std::vector<uint32_t> values;
            int32_t current = -1;
            for (uint8_t i = 0; i < c_max_channels; i++) {
                if (!channelInfo[i].enabled)
                    continue;
                if (i == wardrivePingChannel())
                    current = (int32_t)items.size();
                items.push_back("# " + chatName(i));
                values.push_back(i);
            }
            openPicker(Setting::WardriveChannel, "Ping on the channel", std::move(items), std::move(values), current,
                       "Only nodes that have the channel can answer");
        }
        return true;
    }
    if (what == Setting::WardriveMode) {
        prefs.wardrivePing = (uint16_t)value;
        wardrivePingTick = 0;
        savePrefs();
        toast(pingName(prefs.wardrivePing).c_str());
        dirty = true;
        return true;
    }
    if (what == Setting::WardriveChannel) {
        prefs.wardriveChannel = (uint8_t)value;
        savePrefs();
        dirty = true;
        return true;
    }
    if (what == Setting::TripMenu) {
        if (value == 0) {
            tripRunning = false;
            tripMeters = 0;
            tripElapsed = tripMoving = 0;
            tripTop = 0;
            savePrefs();
            toast("Trip set to zero");
        } else if (value == 2) {
            offerFile(tripFile.empty() ? "/apps/trip/" : tripFile.c_str());
        } else {
            openPicker(Setting::OdometerReset, "Set the odometer to zero?", {"No, keep it", "Yes, set it to zero"}, {0, 1}, 0,
                       tripDistance(odometerMeters, imperialUnits()).c_str());
        }
        return true;
    }
    if (what == Setting::OdometerReset) {
        if (value == 1) {
            odometerMeters = 0;
            savePrefs();
            toast("Odometer set to zero");
        }
        return true;
    }
    return false;
}

// "running,meters,elapsed,resumed,moving,top in mm/s" as it stands in the file of the view
std::string KeyboardView_320x240::tripLine(void) const
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%u,%.0f,%u,%u,%u,%u", (unsigned)tripRunning, tripMeters, (unsigned)tripElapsed,
             (unsigned)tripResumed, (unsigned)tripMoving, (unsigned)lround(tripTop * 1000.0f));
    return buf;
}

void KeyboardView_320x240::setTripLine(const std::string &line)
{
    unsigned running = 0, elapsed = 0, resumed = 0, moving = 0, top = 0;
    double meters = 0;
    if (sscanf(line.c_str(), "%u,%lf,%u,%u,%u,%u", &running, &meters, &elapsed, &resumed, &moving, &top) != 6)
        return;
    tripRunning = running != 0;
    tripMeters = meters >= 0 && meters < 1e9 ? meters : 0;
    tripElapsed = elapsed;
    tripResumed = resumed;
    tripMoving = moving;
    tripTop = top / 1000.0f;
}

void KeyboardView_320x240::renderTrip(void)
{
    const bool imperial = imperialUnits();
    const bool started = tripRunning || tripElapsed || tripMeters > 0;
    label(content, 8, 6, 150, font12, tripRunning ? c_focus : c_muted, tripRunning ? "Running" : started ? "Paused" : "Not started");
    label(content, 8, 22, c_width - 16, font20, c_text, tripDistance(tripMeters, imperial).c_str());
    label(content, 170, 26, c_width - 178, font16, c_text, durationClock(tripElapsedNow()).c_str(), LV_TEXT_ALIGN_RIGHT);

    const int32_t top = 54;
    int32_t row = 0;
    const auto line = [this, top, &row](const char *name, const std::string &value) {
        settingRow(content, top + row++ * c_settingHeight, false, name, value.c_str(), false);
    };
    line("Moving time", durationClock(tripMoving));
    line("Average while moving", tripMoving ? speedText(tripMeters / tripMoving, imperial) : std::string("--"));
    line("Fastest stretch", tripTop > 0 ? speedText(tripTop, imperial) : std::string("--"));
    line("Odometer", tripDistance(odometerMeters, imperial));
    const NodeRecord *own = nodeStore.find(ownNode);
    const char *note = !own || !own->position.hasCoordinates() ? "No GPS fix: nothing is counted"
                                                              : "Counts from fix to fix, about one a minute";
    label(content, 8, 168, c_width - 16, font12, own && own->position.hasCoordinates() ? c_muted : c_focus, note,
          LV_TEXT_ALIGN_CENTER);
    label(content, 8, 196, c_width - 16, font12, c_focusMuted,
          tripRunning ? "OK: pause   Hold OK: reset" : started ? "OK: go on   Hold OK: reset" : "OK: start   Hold OK: reset",
          LV_TEXT_ALIGN_CENTER);
}

KeyboardView_320x240::TileView KeyboardView_320x240::tripTile(void) const
{
    TileView view;
    const bool started = tripRunning || tripElapsed || tripMeters > 0;
    view.value = started ? tripDistance(tripMeters, imperialUnits()) : std::string("--");
    view.sub = tripRunning ? durationClock(tripElapsedNow() / 60 * 60) + " running" : started ? "Paused" : "OK: start a trip";
    view.accent = tripRunning;
    return view;
}

// -------- Wardrive --------

/**
 * A packet came in without a relay.
 */
void KeyboardView_320x240::wardriveHeard(uint32_t nodeNum, int32_t rssi, float snr)
{
    if (!prefs.wardrive || demoData)
        return;
    // for the line of the screen: who was heard, who within a minute of a ping, who from farthest away
    if (wardriveNodes.size() < 500)
        wardriveNodes.insert(nodeNum);
    if (wardrivePingSent && lv_tick_elaps(wardrivePingSent) < c_answerWindow && wardriveAnswered.size() < 500)
        wardriveAnswered.insert(nodeNum);
    const NodeRecord *own = nodeStore.find(ownNode), *node = nodeStore.find(nodeNum);
    if (own && node && own->position.hasCoordinates() && node->position.hasCoordinates()) {
        const double meters =
            geo::leg(own->position.latitude, own->position.longitude, node->position.latitude, node->position.longitude).meters;
        if (meters > wardriveFarMeters) {
            wardriveFarMeters = meters;
            wardriveFarNode = nodeNum;
        }
    }
    if (lv_tick_elaps(wardriveRowTick) < c_rowInterval)
        return; // a burst of packets is not a burst of card writes
    const auto last = wardriveNodeTick.find(nodeNum);
    if (last != wardriveNodeTick.end() && lv_tick_elaps(last->second) < c_nodeInterval)
        return;
    if (wardriveNodeTick.size() > 300)
        wardriveNodeTick.clear();
    wardriveNodeTick[nodeNum] = lv_tick_get();
    if (wardriveWrite(nodeNum, rssi, snr)) {
        wardriveFailures = 0;
    } else if (validTime(now()) && ++wardriveFailures >= c_maxFailures) {
        // a card that is gone or full: every further try would hold the screen up for its timeouts
        wardriveFailures = 0;
        wardriveStop("Stopped: cannot write to the card");
    }
}

/**
 * Where nothing was heard is worth knowing too: a row without a node after a quiet minute.
 */
void KeyboardView_320x240::wardriveTask(void)
{
    if (!prefs.wardrive || demoData)
        return;
    if (lv_tick_elaps(wardriveChecked) >= c_spaceInterval) {
        wardriveChecked = lv_tick_get();
        StorageInfo info;
        if (!ensureStorage() || !storage->info(info)) {
            wardriveNote = "No SD card";
        } else {
            if (info.statsValid)
                wardriveFreeMb = (uint32_t)(info.free / (1024 * 1024));
            if (info.statsValid && wardriveFreeMb < c_minFreeMb) {
                wardriveStop("Stopped: less than 50 MB free on the card");
                return;
            }
        }
    }
    if (prefs.wardrivePing && (!wardrivePingTick || lv_tick_elaps(wardrivePingTick) >= prefs.wardrivePing * 1000u)) {
        wardrivePingTick = lv_tick_get() ? lv_tick_get() : 1;
        // a busy channel is left alone; the next turn tries again
        const bool busy = hasAirtime && channelUse > c_busyChannel;
        const bool sent = !busy && jinxmesh_ping && jinxmesh_ping(wardrivePingChannel());
        if (sent) {
            wardrivePings++;
            wardrivePingSent = wardrivePingTick;
        }
        wardriveWrite(0, 0, 0, sent ? 2 : 3);
        return;
    }
    if (lv_tick_elaps(wardriveRowTick) >= c_quietInterval) {
        if (wardriveWrite(0, 0, 0, 1)) {
            wardriveFailures = 0;
        } else if (validTime(now()) && ++wardriveFailures >= c_maxFailures) {
            wardriveFailures = 0;
            wardriveStop("Stopped: cannot write to the card");
        }
    }
}

/**
 * One ping at once, whatever the mode; a row of the log when that runs.
 */
void KeyboardView_320x240::wardrivePingNow(void)
{
    if (demoData) {
        toast("Not with the sample mesh");
        return;
    }
    const bool busy = hasAirtime && channelUse > c_busyChannel;
    const bool sent = !busy && jinxmesh_ping && jinxmesh_ping(wardrivePingChannel());
    if (sent) {
        wardrivePings++;
        wardrivePingTick = wardrivePingSent = lv_tick_get() ? lv_tick_get() : 1; // the next of a row of pings counts from here
    }
    if (prefs.wardrive)
        wardriveWrite(0, 0, 0, sent ? 2 : 3);
    if (sent)
        toast(("Ping sent on # " + chatName(wardrivePingChannel())).c_str());
    else
        toast(!jinxmesh_ping ? "This radio cannot ping" : busy ? "No ping: the channel is busy" : "No ping: no position yet", 3000);
    dirty = true;
}

/**
 * Appends a row to the file of the day: /apps/wardrive/<yyyy-mm-dd>-3.csv, the day by the local clock.
 */
bool KeyboardView_320x240::wardriveWrite(uint32_t nodeNum, int32_t rssi, float snr, uint8_t event)
{
    wardriveRowTick = lv_tick_get();
    const uint32_t time = now();
    if (!validTime(time)) {
        wardriveNote = "Waiting for the time";
        return false;
    }
    if (!ensureStorage()) {
        wardriveNote = "No SD card";
        return false;
    }
    char buf[200];
    const time_t t = time;
    struct tm local, utc;
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
    // "-3": the columns of these files; the kinds before had fewer
    snprintf(buf, sizeof(buf), "%s/%04d-%02d-%02d-3.csv", c_logFolder, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    const std::string path = buf;
    const bool fresh = !storage->exists(path.c_str());
    if (fresh) {
        storage->mkdir("/apps");
        storage->mkdir(c_logFolder);
    }
    std::unique_ptr<IFile> file = storage->open(path.c_str(), OpenMode::Append);
    if (!file) {
        wardriveNote = "Cannot write to the card";
        return false;
    }
    std::string row;
    if (fresh)
        row = "time_utc,event,mode,lat,lon,alt_m,fix_age_s,node,short_name,long_name,role,hw_model,rssi_dbm,snr_db,channel,"
              "node_lat,node_lon,node_alt_m,distance_m,bearing_deg,node_battery_pct,node_voltage,node_ch_util_pct,"
              "node_air_util_tx_pct,via_mqtt,has_key,since_ping_s,ch_util_pct,air_util_tx_pct,preset,slot,town,mark\n";
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    row += buf;
    static const char *const events[] = {"heard", "quiet", "ping", "ping_not_sent", "start", "mark"};
    row += std::string(",") + events[event < 6 ? event : 0] + (prefs.wardrivePing ? ",active" : ",passive");
    const NodeRecord *own = nodeStore.find(ownNode);
    const bool here = own && own->position.hasCoordinates();
    std::string town;
    if (here) {
        const auto fix = positionTimes.find(ownNode);
        snprintf(buf, sizeof(buf), ",%.6f,%.6f,%d,", own->position.latitude * 1e-7, own->position.longitude * 1e-7,
                 (int)own->position.altitude);
        row += buf;
        if (fix != positionTimes.end() && time >= fix->second)
            row += std::to_string(time - fix->second);
        town = townOf(own->position.latitude, own->position.longitude);
    } else {
        row += ",,,,";
    }
    const NodeRecord *node = nodeNum ? nodeStore.find(nodeNum) : nullptr;
    if (nodeNum) {
        snprintf(buf, sizeof(buf), ",!%08x,", (unsigned)nodeNum);
        row += buf + quoted(shortName(nodeNum)) + ",";
        if (node && node->hasUser) {
            const unsigned role = (unsigned)node->user.role;
            row += quoted(node->user.long_name) + ",";
            row += role < sizeof(c_roles) / sizeof(c_roles[0]) ? std::string(c_roles[role]) : std::to_string(role);
            row += "," + std::to_string((unsigned)node->user.hw_model);
        } else {
            row += ",,";
        }
        snprintf(buf, sizeof(buf), ",%d,%.1f,%u", (int)rssi, snr, node ? (unsigned)node->channel : 0u);
        row += buf;
        // where the node says it is, and how far that is from here
        if (node && node->position.hasCoordinates()) {
            snprintf(buf, sizeof(buf), ",%.6f,%.6f,%d", node->position.latitude * 1e-7, node->position.longitude * 1e-7,
                     (int)node->position.altitude);
            row += buf;
            if (here) {
                const geo::Leg leg = geo::leg(own->position.latitude, own->position.longitude, node->position.latitude,
                                              node->position.longitude);
                snprintf(buf, sizeof(buf), ",%.0f,%.0f", leg.meters, leg.bearing);
                row += buf;
            } else {
                row += ",,";
            }
        } else {
            row += ",,,,,";
        }
        if (node && node->hasDeviceMetrics) {
            snprintf(buf, sizeof(buf), ",%u,%.2f,%.1f,%.1f", (unsigned)node->deviceMetrics.battery_level, node->deviceMetrics.voltage,
                     node->deviceMetrics.channel_utilization, node->deviceMetrics.air_util_tx);
            row += buf;
        } else {
            row += ",,,,";
        }
        row += node ? (node->viaMqtt ? ",1" : ",0") : ",";
        row += node ? (node->hasKey ? ",1" : ",0") : ",";
    } else {
        row += ",,,,,,,,,,,,,,,,,,,"; // the 19 columns of a node, empty
    }
    // an answer to a ping comes within seconds of it
    row += ",";
    if (wardrivePingSent)
        row += std::to_string(lv_tick_elaps(wardrivePingSent) / 1000);
    if (hasAirtime) {
        snprintf(buf, sizeof(buf), ",%.1f,%.1f", channelUse, txUse);
        row += buf;
    } else {
        row += ",,";
    }
    // the radio settings this was heard with: the number of the preset (-1: none, its own values) and the slot
    snprintf(buf, sizeof(buf), ",%d,%u,", lora.use_preset ? (int)lora.modem_preset : -1, (unsigned)activeSlot());
    row += buf + quoted(town) + ",";
    if (event == 5)
        row += std::to_string(wardriveMarks + 1);
    row += "\n";
    if (file->write(row.data(), (uint32_t)row.size()) != (int32_t)row.size()) {
        wardriveNote = "Cannot write to the card";
        return false;
    }
    wardriveNote.clear();
    wardriveFile = path;
    wardriveRows++;
    if (event == 5)
        wardriveMarks++;

    // the same in words, for the screen
    char when[16];
    clockString(time, when, sizeof(when), twelveHour(), false);
    if (nodeNum)
        snprintf(buf, sizeof(buf), "%s  %s  %d dBm  %.1f dB", when, shortName(nodeNum).c_str(), (int)rssi, snr);
    else if (event == 2)
        snprintf(buf, sizeof(buf), "%s  ping sent on # %s", when, chatName(wardrivePingChannel()).c_str());
    else if (event == 3)
        snprintf(buf, sizeof(buf), "%s  no ping: %s", when,
                 !jinxmesh_ping ? "this radio has none" : hasAirtime && channelUse > c_busyChannel ? "channel busy" : "no position yet");
    else if (event == 4)
        snprintf(buf, sizeof(buf), "%s  logging starts", when);
    else if (event == 5)
        snprintf(buf, sizeof(buf), "%s  mark %u", when, (unsigned)wardriveMarks);
    else
        snprintf(buf, sizeof(buf), "%s  nothing heard for a minute", when);
    if (wardriveLast.size() >= c_lastKept)
        wardriveLast.erase(wardriveLast.begin());
    wardriveLast.push_back(buf);
    if (inApp() && app == App::Wardrive)
        dirty = true;
    return true;
}

void KeyboardView_320x240::wardriveStop(const char *why)
{
    prefs.wardrive = false;
    wardriveNote = why;
    savePrefs();
    dirty = true;
}

/**
 * OK: start or stop. Starting needs a card and the time, and writes a first row to show that it can.
 */
void KeyboardView_320x240::wardriveToggle(void)
{
    if (prefs.wardrive) {
        wardriveStop("");
        toast("Logging stopped");
        return;
    }
    if (demoData) {
        toast("Not with the sample mesh");
        return;
    }
    if (!ensureStorage()) {
        toast("No SD card");
        return;
    }
    if (!validTime(now())) {
        toast("The log needs the time: wait for GPS");
        return;
    }
    StorageInfo info;
    if (storage->info(info) && info.statsValid) {
        wardriveFreeMb = (uint32_t)(info.free / (1024 * 1024));
        if (wardriveFreeMb < c_minFreeMb) {
            toast("Less than 50 MB free on the card");
            return;
        }
    }
    if (!wardriveWrite(0, 0, 0, 4)) {
        toast(wardriveNote.c_str());
        return;
    }
    prefs.wardrive = true;
    wardriveChecked = lv_tick_get();
    wardrivePingTick = wardrivePingSent = 0; // the first ping goes out right away
    savePrefs();
    toast(prefs.wardrivePing ? "Logging, and pinging" : "Logging what is heard");
}

/**
 * The channel the ping goes out on: the chosen one, or else the public one (the default key) that most of the
 * nodes around can read, or else the first.
 */
uint8_t KeyboardView_320x240::wardrivePingChannel(void) const
{
    if (prefs.wardriveChannel < c_max_channels && channelInfo[prefs.wardriveChannel].enabled)
        return prefs.wardriveChannel;
    for (uint8_t i = 0; i < c_max_channels; i++) {
        const meshtastic_ChannelSettings &settings = channels[i].settings;
        if (channelInfo[i].enabled && settings.psk.size == 1 && settings.psk.bytes[0] == 1)
            return i;
    }
    return 0;
}

/**
 * Hold OK: start and stop, whether it pings and how often, on which channel, and the log to whatever is
 * connected by Bluetooth (which fetches it; see JinxBle.cpp of the firmware).
 */
void KeyboardView_320x240::wardriveHold(void)
{
    openPicker(Setting::WardriveMenu, "Wardrive",
               {prefs.wardrive ? "Stop logging" : "Start logging", "Mode: " + pingName(prefs.wardrivePing),
                "Ping on: # " + chatName(wardrivePingChannel()), "Ping now", "Mark this place", "Send the log by Bluetooth"},
               {0, 1, 2, 4, 5, 3}, -1, "A ping asks who can hear this device");
}

void KeyboardView_320x240::renderWardrive(void)
{
    char buf[64];
    label(content, 8, 6, 200, font20, prefs.wardrive ? c_focus : c_text, prefs.wardrive ? "Logging" : "Not logging");
    snprintf(buf, sizeof(buf), "%u rows", (unsigned)wardriveRows);
    label(content, 200, 10, c_width - 208, font16, c_text, buf, LV_TEXT_ALIGN_RIGHT);
    std::string state = wardriveNote;
    if (state.empty())
        state = wardriveFile.empty() ? std::string("Writes to ") + c_logFolder + " on the card" : wardriveFile;
    label(content, 8, 34, c_width - 16, font12, wardriveNote.empty() ? c_muted : c_focus, state.c_str());
    if (!wardriveNodes.empty()) {
        std::string sum = std::to_string(wardriveNodes.size()) + (wardriveNodes.size() == 1 ? " node" : " nodes");
        if (wardrivePings)
            sum += " · " + std::to_string(wardriveAnswered.size()) + " after a ping";
        if (wardriveFarNode)
            sum += " · farthest " + shortName(wardriveFarNode) + " " + tripDistance(wardriveFarMeters, imperialUnits());
        label(content, 8, 50, c_width - 16, font12, c_text, sum.c_str());
    } else if (wardriveFreeMb) {
        snprintf(buf, sizeof(buf), "%u MB free on the card", (unsigned)wardriveFreeMb);
        label(content, 8, 50, c_width - 16, font12, c_muted, buf);
    }
    int32_t y = 72;
    if (wardriveLast.empty())
        label(content, 8, y + 30, c_width - 16, font14, c_muted, "Nothing logged yet", LV_TEXT_ALIGN_CENTER);
    for (auto it = wardriveLast.rbegin(); it != wardriveLast.rend(); ++it, y += 20)
        label(content, 8, y, c_width - 16, font14, it == wardriveLast.rbegin() ? c_text : c_muted, it->c_str());
    std::string mode = "Only listens: nothing is sent for the log";
    if (prefs.wardrivePing) {
        mode = pingName(prefs.wardrivePing) + " on # " + chatName(wardrivePingChannel());
        if (wardrivePings)
            mode += " · " + std::to_string(wardrivePings) + " sent";
    }
    label(content, 8, 178, c_width - 16, font12, prefs.wardrivePing ? c_focus : c_muted, mode.c_str(), LV_TEXT_ALIGN_CENTER);
    label(content, 8, 196, c_width - 16, font12, c_focusMuted, prefs.wardrive ? "OK: stop   Hold OK: menu" : "OK: start   Hold OK: menu",
          LV_TEXT_ALIGN_CENTER);
}

#endif
