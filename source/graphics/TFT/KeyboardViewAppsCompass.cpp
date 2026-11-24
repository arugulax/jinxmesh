#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "util/Declination.h"
#include "util/GeoMath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace kbview;

/**
 * Compass: which way the device points, from the magnetometer the firmware reads. The firmware hands the
 * heading over through two functions; where they are missing (the simulator, a board without a compass) the
 * screens say that there is no compass and the pointer page keeps north at the top.
 */

extern "C" {
// Defined by the firmware (src/motion/JinxCompass.cpp). 0: no compass, 1: *heading is degrees from magnetic
// north, 2: being calibrated, 3: waiting for a reading.
int jinxmesh_compass(float *heading) __attribute__((weak));
void jinxmesh_compass_calibrate(unsigned seconds) __attribute__((weak));
}

namespace
{

constexpr uint32_t c_pollInterval = 150; // ms between two looks at the sensor while a screen shows the heading
constexpr float c_redrawStep = 2.0f;     // degrees the heading must turn before the screen is drawn again
constexpr float c_smoothing = 0.35f;     // share of a new reading in the smoothed heading
constexpr unsigned c_calibrateSecs = 30;
constexpr int32_t c_roseSize = 150;
constexpr int32_t c_roseLeft = 10;
constexpr int32_t c_roseTop = 34;

float wrap(float degrees)
{
    degrees = fmodf(degrees, 360.0f);
    return degrees < 0 ? degrees + 360.0f : degrees;
}

} // namespace

/**
 * While a screen shows the heading the sensor is looked at several times a second; otherwise never, and the
 * firmware lets the sensor rest.
 */
void KeyboardView_320x240::compassTask(void)
{
    const bool shown = inApp() && (app == App::Compass || app == App::Pointer) && !screenOff && menu == Menu::None;
    if (!shown || lv_tick_elaps(compassPolled) < c_pollInterval)
        return;
    compassPolled = lv_tick_get();
    float raw = 0;
    uint8_t state = 0;
#if defined(ARCH_PORTDUINO)
    if (demoData) {
        // the sample mesh comes with a hand that turns the device slowly, so that the screens can be looked at
        state = 1;
        raw = fmodf(lv_tick_get() / 100.0f, 360.0f);
    }
#endif
    if (!state && jinxmesh_compass)
        state = (uint8_t)jinxmesh_compass(&raw);
    if (state == 1) {
        const float rad = raw * (float)M_PI / 180.0f;
        if (compassState != 1) {
            compassNorth = cosf(rad);
            compassEast = sinf(rad);
        } else {
            compassNorth += (cosf(rad) - compassNorth) * c_smoothing;
            compassEast += (sinf(rad) - compassEast) * c_smoothing;
        }
    }
    float heading = 0;
    const bool changed = state != compassState;
    compassState = state;
    if (changed || (compassHeading(heading) && fabsf(remainderf(heading - compassShown, 360.0f)) >= c_redrawStep))
        dirty = true;
}

/**
 * The heading to show: true when a declination is set, else magnetic. False without a reading.
 */
bool KeyboardView_320x240::compassHeading(float &degrees) const
{
    if (compassState != 1)
        return false;
    float declination = 0;
    declinationNow(declination);
    degrees = wrap(atan2f(compassEast, compassNorth) * 180.0f / (float)M_PI + prefs.compassTurn + declination);
    return true;
}

/**
 * The declination in use: from the magnetic model for where this device is, or the one that was typed in.
 * False when there is none and headings are magnetic.
 */
bool KeyboardView_320x240::declinationNow(float &degrees) const
{
    if (prefs.declinationAuto) {
        // looked up again only after a move of some kilometers: it changes by a degree over a hundred
        static int32_t latitude = 0, longitude = 0;
        static float value = 0;
        static bool known = false;
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates()) {
            if (!known || abs(own->position.latitude - latitude) > 500000 || abs(own->position.longitude - longitude) > 500000) {
                latitude = own->position.latitude;
                longitude = own->position.longitude;
                value = geo::declination(latitude * 1e-7, longitude * 1e-7);
                known = true;
            }
        }
        if (known) {
            degrees = value;
            return true;
        }
    }
    if (prefs.declinationSet) {
        degrees = prefs.declination;
        return true;
    }
    return false;
}

void KeyboardView_320x240::compassKey(uint32_t key)
{
    if (key == LV_KEY_LEFT)
        closeApp();
    else if (key == LV_KEY_RIGHT)
        compassHold();
}

/**
 * OK: what can be set about the compass.
 */
void KeyboardView_320x240::compassHold(void)
{
    char buf[40];
    std::vector<std::string> items = {"Calibrate (30 seconds)"};
    float declination = 0;
    if (declinationNow(declination))
        snprintf(buf, sizeof(buf), "Declination: %.0f° %s%s", fabsf(declination), declination < 0 ? "west" : "east",
                 prefs.declinationAuto ? " (auto)" : "");
    else
        snprintf(buf, sizeof(buf), prefs.declinationAuto ? "Declination: waiting for GPS" : "Declination: not set");
    items.push_back(buf);
    snprintf(buf, sizeof(buf), "Turn the reading: %u°", (unsigned)prefs.compassTurn);
    items.push_back(buf);
    openPicker(Setting::CompassMenu, "Compass", std::move(items), {0, 1, 2}, -1, "Without a declination headings are magnetic");
}

bool KeyboardView_320x240::compassPick(Setting what, uint32_t value, uint32_t)
{
    if (what == Setting::CompassMenu) {
        if (value == 0) {
            if (compassState == 0 || !jinxmesh_compass_calibrate) {
                toast("No compass in this device");
            } else {
                jinxmesh_compass_calibrate(c_calibrateSecs);
                toast("Turn the device every way for 30 seconds", 5000);
            }
        } else if (value == 1) {
            openPicker(Setting::DeclinationAuto, "Declination", {"From the GPS position", "Type it in"}, {1, 0},
                       prefs.declinationAuto ? 0 : 1, "What is typed in is used until there is a position");
        } else {
            openPicker(Setting::CompassTurn, "Turn the reading by", {"0°", "90°", "180°", "270°"}, {0, 90, 180, 270},
                       (int32_t)(prefs.compassTurn / 90), "If north shows up where east, south or west is");
        }
        return true;
    }
    if (what == Setting::DeclinationAuto) {
        prefs.declinationAuto = value != 0;
        savePrefs();
        if (!prefs.declinationAuto)
            openEditor(Setting::Declination, "Declination in degrees", prefs.declinationSet ? std::to_string(prefs.declination) : "", 4,
                       "West is negative: -14", "Empty: headings stay magnetic");
        return true;
    }
    if (what == Setting::CompassTurn) {
        prefs.compassTurn = (uint16_t)(value % 360);
        savePrefs();
        return true;
    }
    return false;
}

bool KeyboardView_320x240::compassEdit(Setting what, const std::string &text, uint32_t)
{
    if (what != Setting::Declination)
        return false;
    char *end = nullptr;
    const long degrees = strtol(text.c_str(), &end, 10);
    prefs.declinationSet = end != text.c_str() && degrees >= -180 && degrees <= 180;
    prefs.declination = prefs.declinationSet ? (int16_t)degrees : 0;
    if (!prefs.declinationSet && !text.empty())
        toast("A number from -180 to 180");
    savePrefs();
    return true;
}

void KeyboardView_320x240::renderCompass(void)
{
    char buf[48];
    const int32_t centerX = c_roseLeft + c_roseSize / 2, centerY = c_roseTop + c_roseSize / 2;
    const int32_t left = c_roseLeft + c_roseSize + 14, width = c_width - left - 6;
    float heading = 0;
    const bool known = compassHeading(heading);
    compassShown = known ? heading : -1000;

    lv_obj_t *rose = plain(content, c_roseLeft, c_roseTop, c_roseSize, c_roseSize);
    outline(rose, c_border, 2, c_roseSize / 2);
    lv_obj_set_style_bg_color(rose, lv_color_hex(c_surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rose, LV_OPA_COVER, LV_PART_MAIN);
    // the mark at the top is the device; the letters go around it
    lv_obj_set_style_radius(box(content, centerX - 2, c_roseTop - 8, 4, 16, c_focus), 2, LV_PART_MAIN);

    if (!known) {
        const char *why = compassState == 2 ? "Calibrating: keep turning" : compassState == 3 ? "Waiting for the compass" : "No compass";
        label(content, c_roseLeft, centerY - 8, c_roseSize, font12, c_focus, why, LV_TEXT_ALIGN_CENTER);
        label(content, left, 50, width, font20, c_text, "--");
        if (compassState == 0)
            label(content, left, 80, width, font12, c_muted, "None found in this device");
        label(content, 8, 196, c_width - 16, font12, c_focusMuted, "OK: calibrate and settings", LV_TEXT_ALIGN_CENTER);
        return;
    }

    static const char *const letters[] = {"N", "E", "S", "W"};
    const double reach = c_roseSize / 2 - 14;
    for (int i = 0; i < 4; i++) {
        const double rad = (i * 90.0 - heading) * M_PI / 180.0;
        label(content, centerX + (int32_t)lround(sin(rad) * reach) - 10, centerY - (int32_t)lround(cos(rad) * reach) - 9, 20,
              i ? font14 : font16, i ? c_muted : c_blue, letters[i], LV_TEXT_ALIGN_CENTER);
    }
    // ticks every 30 degrees between the letters
    for (int i = 1; i < 12; i++) {
        if (i % 3 == 0)
            continue;
        const double rad = (i * 30.0 - heading) * M_PI / 180.0;
        lv_obj_set_style_radius(box(content, centerX + (int32_t)lround(sin(rad) * reach) - 2, centerY - (int32_t)lround(cos(rad) * reach) - 2,
                                    4, 4, c_border),
                                2, LV_PART_MAIN);
    }

    // what the pointer page was last set on: a line from the middle towards it
    const NodeRecord *own = nodeStore.find(ownNode);
    const NodeRecord *target = pointerPlace ? nullptr : nodeStore.find(pointerNode);
    std::string towards;
    if (own && own->position.hasCoordinates() && (pointerPlace || (target && target->position.hasCoordinates()))) {
        const geo::Leg leg = geo::leg(own->position.latitude, own->position.longitude, pointerPlace ? placeLatitude : target->position.latitude,
                                      pointerPlace ? placeLongitude : target->position.longitude);
        const double rad = (leg.bearing - heading) * M_PI / 180.0;
        compassPoints[0].x = (lv_value_precise_t)centerX;
        compassPoints[0].y = (lv_value_precise_t)centerY;
        compassPoints[1].x = (lv_value_precise_t)lround(centerX + sin(rad) * (reach - 16));
        compassPoints[1].y = (lv_value_precise_t)lround(centerY - cos(rad) * (reach - 16));
        lv_obj_t *line = lv_line_create(content);
        lv_obj_remove_style_all(line);
        lv_line_set_points(line, compassPoints, 2);
        lv_obj_set_style_line_width(line, 4, LV_PART_MAIN);
        lv_obj_set_style_line_color(line, lv_color_hex(c_focus), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
        geo::distanceText(leg.meters, imperialUnits(), buf, sizeof(buf));
        towards = (pointerPlace ? placeName : chatName(target->id)) + " " + buf;
    }

    snprintf(buf, sizeof(buf), "%d°", (int)lround(heading) % 360);
    label(content, left, 44, width, font20, c_text, buf);
    label(content, left, 72, width, font16, c_text, geo::cardinal(heading));
    float declination = 0;
    const bool trueNorth = declinationNow(declination);
    label(content, left, 98, width, font12, trueNorth ? c_muted : c_focus, trueNorth ? "True north" : "Magnetic north");
    label(content, left, 115, width, font12, c_muted, "Hold it flat");
    if (!towards.empty()) {
        label(content, left, 140, width, font12, c_focus, "Line: to");
        label(content, left, 156, width, font12, c_focusMuted, towards.c_str());
    }
    label(content, 8, 196, c_width - 16, font12, c_focusMuted, "OK: calibrate and settings", LV_TEXT_ALIGN_CENTER);
}

#endif
