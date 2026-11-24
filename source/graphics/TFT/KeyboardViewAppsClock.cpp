#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/driver/DisplayDriver.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

using namespace kbview;

/**
 * Stopwatch with splits and a timer. Both count with the processor's running clock, which a correction of the
 * time of day cannot bend, and both go on behind every other screen, a dark screen and the lock.
 */

namespace
{

constexpr size_t c_maxSplits = 99;
constexpr uint32_t c_drawInterval = 200;   // ms between two drawings of a running clock
constexpr uint32_t c_awakeBefore = 180000; // ms before zero from which the radio is kept from sleeping
constexpr uint32_t c_beepInterval = 400;   // ms between two beeps
constexpr uint8_t c_beepCount = 6;
constexpr uint32_t c_ringTime = 120000;    // ms the notice stays (and keeps the radio awake) when nobody presses a key
constexpr uint32_t c_maxTimer = 99 * 3600 + 59 * 60 + 59;
constexpr size_t c_splitRows = 4;
const uint32_t c_presets[] = {60, 180, 300, 600, 900, 1800, 3600};

#if defined(ARDUINO_ARCH_ESP32) && defined(ELECROW_ThinkNode_M9)
constexpr uint8_t c_buzzerPin = 9; // PIN_BUZZER of the M9; the firmware sounds it the same way
#endif

void beep(void)
{
#if defined(ARDUINO_ARCH_ESP32) && defined(ELECROW_ThinkNode_M9)
    tone(c_buzzerPin, 2600, 160);
#endif
}

// "1:02:03.4", "2:03.4"
std::string stopwatchText(uint32_t ms, bool tenths = true)
{
    char buf[24];
    const uint32_t seconds = ms / 1000;
    int length;
    if (seconds >= 3600)
        length = snprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)(seconds / 3600), (unsigned)(seconds % 3600 / 60),
                          (unsigned)(seconds % 60));
    else
        length = snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    if (tenths)
        snprintf(buf + length, sizeof(buf) - length, ".%u", (unsigned)(ms % 1000 / 100));
    return buf;
}

std::string lengthText(uint32_t seconds)
{
    return stopwatchText(seconds * 1000, false);
}

/**
 * Digits as they are typed for a length, read from the right as seconds, minutes, hours: "500" is 5:00.
 */
uint32_t typedLength(const std::string &digits)
{
    const unsigned long value = strtoul(digits.c_str(), nullptr, 10);
    return (uint32_t)std::min<unsigned long>(c_maxTimer, value / 10000 * 3600 + value / 100 % 100 * 60 + value % 100);
}

} // namespace

/**
 * A short sound for the screen going on (two notes up) or off (one low note).
 */
void KeyboardView_320x240::screenSound(bool on)
{
#if defined(ARDUINO_ARCH_ESP32) && defined(ELECROW_ThinkNode_M9)
    tone(c_buzzerPin, on ? 2400 : 1500, on ? 50 : 70);
#else
    (void)on;
#endif
}

/**
 * The stopwatch or the timer on a tile of Home.
 */
KeyboardView_320x240::TileView KeyboardView_320x240::clockTile(bool timer) const
{
    TileView view;
    if (timer) {
        const int32_t left = timerLeft();
        view.value = lengthText(timerRunning || timerPaused ? (uint32_t)std::max<int32_t>(left, 0) / 1000 : prefs.timerLength);
        view.sub = timerRinging ? "Time is up" : timerRunning ? "Running" : timerPaused ? "Paused" : "OK: open";
        view.accent = timerRinging || timerRunning;
    } else {
        const uint32_t ms = stopwatchNow();
        view.value = stopwatchText(ms, false);
        view.sub = stopwatchRunning ? "Running" : ms ? "Stopped" : "OK: open";
        view.accent = stopwatchRunning;
    }
    return view;
}

// -------- stopwatch --------

uint32_t KeyboardView_320x240::stopwatchNow(void) const
{
    return stopwatchBase + (stopwatchRunning ? lv_tick_elaps(stopwatchStart) : 0);
}

/**
 * A split: the time since the last one, the total, and where and when that was.
 */
void KeyboardView_320x240::stopwatchSplit(void)
{
    if (!stopwatchRunning)
        return;
    if (splits.size() >= c_maxSplits) {
        toast("99 splits are the most");
        return;
    }
    const uint32_t total = stopwatchNow();
    Split split{};
    split.total = total;
    split.lap = total - (splits.empty() ? 0 : splits.back().total);
    split.clock = validTime(now()) ? now() : 0;
    const NodeRecord *own = nodeStore.find(ownNode);
    split.placed = own && own->position.hasCoordinates();
    if (split.placed) {
        split.latitude = own->position.latitude;
        split.longitude = own->position.longitude;
    }
    split.tripMeters = tripMeters;
    splits.push_back(split);
    splitsTop = 0;
}

/**
 * The splits as a file on the card: /apps/stopwatch/<yyyy-mm-dd>-<hhmm>.csv.
 */
bool KeyboardView_320x240::stopwatchSave(void)
{
    if (splits.empty()) {
        toast("No splits to save");
        return false;
    }
    if (!ensureStorage()) {
        toast("No SD card");
        return false;
    }
    char buf[96];
    const time_t t = validTime(now()) ? now() : 0;
    struct tm tm;
    localtime_r(&t, &tm);
    storage->mkdir("/apps");
    storage->mkdir("/apps/stopwatch");
    if (t)
        snprintf(buf, sizeof(buf), "/apps/stopwatch/%04d-%02d-%02d-%02d%02d.csv", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
    else
        snprintf(buf, sizeof(buf), "/apps/stopwatch/no-date-%u.csv", (unsigned)(lv_tick_get() / 1000));
    const std::string path = buf;
    std::unique_ptr<IFile> file = storage->open(path.c_str(), OpenMode::Write);
    if (!file) {
        toast("Cannot write to the card");
        return false;
    }
    std::string out = "split,lap_s,total_s,clock,lat,lon,trip_m\n";
    for (size_t i = 0; i < splits.size(); i++) {
        const Split &split = splits[i];
        snprintf(buf, sizeof(buf), "%u,%.1f,%.1f,", (unsigned)(i + 1), split.lap / 1000.0, split.total / 1000.0);
        out += buf;
        if (split.clock) {
            const time_t at = split.clock;
            struct tm when;
            localtime_r(&at, &when);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &when);
            out += buf;
        }
        if (split.placed)
            snprintf(buf, sizeof(buf), ",%.6f,%.6f,%.0f\n", split.latitude * 1e-7, split.longitude * 1e-7, split.tripMeters);
        else
            snprintf(buf, sizeof(buf), ",,,%.0f\n", split.tripMeters);
        out += buf;
    }
    if (file->write(out.data(), (uint32_t)out.size()) != (int32_t)out.size()) {
        toast("Cannot write to the card");
        return false;
    }
    toast(("Saved as " + path.substr(path.rfind('/') + 1)).c_str(), 4000);
    return true;
}

// -------- timer --------

/**
 * Milliseconds until zero; negative once it is over.
 */
int32_t KeyboardView_320x240::timerLeft(void) const
{
    if (timerRunning)
        return (int32_t)(timerEnd - lv_tick_get());
    return (int32_t)(timerPaused ? timerPaused : prefs.timerLength * 1000);
}

void KeyboardView_320x240::timerStart(void)
{
    timerEnd = lv_tick_get() + (uint32_t)timerLeft();
    timerPaused = 0;
    timerRunning = true;
    timerRinging = false;
}

/**
 * Keeps running clocks on the screen, and rings the timer.
 */
extern "C" {
// of the firmware: a file that comes in over the cable right now (jinx/JinxFiles.cpp); 0 when there is none
int jinxmesh_transfer(char *name, unsigned nameSize, uint32_t *done, uint32_t *total) __attribute__((weak));
}

void KeyboardView_320x240::clockTask(void)
{
    if (jinxmesh_transfer && lv_tick_elaps(transferShown) >= 1000) {
        transferShown = lv_tick_get();
        char name[40];
        uint32_t done = 0, total = 0;
        if (jinxmesh_transfer(name, sizeof(name), &done, &total)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Receiving %s  %u%%", name, total ? (unsigned)((uint64_t)done * 100 / total) : 0);
            toast(buf, 1600);
        }
    }
    if (tonesStale && lv_tick_get() > 5000) {
        // not at the start: a tune of the user's own is read from the card
        tonesStale = false;
        applyTones();
    }

    // close to zero the radio must not put the processor to sleep, or the timer rings late
    const bool near = timerRunning && timerLeft() < (int32_t)c_awakeBefore;
    mustSendHeartbeat = uploading || near || timerRinging;

    if (timerRunning && timerLeft() <= 0) {
        timerRounds++;
        timerRinging = true;
        timerRingStart = lv_tick_get();
        // its tune when it has one and there is sound, the beeps otherwise
        timerBeeps = !silent() && playTone(prefs.toneTimer) ? 0 : c_beepCount;
        timerRang = lv_tick_get() - c_beepInterval;
        if (prefs.timerRepeat) {
            timerEnd += prefs.timerLength * 1000; // from when it should have ended, so that rounds do not drift
            if (timerLeft() <= 0)
                timerEnd = lv_tick_get() + prefs.timerLength * 1000;
        } else {
            timerRunning = false;
            timerPaused = 0;
        }
        // on the screen, whatever was on it; a locked screen keeps its lock and gets the notice on top
        if (screenOff)
            displaydriver->forceWakeup();
        if (!locked && !bluetoothMode && !restarting)
            openApp(App::Timer);
        else
            toast("Timer: time is up", 8000); // where the notice cannot show
        if (prefs.keyFlash && !pulseLeft) {
            pulseLeft = 8;
            pulseTime = lv_tick_get();
        }
        dirty = urgent = true;
    }
    // Nobody there, or the notice could not show (the lock, a restart): it ends by itself, or the radio would be
    // kept awake until somebody finds the Timer again.
    if (timerRinging && lv_tick_elaps(timerRingStart) >= c_ringTime) {
        timerRinging = false;
        dirty = true;
    }
    if (timerBeeps && lv_tick_elaps(timerRang) >= c_beepInterval) {
        timerRang = lv_tick_get();
        timerBeeps--;
        if (!silent())
            beep();
    }

    const bool shown = inApp() && !screenOff && menu == Menu::None &&
                       ((app == App::Stopwatch && stopwatchRunning) || (app == App::Timer && timerRunning));
    if (shown && lv_tick_elaps(clockDrawn) >= c_drawInterval)
        dirty = urgent = true;
}

// -------- keys --------

void KeyboardView_320x240::clockKey(uint32_t key)
{
    if (app == App::Timer && timerRinging) {
        timerRinging = false; // any key takes the notice off
        timerBeeps = 0;
        return;
    }
    if (key == LV_KEY_LEFT) {
        closeApp();
        return;
    }
    if (app == App::Stopwatch) {
        if (key == ' ' || key == LV_KEY_RIGHT) {
            if (stopwatchRunning)
                stopwatchSplit();
            else
                clockActivate();
        } else if (key == LV_KEY_UP && splitsTop > 0) {
            splitsTop--;
        } else if (key == LV_KEY_DOWN && splitsTop + c_splitRows < splits.size()) {
            splitsTop++;
        }
        return;
    }
    // the timer: digits set a new length while it stands
    if (timerRunning) {
        if (key == LV_KEY_RIGHT)
            clockActivate();
        return;
    }
    if (key >= '0' && key <= '9' && timerEntry.size() < 6) {
        timerEntry.push_back((char)key);
    } else if (key == LV_KEY_BACKSPACE && !timerEntry.empty()) {
        timerEntry.pop_back();
    } else if (key == LV_KEY_RIGHT) {
        clockActivate();
    }
}

/**
 * OK: start and stop.
 */
void KeyboardView_320x240::clockActivate(void)
{
    if (app == App::Stopwatch) {
        if (stopwatchRunning) {
            stopwatchBase = stopwatchNow();
            stopwatchRunning = false;
        } else {
            stopwatchStart = lv_tick_get();
            stopwatchRunning = true;
        }
        return;
    }
    if (timerRinging) {
        timerRinging = false;
        timerBeeps = 0;
        return;
    }
    if (timerRunning) {
        timerPaused = (uint32_t)std::max<int32_t>(1, timerLeft());
        timerRunning = false;
        return;
    }
    if (!timerEntry.empty()) {
        // what was typed becomes the length
        const uint32_t length = typedLength(timerEntry);
        timerEntry.clear();
        if (!length) {
            toast("Type a length, such as 500 for 5:00");
            return;
        }
        prefs.timerLength = length;
        timerPaused = 0;
        savePrefs();
    }
    if (!timerPaused)
        timerRounds = 0;
    timerStart();
}

void KeyboardView_320x240::clockHold(void)
{
    if (app == App::Stopwatch) {
        openPicker(Setting::StopwatchMenu, "Stopwatch", {"Reset", "Save the splits to the card", "Clear the splits"}, {0, 1, 2});
        return;
    }
    openPicker(Setting::TimerMenu, "Timer", {"Reset", prefs.timerRepeat ? "Repeat: on" : "Repeat: off", "Change the length"},
               {0, 1, 2});
}

bool KeyboardView_320x240::clockPick(Setting what, uint32_t value, uint32_t)
{
    switch (what) {
    case Setting::StopwatchMenu:
        if (value == 0) {
            stopwatchRunning = false;
            stopwatchBase = 0;
            splits.clear();
            splitsTop = 0;
        } else if (value == 1) {
            stopwatchSave();
        } else {
            splits.clear();
            splitsTop = 0;
        }
        return true;
    case Setting::TimerMenu:
        if (value == 0) {
            timerRunning = timerRinging = false;
            timerPaused = 0;
            timerRounds = 0;
            timerEntry.clear();
        } else if (value == 1) {
            prefs.timerRepeat = !prefs.timerRepeat;
            savePrefs();
        } else {
            std::vector<std::string> items;
            std::vector<uint32_t> values;
            int32_t current = -1;
            bool listed = false;
            for (const uint32_t preset : c_presets) {
                if (preset == prefs.timerLength) {
                    current = (int32_t)items.size();
                    listed = true;
                }
                items.push_back(lengthText(preset));
                values.push_back(preset);
            }
            if (!listed) {
                current = (int32_t)items.size();
                items.push_back(lengthText(prefs.timerLength) + "  (last used)");
                values.push_back(prefs.timerLength);
            }
            openPicker(Setting::TimerLength, "Length of the timer", std::move(items), std::move(values), current,
                       "Or type it on the timer: 500 is 5:00");
        }
        return true;
    case Setting::TimerLength:
        prefs.timerLength = std::clamp<uint32_t>(value, 1, c_maxTimer);
        timerRunning = false;
        timerPaused = 0;
        timerEntry.clear();
        savePrefs();
        return true;
    default:
        return false;
    }
}

// -------- screens --------

void KeyboardView_320x240::renderStopwatch(void)
{
    clockDrawn = lv_tick_get();
    const uint32_t total = stopwatchNow();
    const uint32_t lap = total - (splits.empty() ? 0 : splits.back().total);
    label(content, 8, 4, 120, font12, stopwatchRunning ? c_focus : c_muted, stopwatchRunning ? "Running" : total ? "Stopped" : "Ready");
    label(content, 8, 20, 190, font20, c_text, stopwatchText(total).c_str());
    if (!splits.empty() || stopwatchRunning) {
        label(content, 200, 8, c_width - 208, font12, c_muted, ("Lap " + std::to_string(splits.size() + 1)).c_str(), LV_TEXT_ALIGN_RIGHT);
        label(content, 200, 24, c_width - 208, font16, c_text, stopwatchText(lap).c_str(), LV_TEXT_ALIGN_RIGHT);
    }

    // the splits, newest first; with three or more the fastest and the slowest lap are marked
    size_t fastest = SIZE_MAX, slowest = SIZE_MAX;
    if (splits.size() >= 3) {
        fastest = slowest = 0;
        for (size_t i = 1; i < splits.size(); i++) {
            if (splits[i].lap < splits[fastest].lap)
                fastest = i;
            if (splits[i].lap > splits[slowest].lap)
                slowest = i;
        }
    }
    const int32_t top = 52;
    splitsTop = std::min(splitsTop, splits.size() > c_splitRows ? splits.size() - c_splitRows : 0);
    for (size_t row = 0; row < c_splitRows && row + splitsTop < splits.size(); row++) {
        const size_t index = splits.size() - 1 - splitsTop - row;
        const Split &split = splits[index];
        std::string title = "Split " + std::to_string(index + 1) + "   " + stopwatchText(split.lap);
        if (index == fastest)
            title += "  fastest";
        else if (index == slowest)
            title += "  slowest";
        settingRow(content, top + (int32_t)row * c_settingHeight, false, title.c_str(), stopwatchText(split.total).c_str(),
                   index == fastest);
    }
    if (splits.empty())
        label(content, 8, top + 40, c_width - 16, font14, c_muted, "Space or Right takes a split", LV_TEXT_ALIGN_CENTER);
    listScrollbar(content, top, splitsTop, splits.size(), c_splitRows, c_settingHeight);
    label(content, 8, 196, c_width - 16, font12, c_focusMuted,
          stopwatchRunning ? "OK: stop   Space: split   Hold OK: more" : "OK: start   Hold OK: reset, save",
          LV_TEXT_ALIGN_CENTER);
}

void KeyboardView_320x240::renderTimer(void)
{
    clockDrawn = lv_tick_get();
    char buf[48];
    if (timerRinging) {
        // the notice: nothing else on the screen until a key is pressed
        lv_obj_t *notice = box(content, 6, 6, c_width - 12, lv_obj_get_height(content) - 12, c_focusTint);
        outline(notice, c_focus, 2, 8);
        label(notice, 0, 50, c_width - 16, font20, c_focus, "Time is up", LV_TEXT_ALIGN_CENTER);
        snprintf(buf, sizeof(buf), "%s timer", lengthText(prefs.timerLength).c_str());
        label(notice, 0, 84, c_width - 16, font16, c_text, buf, LV_TEXT_ALIGN_CENTER);
        if (prefs.timerRepeat) {
            snprintf(buf, sizeof(buf), "Round %u runs: %s left", (unsigned)(timerRounds + 1),
                     stopwatchText((uint32_t)std::max<int32_t>(0, timerLeft()), false).c_str());
            label(notice, 0, 112, c_width - 16, font14, c_muted, buf, LV_TEXT_ALIGN_CENTER);
        }
        label(notice, 0, 160, c_width - 16, font12, c_focusMuted, "Any key", LV_TEXT_ALIGN_CENTER);
        return;
    }
    const int32_t left = std::max<int32_t>(0, timerLeft());
    const bool typing = !timerRunning && !timerEntry.empty();
    const char *state = timerRunning ? "Running" : typing ? "New length" : timerPaused ? "Paused" : "Ready";
    label(content, 8, 6, 150, font12, timerRunning || typing ? c_focus : c_muted, state);
    // rounded up, so that it shows 0:00 only when it is over
    const std::string shown = typing ? lengthText(typedLength(timerEntry)) : stopwatchText((uint32_t)left + 999, false);
    label(content, 8, 40, c_width - 16, font20, typing ? c_focus : c_text, shown.c_str(), LV_TEXT_ALIGN_CENTER);

    // how much of the length is gone
    const uint32_t length = std::max<uint32_t>(1, prefs.timerLength) * 1000;
    const int32_t barWidth = c_width - 40;
    lv_obj_set_style_radius(box(content, 20, 78, barWidth, 8, c_raised), 4, LV_PART_MAIN);
    if (!typing && (timerRunning || timerPaused)) {
        const int32_t done = (int32_t)((uint64_t)(length - std::min<uint32_t>(length, (uint32_t)left)) * barWidth / length);
        if (done > 0)
            lv_obj_set_style_radius(box(content, 20, 78, done, 8, c_focus), 4, LV_PART_MAIN);
    }
    snprintf(buf, sizeof(buf), "Length %s", lengthText(prefs.timerLength).c_str());
    settingRow(content, 100, false, buf, prefs.timerRepeat ? "Repeats" : "Once", false);
    if (timerRounds) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)timerRounds);
        settingRow(content, 100 + c_settingHeight, false, "Rounds done", buf, false);
    }
    label(content, 8, 172, c_width - 16, font12, c_muted, silent() ? "Sound is off: the notice and the light only" : "",
          LV_TEXT_ALIGN_CENTER);
    label(content, 8, 196, c_width - 16, font12, c_focusMuted,
          timerRunning ? "OK: pause   Hold OK: more" : typing ? "OK: start   Del: correct" : "OK: start   Digits: new length   Hold OK: more",
          LV_TEXT_ALIGN_CENTER);
}

#endif
