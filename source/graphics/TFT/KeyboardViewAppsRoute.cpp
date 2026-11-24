#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/map/MapTileSettings.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include "util/GeoMath.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

using namespace kbview;

/**
 * Navigate: a route over roads and trails to a node or a place, the next turn, the list of turns, and the line
 * on the map. The roads come from /geo/roads.bin on the card (util/RouteGraph.h, built by
 * kbview-harness/maps/roads.py from OpenStreetMap); without that file the screen says where it goes.
 */

// The firmware keeps the receiver on while somebody follows the position, and hands out every fix. Where it has
// neither (the simulator, other boards) the position is what the radio reports, about once a minute.
extern "C" {
int jinxmesh_fix(int32_t *latitude, int32_t *longitude, uint32_t *ageMs) __attribute__((weak));
void jinxmesh_track(unsigned seconds) __attribute__((weak));
// what the page sent over Bluetooth, a line at a time ("D", "R", "S", "G", "K": routeCommand); 0: nothing waits
int jinxmesh_inbox(char *buffer, int size) __attribute__((weak));
}

namespace
{

const char *const c_roadFile = "/geo/roads.bin";
constexpr uint32_t c_retryTime = 30000;   // ms between two looks for the file
constexpr uint32_t c_slice = 25;          // ms of a pass the search may use
constexpr uint32_t c_pollTime = 2000;     // ms between two looks at the receiver
constexpr uint32_t c_trackSecs = 20;      // the receiver is asked to stay on for this long, again and again
constexpr uint32_t c_freshFix = 5000;     // ms a fix may be old
constexpr int32_t c_movedLat = 250;       // 1e-7 degrees, about 3 m: less is no move
constexpr uint32_t c_offCar = 70, c_offFoot = 40; // meters off the line that count as having left the route
constexpr uint32_t c_offTime = 8000;      // ms off the route before a new one is looked for
constexpr uint32_t c_arriveMeters = 25;
constexpr double c_atStart = 50; // meters from the first stop within which a route is begun without asking
constexpr size_t c_window = 60;           // stretches of the line looked at around the last position
constexpr size_t c_listRows = 8;
constexpr double c_unit = 0.0111319;      // meters in 1e-7 degrees of latitude
const char *const c_planFolder = "/apps/routes";
constexpr size_t c_maxStops = 1024;
constexpr size_t c_planBytes = c_maxStops * 96; // of a route file: a line is two numbers and a name of 24 letters
constexpr size_t c_maxPlans = 40;
constexpr uint32_t c_nextStopTime = 4000; // ms an arrival stays before the route to the next stop is looked for
constexpr uint32_t c_inboxTime = 500;     // ms between two looks for what the page sent
constexpr uint32_t c_planNew = 1000, c_planStopBase = 100;

std::string trimmed(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/**
 * Degrees with a point, latitude first: "44.2601, -72.5754", "44.2601 N 72.5754 W", "N44.2601 W72.5754".
 */
bool parseCoordinates(const std::string &text, int32_t &latitude, int32_t &longitude)
{
    double value[2] = {0, 0};
    char letter[2] = {0, 0};
    char pending = 0;
    int n = 0;
    const char *p = text.c_str();
    while (*p) {
        const char c = (char)toupper((unsigned char)*p);
        if (c == ' ' || c == ',' || c == ';' || c == '\t') {
            p++;
        } else if (c == 'N' || c == 'S' || c == 'E' || c == 'W') {
            if (n > 0 && !letter[n - 1] && !pending)
                letter[n - 1] = c;
            else
                pending = c;
            p++;
        } else {
            char *end = nullptr;
            const double v = strtod(p, &end);
            if (end == p || n >= 2)
                return false;
            value[n] = v;
            letter[n] = pending;
            pending = 0;
            n++;
            p = end;
        }
    }
    if (n != 2 || pending)
        return false;
    if ((letter[0] == 'E' || letter[0] == 'W') && (letter[1] == 'N' || letter[1] == 'S')) {
        std::swap(value[0], value[1]);
        std::swap(letter[0], letter[1]);
    }
    for (int i = 0; i < 2; i++)
        if (letter[i])
            value[i] = (letter[i] == 'S' || letter[i] == 'W') ? -fabs(value[i]) : fabs(value[i]);
    if (fabs(value[0]) > 90.0 || fabs(value[1]) > 180.0 || (value[0] == 0.0 && value[1] == 0.0))
        return false;
    latitude = (int32_t)lround(value[0] * 1e7);
    longitude = (int32_t)lround(value[1] * 1e7);
    return true;
}

/** A name as a file can have it: letters, digits, spaces, - and _. */
std::string fileName(const std::string &name)
{
    std::string out;
    for (char c : trimmed(name))
        if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')
            out += c;
    return trimmed(out.substr(0, 28));
}

std::string coordinateText(int32_t latitude, int32_t longitude)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "%.5f, %.5f", latitude * 1e-7, longitude * 1e-7);
    return buf;
}

#if defined(ARDUINO_ARCH_ESP32)
void *externalAllocate(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}
void externalRelease(void *memory)
{
    heap_caps_free(memory);
}
#endif

lv_obj_t *wrapped(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, uint32_t color, const char *text)
{
    lv_obj_t *obj = label(parent, x, y, 0, font12, color, text);
    lv_obj_set_width(obj, w);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    return obj;
}

const char *wayWord(uint8_t kind)
{
    switch (kind) {
    case RouteGraph::Path:
    case RouteGraph::Cycleway:
        return "the trail";
    case RouteGraph::Steps:
        return "the steps";
    case RouteGraph::Track:
        return "the track";
    case RouteGraph::Ferry:
        return "the ferry";
    default:
        return "the road";
    }
}

std::string timeText(uint32_t seconds)
{
    char buf[24];
    const uint32_t minutes = (seconds + 30) / 60;
    if (minutes >= 60)
        snprintf(buf, sizeof(buf), "%u h %02u min", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
    else
        snprintf(buf, sizeof(buf), "%u min", (unsigned)std::max<uint32_t>(1, minutes));
    return buf;
}

// degrees the arrow of a turn leans to; 0 is straight on
int turnAngle(uint8_t turn)
{
    switch (turn) {
    case RouteGraph::SlightLeft:
        return -40;
    case RouteGraph::Left:
        return -90;
    case RouteGraph::SharpLeft:
        return -135;
    case RouteGraph::SlightRight:
        return 40;
    case RouteGraph::Right:
        return 90;
    case RouteGraph::SharpRight:
        return 135;
    case RouteGraph::UTurn:
        return 180;
    default:
        return 0;
    }
}

const char *failureText(RouteGraph::Progress why)
{
    switch (why) {
    case RouteGraph::Progress::NoWayNearStart:
        return "No road or trail near this device";
    case RouteGraph::Progress::NoWayNearEnd:
        return "No road or trail near the destination";
    case RouteGraph::Progress::NoRoute:
        return "No way there in this mode";
    case RouteGraph::Progress::TooFar:
        return "Too far for this device to work out";
    case RouteGraph::Progress::NoMemory:
        return "Not enough memory for a route now";
    default:
        return "The road data cannot be read";
    }
}

} // namespace

/**
 * Opens the road data once it is there. A card that came later is found on the next look.
 */
bool KeyboardView_320x240::openRoads(void)
{
    if (roads.state() == RouteGraph::State::Ok)
        return true;
    if (roadsTried && lv_tick_elaps(roadsTried) < c_retryTime)
        return false;
    roadsTried = lv_tick_get() | 1;
    if (!ensureStorage() || !storage->exists(c_roadFile))
        return false;
    std::shared_ptr<IFile> file(storage->open(c_roadFile, OpenMode::Read).release());
    if (!file)
        return false;
#if defined(ARDUINO_ARCH_ESP32)
    RouteGraph::setAllocator(externalAllocate, externalRelease);
#endif
    const RouteGraph::State state = roads.open([file](uint32_t offset, void *buffer, uint32_t length) {
        return file->seek(offset) && file->read(buffer, length) == (int32_t)length;
    });
    ILOG_INFO("road data: %s, %u vertices", state == RouteGraph::State::Ok ? "open" : "not usable", (unsigned)roads.vertices());
    return state == RouteGraph::State::Ok;
}

/**
 * A route from where this device is to a position, by car (mode 0) or on foot (1).
 */
void KeyboardView_320x240::navigateTo(int32_t latitude, int32_t longitude, const std::string &name, uint8_t mode, uint32_t node)
{
    routeEnd();
    planRunning = false;
    planNext = 0;
    routeLatitude = latitude;
    routeLongitude = longitude;
    routeName = name;
    routeMode = mode ? 1 : 0;
    routeNode = node;
    openApp(App::Navigate);
    routeStart();
}

/**
 * The route to one stop of the route of several; the stops after it follow as each is reached.
 */
void KeyboardView_320x240::planGo(size_t index, uint8_t mode)
{
    if (index >= planStops.size())
        return;
    routeEnd();
    routeLatitude = planStops[index].latitude;
    routeLongitude = planStops[index].longitude;
    routeName = planStops[index].name;
    routeMode = mode ? 1 : 0;
    routeNode = 0;
    planRunning = true;
    planAt = index;
    planNext = 0;
    routeStart();
}

/**
 * Begins a route of several stops. At its first stop it leads on to the second at once; anywhere else it asks
 * whether to go to the starting point first, or to join the route where this device is: at the end of the
 * stretch between two stops that is closest.
 */
void KeyboardView_320x240::planBegin(uint8_t mode)
{
    if (planStops.empty())
        return;
    const NodeRecord *own = nodeStore.find(ownNode);
    if (planStops.size() < 2 || !own || !own->position.hasCoordinates()) {
        planGo(0, mode); // one stop, or no fix to tell by
        return;
    }
    const double k = cos(own->position.latitude * 1e-7 * M_PI / 180.0);
    const double dy = (planStops[0].latitude - own->position.latitude) * c_unit;
    const double dx = (planStops[0].longitude - own->position.longitude) * c_unit * k;
    const double meters = sqrt(dx * dx + dy * dy);
    if (meters <= c_atStart) {
        planGo(1, mode);
        return;
    }
    char buf[64];
    if (meters < 1000)
        snprintf(buf, sizeof(buf), "It is %u m from here: %s", (unsigned)lround(meters), planStops[0].name.c_str());
    else
        snprintf(buf, sizeof(buf), "It is %.1f km from here: %s", meters / 1000.0, planStops[0].name.c_str());
    openPicker(Setting::RouteJoin, "Navigate to starting point?", {"Yes, to the starting point", "No, join the route from here"}, {0, 1}, -1, buf,
               mode);
}

/**
 * Starts the search from where this device is now. What stands in the way is said on the screen.
 */
void KeyboardView_320x240::routeStart(void)
{
    roads.cancel();
    routeSearching = routeActive = routeArrived = false;
    routeList = false;
    routeNote.clear();
    routeOffSince = 0;
    routeOffFixes = 0;
    const NodeRecord *own = nodeStore.find(ownNode);
    if (!own || !own->position.hasCoordinates()) {
        routeNote = "No GPS fix yet: a route starts where this device is";
    } else if (!openRoads()) {
        roadsTried = 0; // the card may be put in before the next try
        routeNote = roads.state() == RouteGraph::State::OtherVersion ? "The road data is of another version"
                    : roads.state() == RouteGraph::State::NotRoadData ? "/geo/roads.bin is not road data"
                                                                       : "No road data: /geo/roads.bin on the card";
    } else {
        const RouteGraph::Progress progress =
            roads.begin(RouteGraph::Point{own->position.latitude, own->position.longitude},
                        RouteGraph::Point{routeLatitude, routeLongitude}, routeMode ? RouteGraph::Mode::Foot : RouteGraph::Mode::Car);
        if (progress == RouteGraph::Progress::Running || progress == RouteGraph::Progress::Done) {
            routeSearching = true;
            routeStarted = lv_tick_get();
        } else {
            routeNote = failureText(progress);
        }
    }
    dirty = urgent = true;
}

void KeyboardView_320x240::routeEnd(void)
{
    roads.cancel();
    routeSearching = routeActive = routeArrived = false;
    route = RouteGraph::Route();
    routeLine.clear();
    routeNote.clear();
    if (jinxmesh_track)
        jinxmesh_track(0);
    if (mapShown)
        dirty = true;
}

/**
 * The search in slices, and while there is a route the position from the receiver every two seconds.
 */
void KeyboardView_320x240::routeTask(void)
{
    if (planNext && lv_tick_elaps(planNext) >= c_nextStopTime) {
        planNext = 0;
        if (planRunning && planAt + 1 < planStops.size())
            planGo(planAt + 1, routeMode);
    }
    if (jinxmesh_inbox && (!inboxPolled || lv_tick_elaps(inboxPolled) >= c_inboxTime)) {
        inboxPolled = lv_tick_get() | 1;
        char line[160];
        for (int i = 0; i < 8 && jinxmesh_inbox(line, sizeof(line)) > 0; i++)
            routeCommand(line);
    }
    if (routeSearching) {
        const RouteGraph::Progress progress = roads.work(c_slice, lv_tick_get);
        if (progress == RouteGraph::Progress::Done) {
            routeSearching = false;
            if (roads.result(route)) {
                routeActive = true;
                routeStep = routeNearest = 0;
                routeAlong = routeOff = 0;
                routeFocus = routeTop = 0;
                ILOG_INFO("route: %u m, %u points, %u steps, %u ms", (unsigned)route.meters, (unsigned)route.points.size(),
                          (unsigned)route.steps.size(), (unsigned)lv_tick_elaps(routeStarted));
                const NodeRecord *own = nodeStore.find(ownNode);
                if (own && own->position.hasCoordinates())
                    routeFix(own->position.latitude, own->position.longitude);
            } else {
                routeNote = failureText(roads.progress());
            }
            dirty = urgent = true;
        } else if (progress != RouteGraph::Progress::Running) {
            routeSearching = false;
            routeNote = failureText(progress);
            dirty = urgent = true;
        } else if (app == App::Navigate && lv_tick_elaps(routeDrawn) >= 400) {
            dirty = true; // the progress
        }
    }
    // a route of several stops is followed on the map even while no way to its next stop is found
    if (!(routeActive || routeSearching || planRunning) || (routePolled && lv_tick_elaps(routePolled) < c_pollTime))
        return;
    routePolled = lv_tick_get() | 1;
    if (jinxmesh_track && !routeArrived)
        jinxmesh_track(c_trackSecs);
    int32_t latitude = 0, longitude = 0;
    uint32_t age = 0;
    const NodeRecord *own = nodeStore.find(ownNode);
    if (!jinxmesh_fix || !own || !jinxmesh_fix(&latitude, &longitude, &age) || age > c_freshFix || (!latitude && !longitude))
        return;
    if (own->position.hasCoordinates() && std::abs(latitude - own->position.latitude) < c_movedLat &&
        std::abs(longitude - own->position.longitude) < c_movedLat)
        return;
    // as if the radio had reported it: the map, the trip and the pointer page follow as well
    updatePosition(ownNode, latitude, longitude, own->position.altitude, own->position.satellites, 32);
}

/**
 * Where this device is on the route: how far along, how far off, which step it is on.
 */
void KeyboardView_320x240::routeFix(int32_t latitude, int32_t longitude)
{
    if (!routeActive || route.points.size() < 2)
        return;
    const double k = cos(latitude * 1e-7 * M_PI / 180.0);
    const size_t stretches = route.points.size() - 1;
    auto look = [&](size_t from, size_t to, double &best, size_t &at, double &along) {
        for (size_t i = from; i < to; i++) {
            const double ax = ((double)route.points[i].longitude - longitude) * k, ay = (double)route.points[i].latitude - latitude,
                         bx = ((double)route.points[i + 1].longitude - longitude) * k,
                         by = (double)route.points[i + 1].latitude - latitude;
            const double dx = bx - ax, dy = by - ay, norm = dx * dx + dy * dy;
            const double t = norm > 0 ? std::clamp(-(ax * dx + ay * dy) / norm, 0.0, 1.0) : 0.0;
            const double px = ax + t * dx, py = ay + t * dy;
            const double off = sqrt(px * px + py * py) * c_unit;
            if (off < best) {
                best = off;
                at = i;
                along = route.along[i] + t * ((double)route.along[i + 1] - route.along[i]);
            }
        }
    };
    double best = 1e18, along = 0;
    size_t at = 0;
    // around where it was last; all of the line when that is no longer near
    const size_t from = routeNearest > 5 ? routeNearest - 5 : 0;
    look(from, std::min(stretches, routeNearest + c_window), best, at, along);
    if (best > 150) {
        best = 1e18;
        look(0, stretches, best, at, along);
    }
    routeNearest = at;
    routeAlong = (uint32_t)lround(along);
    routeOff = (uint32_t)lround(best);
    routeStep = 0;
    while (routeStep + 1 < route.steps.size() && route.along[route.steps[routeStep + 1].point] <= routeAlong + 5)
        routeStep++;
    const uint32_t limit = routeMode ? c_offFoot : c_offCar;
    if (route.meters - std::min(routeAlong, route.meters) <= c_arriveMeters && routeOff <= limit * 2) {
        if (!routeArrived) {
            routeArrived = true;
            if (planRunning && planAt + 1 < planStops.size()) {
                toast(("Arrived: " + routeName + ". Next: " + planStops[planAt + 1].name).c_str(), c_nextStopTime);
                planNext = lv_tick_get() | 1;
            } else {
                toast(("Arrived: " + routeName).c_str(), 4000);
                if (jinxmesh_track)
                    jinxmesh_track(0);
            }
        }
    } else if (routeOff > limit) {
        routeArrived = false;
        if (!routeOffSince)
            routeOffSince = lv_tick_get() | 1;
        if (routeOffFixes < 255)
            routeOffFixes++;
        // not on the route for a while: a new one from here
        if (routeOffFixes >= 2 && lv_tick_elaps(routeOffSince) >= c_offTime && lv_tick_elaps(routeStarted) >= 15000) {
            ILOG_INFO("off the route by %u m: a new one", (unsigned)routeOff);
            const bool list = routeList;
            routeStart();
            routeList = list;
            if (routeSearching)
                toast("Off the route: finding a new one");
        }
    } else {
        routeArrived = false;
        routeOffSince = 0;
        routeOffFixes = 0;
    }
    if (app == App::Navigate || mapShown)
        dirty = true;
}

/**
 * A step in words: "Left onto Main Street".
 */
std::string KeyboardView_320x240::stepText(size_t index) const
{
    if (index >= route.steps.size())
        return "";
    const RouteGraph::Step &step = route.steps[index];
    const std::string way = step.name.empty() ? wayWord(step.kind) : step.name;
    switch (step.turn) {
    case RouteGraph::Start:
        return std::string("Head ") + geo::cardinal(step.bearing) + " on " + way;
    case RouteGraph::Continue:
        return "Continue on " + way;
    case RouteGraph::Arrive:
        return "Arrive: " + routeName;
    default:
        return std::string(RouteGraph::turnText(step.turn)) + " onto " + way;
    }
}

void KeyboardView_320x240::routeKey(uint32_t key)
{
#if defined(ARCH_PORTDUINO)
    // no receiver here: N moves this device 150 m along the route, M 200 m off it
    if (routeActive && (key == 'n' || key == 'm') && route.points.size() >= 2) {
        const uint32_t target = std::min(route.meters, routeAlong + 150);
        size_t i = 0;
        while (i + 2 < route.points.size() && route.along[i + 1] < target)
            i++;
        const double span = std::max<double>(1, (double)route.along[i + 1] - route.along[i]);
        const double t = std::clamp(((double)target - route.along[i]) / span, 0.0, 1.0);
        int32_t latitude = route.points[i].latitude + (int32_t)lround(t * (route.points[i + 1].latitude - route.points[i].latitude));
        const int32_t longitude =
            route.points[i].longitude + (int32_t)lround(t * (route.points[i + 1].longitude - route.points[i].longitude));
        if (key == 'm')
            latitude += 18000;
        updatePosition(ownNode, latitude, longitude, 240, 9, 32);
        return;
    }
#endif
#if defined(ARCH_PORTDUINO)
    // no Bluetooth here: I takes the lines of /apps/inbox.txt as if the page had sent them
    if (key == 'i' && ensureStorage() && storage->exists("/apps/inbox.txt")) {
        std::unique_ptr<IFile> file = storage->open("/apps/inbox.txt", OpenMode::Read);
        std::string text(file ? (size_t)std::min<uint64_t>(file->size(), c_planBytes) : 0, 0);
        if (file && !text.empty())
            file->read(&text[0], (uint32_t)text.size());
        for (size_t at = 0; at < text.size();) {
            const size_t end = std::min(text.find('\n', at), text.size());
            routeCommand(text.substr(at, end - at));
            at = end + 1;
        }
        return;
    }
#endif
    if (routeActive && routeList) {
        if (key == LV_KEY_UP && routeFocus > 0)
            routeFocus--;
        else if (key == LV_KEY_DOWN && routeFocus + 1 < route.steps.size())
            routeFocus++;
        else if (key == LV_KEY_LEFT)
            routeList = false;
        return;
    }
    if (key == LV_KEY_LEFT)
        closeApp();
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)
        routeActivate();
}

/**
 * OK: from the next turn to the list of turns and back; without a route, another try.
 */
void KeyboardView_320x240::routeActivate(void)
{
    if (routeActive) {
        routeList = !routeList;
        if (routeList)
            routeFocus = std::min(routeStep + 1, route.steps.size() - 1);
    } else if (!routeSearching && (routeLatitude || routeLongitude)) {
        routeStart();
    } else if (!routeSearching) {
        openRouteGo();
    }
}

void KeyboardView_320x240::routeHold(void)
{
    if (!routeActive && !routeSearching && !(routeLatitude || routeLongitude)) {
        openRouteGo();
        return;
    }
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    if (routeActive) {
        items.push_back("Show on the map");
        values.push_back(0);
    }
    items.push_back(routeNode && nodeStore.find(routeNode) ? "Route again, to where the node is now" : "Route again");
    values.push_back(1);
    items.push_back(routeMode ? "Switch to: by road, driving" : "Switch to: on foot, roads and trails");
    values.push_back(2);
    items.push_back("Straight line: the arrow");
    values.push_back(3);
    if (planRunning && planAt + 1 < planStops.size()) {
        items.push_back("Next stop: " + planStops[planAt + 1].name);
        values.push_back(5);
    }
    if (planRunning && planAt > 0) {
        items.push_back("Back to the stop before: " + planStops[planAt - 1].name);
        values.push_back(6);
    }
    items.push_back("Somewhere else");
    values.push_back(10);
    items.push_back("End navigation");
    values.push_back(4);
    openPicker(Setting::RouteMenu, ("To " + routeName).c_str(), std::move(items), std::move(values));
}

bool KeyboardView_320x240::routePick(Setting what, uint32_t value, uint32_t target)
{
    switch (what) {
    case Setting::RouteMenu:
        break;
    case Setting::RouteJoin: {
        size_t to = 0;
        const NodeRecord *own = nodeStore.find(ownNode);
        if (value == 1 && own && own->position.hasCoordinates() && planStops.size() >= 2) {
            // the stretch between two stops that is closest: its end is where the route goes on
            const double k = cos(own->position.latitude * 1e-7 * M_PI / 180.0);
            double best = 1e18;
            for (size_t i = 0; i + 1 < planStops.size(); i++) {
                const double ax = (planStops[i].longitude - own->position.longitude) * c_unit * k;
                const double ay = (planStops[i].latitude - own->position.latitude) * c_unit;
                const double bx = (planStops[i + 1].longitude - own->position.longitude) * c_unit * k;
                const double by = (planStops[i + 1].latitude - own->position.latitude) * c_unit;
                const double sx = bx - ax, sy = by - ay, length = sx * sx + sy * sy;
                const double t = length > 0 ? std::clamp(-(ax * sx + ay * sy) / length, 0.0, 1.0) : 0.0;
                const double px = ax + t * sx, py = ay + t * sy, d = px * px + py * py;
                if (d < best) {
                    best = d;
                    to = i + 1;
                }
            }
        }
        if (planStops.empty())
            return true;
        openApp(App::Navigate);
        planGo(to, target ? 1 : 0);
        return true;
    }
    case Setting::RouteGo:
        planAdding = false;
        if (value == 0)
            openEditor(Setting::RouteCoordinates, "Coordinates", "", 40, "44.2601, -72.5754", "Latitude, longitude: degrees with a point");
        else if (value == 1)
            routePick(Setting::RoutePlanMenu, 2, 1); // the list of waypoints, for a destination
        else if (value == 2)
            openPlans();
        else
            openPlan();
        return true;
    case Setting::RouteWaypoint:
        if (value >= waypoints.size())
            return true;
        routeChosen = Stop{waypoints[value].latitude, waypoints[value].longitude, waypoints[value].name};
        if (planAdding) {
            if (planStops.size() < c_maxStops)
                planStops.push_back(routeChosen);
            if (!planName.empty())
                planSave();
            openPlan();
        } else {
            openPicker(Setting::RouteMode, routeChosen.name.c_str(), {"By road, driving", "On foot, roads and trails"}, {0, 1}, -1,
                       coordinateText(routeChosen.latitude, routeChosen.longitude).c_str());
        }
        return true;
    case Setting::RouteMode:
        navigateTo(routeChosen.latitude, routeChosen.longitude, routeChosen.name, value ? 1 : 0, 0);
        return true;
    case Setting::RoutePlans:
        if (value == c_planNew) {
            planStops.clear();
            planName.clear();
            planRunning = false;
            openPlan();
        } else if (value < planFiles.size()) {
            if (planLoad(planFiles[value]))
                openPlan();
            else
                toast("That route could not be read");
        }
        return true;
    case Setting::RoutePlanMenu: {
        if (value >= c_planStopBase && value - c_planStopBase < planStops.size()) {
            const size_t i = value - c_planStopBase;
            openPicker(Setting::RoutePlanStop, planStops[i].name.c_str(), {"Start here: by road", "Start here: on foot", "Move up", "Move down", "Remove"},
                       {3, 4, 1, 2, 0}, -1, coordinateText(planStops[i].latitude, planStops[i].longitude).c_str(), (uint32_t)i);
            return true;
        }
        planAdding = target != 1;
        switch (value) {
        case 1:
            openEditor(Setting::RouteCoordinates, "Coordinates of the stop", "", 40, "44.2601, -72.5754",
                       "Latitude, longitude: degrees with a point");
            break;
        case 2: {
            if (waypoints.empty()) {
                toast("No waypoints: hold OK on the map, or send one from a phone");
                break;
            }
            std::vector<std::string> items;
            std::vector<uint32_t> values;
            for (size_t i = 0; i < waypoints.size(); i++) {
                items.push_back(waypoints[i].name);
                values.push_back((uint32_t)i);
            }
            openPicker(Setting::RouteWaypoint, planAdding ? "Add a waypoint" : "To a waypoint", std::move(items), std::move(values));
            break;
        }
        case 3: {
            const NodeRecord *own = nodeStore.find(ownNode);
            if (!own || !own->position.hasCoordinates()) {
                toast("No GPS fix yet");
                break;
            }
            routeChosen = Stop{own->position.latitude, own->position.longitude, ""};
            char name[16];
            snprintf(name, sizeof(name), "Stop %u", (unsigned)planStops.size() + 1);
            openEditor(Setting::RouteStopName, "Name the stop", name, 24, "Name");
            break;
        }
        case 4:
        case 5:
            if (planStops.empty()) {
                toast("No stops yet");
                break;
            }
            openApp(App::Navigate);
            planBegin(value == 5 ? 1 : 0);
            break;
        case 6:
            openEditor(Setting::RoutePlanName, "Name the route", planName, 28, "Name", "Kept on the card in /apps/routes");
            break;
        case 7:
            if (!planName.empty() && ensureStorage())
                storage->remove((std::string(c_planFolder) + "/" + planName + ".txt").c_str());
            toast("Route deleted");
            planName.clear();
            planStops.clear();
            planRunning = false;
            break;
        default:
            break;
        }
        return true;
    }
    case Setting::RoutePlanStop: {
        if (target >= planStops.size())
            return true;
        if (value == 3 || value == 4) {
            openApp(App::Navigate);
            planGo(target, value == 4 ? 1 : 0);
            return true;
        }
        if (value == 0)
            planStops.erase(planStops.begin() + target);
        else if (value == 1 && target > 0)
            std::swap(planStops[target], planStops[target - 1]);
        else if (value == 2 && target + 1 < planStops.size())
            std::swap(planStops[target], planStops[target + 1]);
        planRunning = false; // the order it was followed in is no more
        if (!planName.empty())
            planSave();
        openPlan();
        return true;
    }
    default:
        return false;
    }
    switch (value) {
    case 0: {
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates()) {
            showOnMap(ownNode);
            followOwn(); // the map goes along with this device
        } else {
            switchTab(Tab::Map);
        }
        break;
    }
    case 1:
    case 2: {
        if (value == 2)
            routeMode = routeMode ? 0 : 1;
        const NodeRecord *node = routeNode ? nodeStore.find(routeNode) : nullptr;
        if (node && node->position.hasCoordinates()) {
            routeLatitude = node->position.latitude;
            routeLongitude = node->position.longitude;
        }
        routeStart();
        break;
    }
    case 3:
        pointTo(routeLatitude * 1e-7, routeLongitude * 1e-7, routeName);
        break;
    case 5:
        planGo(planAt + 1, routeMode);
        break;
    case 6:
        if (planAt > 0)
            planGo(planAt - 1, routeMode);
        break;
    case 10:
        openRouteGo();
        break;
    case 4:
        routeEnd();
        planRunning = false;
        planNext = 0;
        routeLatitude = routeLongitude = 0;
        routeName.clear();
        toast("Navigation ended");
        break;
    default:
        break;
    }
    return true;
}

/**
 * Back: out of the list of turns, and out of a search. The route itself stays until it is ended.
 */
bool KeyboardView_320x240::routeBack(void)
{
    if (app != App::Navigate)
        return false;
    if (routeActive && routeList) {
        routeList = false;
        return true;
    }
    if (routeSearching) {
        roads.cancel();
        routeSearching = false;
        routeNote = "Search stopped";
        return true;
    }
    return false;
}

KeyboardView_320x240::TileView KeyboardView_320x240::routeTile(void) const
{
    TileView view;
    char buf[24];
    view.value = "--";
    view.sub = "No route";
    if (routeSearching) {
        view.value = "...";
        view.sub = "Finding a route";
    } else if (routeActive) {
        geo::distanceText(route.meters - std::min(routeAlong, route.meters), imperialUnits(), buf, sizeof(buf));
        view.value = routeArrived ? "Arrived" : buf;
        view.sub = routeName;
        view.accent = true;
    }
    return view;
}

void KeyboardView_320x240::renderRoute(void)
{
    char buf[64];
    routeDrawn = lv_tick_get();
    const bool imperial = imperialUnits();
    if (routeSearching) {
        label(content, 8, 40, c_width - 16, font16, c_text, ("Finding a route to " + routeName).c_str(), LV_TEXT_ALIGN_CENTER);
        // the bar says how close the search has come to the destination, which is not how long it still takes
        const int32_t width = 200, done = width * roads.percent() / 100;
        box(content, (c_width - width) / 2, 96, width, 8, c_raised);
        box(content, (c_width - width) / 2, 96, std::max<int32_t>(done, 2), 8, c_blue);
        snprintf(buf, sizeof(buf), "%s · %u junctions looked at", routeMode ? "On foot" : "By car", (unsigned)roads.settled());
        label(content, 8, 114, c_width - 16, font12, c_muted, buf, LV_TEXT_ALIGN_CENTER);
        label(content, 8, 190, c_width - 16, font12, c_muted, "Back: stop", LV_TEXT_ALIGN_CENTER);
        return;
    }
    if (!routeActive) {
        const bool again = routeLatitude || routeLongitude;
        label(content, 8, 60, c_width - 16, font16, c_text, routeNote.empty() ? "No route" : routeNote.c_str(), LV_TEXT_ALIGN_CENTER);
        wrapped(content, 20, 110, c_width - 40, c_muted,
                again ? "OK: try again   Hold OK: other ways there"
                      : "OK: where to? Coordinates, a waypoint, or a route of several stops. A node: hold OK on it, then "
                        "\"Navigate to last reported position\"");
        return;
    }
    if (routeList) {
        // every turn with the length of the stretch behind it; the one that comes up is marked
        const size_t count = route.steps.size();
        routeFocus = std::min(routeFocus, count - 1);
        routeTop = scrollTop(routeTop, routeFocus, count, c_listRows);
        for (size_t i = routeTop; i < count && i < routeTop + c_listRows; i++) {
            buf[0] = 0;
            if (route.steps[i].turn != RouteGraph::Arrive)
                geo::distanceText(route.steps[i].meters, imperial, buf, sizeof(buf));
            const std::string text = (i == routeStep + 1 ? "> " : "") + stepText(i);
            settingRow(content, 1 + (int32_t)(i - routeTop) * c_settingHeight, i == routeFocus, text.c_str(), buf, false);
        }
        listScrollbar(content, 1, routeTop, count, c_listRows, c_settingHeight);
        return;
    }

    const size_t next = std::min(routeStep + 1, route.steps.size() - 1);
    const RouteGraph::Step &step = route.steps[next];
    const uint32_t reached = route.along[step.point];
    const uint32_t toTurn = reached > routeAlong ? reached - routeAlong : 0;
    const uint32_t left = route.meters - std::min(routeAlong, route.meters);

    std::string heading = "To " + routeName;
    if (planRunning && planStops.size() > 1) {
        // the count first: what is cut off when there is no room is the end of the name
        snprintf(buf, sizeof(buf), "Stop %u of %u: ", (unsigned)planAt + 1, (unsigned)planStops.size());
        heading = buf + routeName;
    }
    lv_obj_t *title = label(content, 8, 4, c_width - 90, font14, c_muted, heading.c_str());
    lv_obj_set_height(title, lv_font_get_line_height(font14));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    label(content, c_width - 84, 5, 76, font12, c_muted, routeMode ? "On foot" : "By car", LV_TEXT_ALIGN_RIGHT);

    // the arrow of the turn that comes up: a stem from below, bent to where the way goes on
    const int32_t boxLeft = 8, boxTop = 28, boxSize = 108;
    lv_obj_t *frame = box(content, boxLeft, boxTop, boxSize, boxSize, c_surface);
    lv_obj_set_style_radius(frame, 8, LV_PART_MAIN);
    const double centerX = boxLeft + boxSize / 2.0, centerY = boxTop + boxSize / 2.0 + 6;
    if (routeArrived || step.turn == RouteGraph::Arrive) {
        lv_obj_t *ring = plain(content, (int32_t)centerX - 22, (int32_t)centerY - 28, 44, 44);
        outline(ring, c_focus, 5, 22);
        lv_obj_set_style_radius(box(content, (int32_t)centerX - 7, (int32_t)centerY - 13, 14, 14, c_focus), 7, LV_PART_MAIN);
    } else {
        const double rad = turnAngle(step.turn) * M_PI / 180.0;
        const double dx = sin(rad), dy = -cos(rad), reach = 34, barb = 16, spread = 0.55;
        size_t n = 0;
        auto put = [&](double x, double y) {
            turnPoints[n].x = (lv_value_precise_t)lround(x);
            turnPoints[n].y = (lv_value_precise_t)lround(y);
            n++;
        };
        const double tipX = centerX + dx * reach, tipY = centerY + dy * reach;
        put(centerX, boxTop + boxSize - 12);
        put(centerX, centerY);
        put(tipX, tipY);
        for (int side = 0; side < 2; side++) {
            const double turn = M_PI + (side ? spread : -spread), c = cos(turn), s = sin(turn);
            put(tipX + (dx * c - dy * s) * barb, tipY + (dx * s + dy * c) * barb);
            put(tipX, tipY);
        }
        lv_obj_t *arrow = lv_line_create(content);
        lv_obj_remove_style_all(arrow);
        lv_line_set_points(arrow, turnPoints, (uint32_t)n);
        lv_obj_set_style_line_width(arrow, 7, LV_PART_MAIN);
        lv_obj_set_style_line_color(arrow, lv_color_hex(c_focus), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(arrow, true, LV_PART_MAIN);
    }

    const int32_t textLeft = boxLeft + boxSize + 12, textWidth = c_width - textLeft - 6;
    if (routeArrived) {
        label(content, textLeft, 34, textWidth, font20, c_text, "Arrived");
        wrapped(content, textLeft, 64, textWidth, c_text, routeName.c_str());
    } else {
        geo::distanceText(toTurn, imperial, buf, sizeof(buf));
        label(content, textLeft, 30, textWidth, font20, c_text, buf);
        lv_obj_t *what = label(content, textLeft, 58, textWidth, font16, c_text, stepText(next).c_str());
        lv_obj_set_height(what, 2 * lv_font_get_line_height(font16));
        if (next + 1 < route.steps.size()) {
            lv_obj_t *then = label(content, textLeft, 104, textWidth, font12, c_muted, ("Then: " + stepText(next + 1)).c_str());
            lv_obj_set_height(then, 2 * lv_font_get_line_height(font12));
        }
    }

    // what is left, and how the route and this device get on
    geo::distanceText(left, imperial, buf, sizeof(buf));
    const uint32_t seconds = route.meters ? (uint32_t)((uint64_t)route.seconds * left / route.meters) : 0;
    label(content, 8, 146, c_width - 16, font16, c_text, (std::string(buf) + " to go · about " + timeText(seconds)).c_str());
    const uint32_t limit = routeMode ? c_offFoot : c_offCar;
    std::string note;
    if (routeOff > limit) {
        geo::distanceText(routeOff, imperial, buf, sizeof(buf));
        note = std::string(buf) + " off the route";
    } else if (route.endOff > 50) {
        geo::distanceText(route.endOff, imperial, buf, sizeof(buf));
        note = std::string("The destination is ") + buf + " from the last " + (routeMode ? "trail or road" : "road");
    }
    if (!note.empty())
        label(content, 8, 170, c_width - 16, font12, routeOff > limit ? c_focus : c_muted, note.c_str());
    label(content, 8, 196, c_width - 16, font12, c_muted, "OK: all turns   Hold OK: map, route again, end");
}

/**
 * Where to: the ways to name a place that is no node.
 */
void KeyboardView_320x240::openRouteGo(void)
{
    std::vector<std::string> items{"Coordinates"};
    std::vector<uint32_t> values{0};
    if (!waypoints.empty()) {
        items.push_back("A waypoint");
        values.push_back(1);
    }
    items.push_back("A saved route");
    values.push_back(2);
    items.push_back(planStops.empty() ? "Plan a route of several stops" : "The route being planned");
    values.push_back(3);
    openPicker(Setting::RouteGo, "Navigate to", std::move(items), std::move(values), -1, "A node: hold OK on it in the list");
}

/**
 * The route of several stops that is being put together or was loaded: its stops, and what can be done with it.
 */
void KeyboardView_320x240::openPlan(void)
{
    std::vector<std::string> items;
    std::vector<uint32_t> values;
    char buf[48];
    for (size_t i = 0; i < planStops.size(); i++) {
        snprintf(buf, sizeof(buf), "%u. ", (unsigned)i + 1);
        items.push_back(buf + planStops[i].name);
        values.push_back(c_planStopBase + (uint32_t)i);
    }
    if (!planStops.empty()) {
        items.push_back("Start: by road, driving");
        values.push_back(4);
        items.push_back("Start: on foot, roads and trails");
        values.push_back(5);
    }
    if (planStops.size() < c_maxStops) {
        items.push_back("Add a stop: coordinates");
        values.push_back(1);
        if (!waypoints.empty()) {
            items.push_back("Add a stop: a waypoint");
            values.push_back(2);
        }
        items.push_back("Add a stop: where I am");
        values.push_back(3);
    }
    if (!planStops.empty()) {
        items.push_back(planName.empty() ? "Save" : "Save under another name");
        values.push_back(6);
    }
    if (!planName.empty()) {
        items.push_back("Delete this route");
        values.push_back(7);
    }
    if (planStops.size() > c_listRows) {
        // a long route: what can be done with it comes first, or it would lie behind all of its stops
        std::rotate(items.begin(), items.begin() + planStops.size(), items.end());
        std::rotate(values.begin(), values.begin() + planStops.size(), values.end());
    }
    snprintf(buf, sizeof(buf), "%u stop%s%s", (unsigned)planStops.size(), planStops.size() == 1 ? "" : "s",
             planName.empty() && !planStops.empty() ? ", not saved" : "");
    openPicker(Setting::RoutePlanMenu, planName.empty() ? "New route" : planName.c_str(), std::move(items), std::move(values), -1, buf);
}

/**
 * The routes kept on the card.
 */
void KeyboardView_320x240::openPlans(void)
{
    planFiles.clear();
    if (ensureStorage())
        storage->list(c_planFolder, [this](const char *name, bool isDir) {
            const std::string file = name;
            if (!isDir && file.size() > 4 && file.compare(file.size() - 4, 4, ".txt") == 0 && planFiles.size() < c_maxPlans)
                planFiles.push_back(file.substr(0, file.size() - 4));
        });
    std::sort(planFiles.begin(), planFiles.end());
    std::vector<std::string> items = planFiles;
    std::vector<uint32_t> values;
    for (size_t i = 0; i < planFiles.size(); i++)
        values.push_back((uint32_t)i);
    items.push_back("New route");
    values.push_back(c_planNew);
    openPicker(Setting::RoutePlans, "Saved routes", std::move(items), std::move(values), -1,
               planFiles.empty() ? "None yet: plan one here, or send one from the page" : "");
}

/**
 * A line per stop: latitude,longitude,name. So the page and a text editor can write one as well.
 */
bool KeyboardView_320x240::planSave(void)
{
    if (planName.empty() || !ensureStorage())
        return false;
    storage->mkdir("/apps");
    storage->mkdir(c_planFolder);
    const std::string path = std::string(c_planFolder) + "/" + planName + ".txt";
    std::unique_ptr<IFile> file = storage->open(path.c_str(), OpenMode::Write);
    if (!file)
        return false;
    bool good = true;
    for (const Stop &stop : planStops) {
        char line[96];
        const int n = snprintf(line, sizeof(line), "%.6f,%.6f,%s\n", stop.latitude * 1e-7, stop.longitude * 1e-7, stop.name.c_str());
        good = good && file->write(line, (uint32_t)n) == n;
    }
    return good;
}

bool KeyboardView_320x240::planLoad(const std::string &name)
{
    if (!ensureStorage())
        return false;
    std::unique_ptr<IFile> file = storage->open((std::string(c_planFolder) + "/" + name + ".txt").c_str(), OpenMode::Read);
    if (!file)
        return false;
    std::string text((size_t)std::min<uint64_t>(file->size(), c_planBytes), 0);
    if (!text.empty() && file->read(&text[0], (uint32_t)text.size()) != (int32_t)text.size())
        return false;
    std::vector<Stop> stops;
    for (size_t at = 0; at < text.size() && stops.size() < c_maxStops;) {
        const size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = trimmed(text.substr(at, end - at));
        at = end + 1;
        const size_t first = line.find(','), second = first == std::string::npos ? first : line.find(',', first + 1);
        Stop stop{};
        if (line.empty() || line[0] == '#' || first == std::string::npos ||
            !parseCoordinates(line.substr(0, second), stop.latitude, stop.longitude))
            continue;
        stop.name = second == std::string::npos ? "" : trimmed(line.substr(second + 1)).substr(0, 24);
        if (stop.name.empty())
            stop.name = "Stop " + std::to_string(stops.size() + 1);
        stops.push_back(stop);
    }
    planStops = std::move(stops);
    planName = name;
    planRunning = false;
    return true;
}

bool KeyboardView_320x240::routeEdit(Setting what, const std::string &text, uint32_t)
{
    switch (what) {
    case Setting::RouteCoordinates: {
        Stop stop{};
        if (!parseCoordinates(text, stop.latitude, stop.longitude)) {
            toast("Not coordinates. Like: 44.2601, -72.5754", 3000);
            openEditor(Setting::RouteCoordinates, planAdding ? "Coordinates of the stop" : "Coordinates", text, 40, "44.2601, -72.5754",
                       "Latitude, longitude: degrees with a point");
            return true;
        }
        stop.name = coordinateText(stop.latitude, stop.longitude);
        routeChosen = stop;
        if (planAdding) {
            char name[16];
            snprintf(name, sizeof(name), "Stop %u", (unsigned)planStops.size() + 1);
            openEditor(Setting::RouteStopName, "Name the stop", name, 24, "Name", stop.name.c_str());
        } else {
            openPicker(Setting::RouteMode, stop.name.c_str(), {"By road, driving", "On foot, roads and trails"}, {0, 1});
        }
        return true;
    }
    case Setting::RouteStopName: {
        std::string name = trimmed(text);
        std::replace(name.begin(), name.end(), ',', ' ');
        routeChosen.name = name.empty() ? "Stop " + std::to_string(planStops.size() + 1) : name;
        if (planStops.size() < c_maxStops)
            planStops.push_back(routeChosen);
        if (!planName.empty())
            planSave();
        openPlan();
        return true;
    }
    case Setting::RoutePlanName: {
        const std::string name = fileName(text);
        if (name.empty()) {
            toast("A name of letters and digits");
        } else {
            planName = name;
            toast(planSave() ? "Route saved" : "It could not be saved: is the card in?");
        }
        openPlan();
        return true;
    }
    default:
        return false;
    }
}

/**
 * What the page sends, a line at a time (the firmware hands the lines over as they came):
 *   D lat,lon,c|f,name     a destination: the route there starts, by car or on foot
 *   R name                 a route of several stops begins
 *   S lat,lon,name         a stop of it
 *   G c|f                  it is saved and started
 *   K                      it is saved only
 */
void KeyboardView_320x240::routeCommand(const std::string &raw)
{
    const std::string line = trimmed(raw);
    if (line.empty())
        return;
    const std::string rest = trimmed(line.substr(1));
    auto place = [&](Stop &stop, std::string &after) {
        const size_t first = rest.find(','), second = first == std::string::npos ? first : rest.find(',', first + 1);
        if (first == std::string::npos || !parseCoordinates(rest.substr(0, second), stop.latitude, stop.longitude))
            return false;
        after = second == std::string::npos ? "" : trimmed(rest.substr(second + 1));
        return true;
    };
    Stop stop{};
    std::string after;
    switch (line[0]) {
    case 'D': {
        if (!place(stop, after)) {
            toast("The page sent a destination that is none");
            break;
        }
        uint8_t mode = 0;
        if (after.size() >= 1 && (after.size() == 1 || after[1] == ',')) {
            mode = after[0] == 'f' ? 1 : 0;
            after = after.size() > 2 ? trimmed(after.substr(2)) : "";
        }
        stop.name = after.empty() ? coordinateText(stop.latitude, stop.longitude) : after.substr(0, 24);
        ILOG_INFO("route: a destination from the page");
        closeMenu();
        navigateTo(stop.latitude, stop.longitude, stop.name, mode, 0);
        toast(("From the page: to " + stop.name).c_str(), 3000);
        break;
    }
    case 'R':
        planStops.clear();
        planRunning = false;
        planName = fileName(rest);
        break;
    case 'S':
        if (place(stop, after) && planStops.size() < c_maxStops) {
            std::replace(after.begin(), after.end(), ',', ' ');
            stop.name = after.empty() ? "Stop " + std::to_string(planStops.size() + 1) : after.substr(0, 24);
            planStops.push_back(stop);
        } else {
            toast("The page sent a stop that is none: left out", 3000);
        }
        break;
    case 'G':
    case 'K': {
        if (planStops.empty()) {
            toast("The page sent a route without stops");
            break;
        }
        const bool saved = planName.empty() || planSave();
        ILOG_INFO("route: %u stops from the page", (unsigned)planStops.size());
        char buf[64];
        snprintf(buf, sizeof(buf), "From the page: a route of %u stops%s", (unsigned)planStops.size(), saved ? "" : ", not saved");
        if (line[0] == 'G') {
            closeMenu();
            openApp(App::Navigate);
            planBegin(rest == "f" ? 1 : 0);
        }
        toast(buf, 3000);
        break;
    }
    default:
        break;
    }
}

/**
 * For the map: what comes up, in a line.
 */
std::string KeyboardView_320x240::guideText(void) const
{
    if (routeSearching)
        return "Finding a route to " + routeName;
    if (!routeActive || route.steps.empty())
        return "";
    if (routeArrived)
        return "Arrived: " + routeName;
    const size_t next = std::min(routeStep + 1, route.steps.size() - 1);
    const uint32_t reached = route.along[route.steps[next].point];
    char buf[24];
    geo::distanceText(reached > routeAlong ? reached - routeAlong : 0, imperialUnits(), buf, sizeof(buf));
    return std::string(buf) + " · " + stepText(next);
}

#endif
