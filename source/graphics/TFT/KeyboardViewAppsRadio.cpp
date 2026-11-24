#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/common/ViewController.h"
#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kbview;

/**
 * RF monitor: how busy the channel is, how much this radio transmits, what it counts and whom it hears.
 * The numbers are the radio's own (its LocalStats and device metrics); the view only keeps a short history.
 */

namespace
{

constexpr uint32_t c_sampleInterval = 20000; // ms between two points of the history
constexpr uint32_t c_askInterval = 10000;    // ms between requests for the counters while the screen is open
constexpr int32_t c_gaugeHeight = 54;
constexpr int32_t c_gaugeWidth = 152;
constexpr int32_t c_barPitch = 3;
constexpr int32_t c_barsHeight = 30;

uint8_t halfPercent(float percent)
{
    return (uint8_t)std::clamp<long>(lround(percent * 2.0f), 0, 200);
}

std::string uptimeText(uint32_t seconds)
{
    char buf[24];
    if (seconds >= 86400)
        snprintf(buf, sizeof(buf), "%u d %u h", (unsigned)(seconds / 86400), (unsigned)(seconds % 86400 / 3600));
    else if (seconds >= 3600)
        snprintf(buf, sizeof(buf), "%u h %u min", (unsigned)(seconds / 3600), (unsigned)(seconds % 3600 / 60));
    else
        snprintf(buf, sizeof(buf), "%u min", (unsigned)(seconds / 60));
    return buf;
}

/**
 * One of the two gauges at the top: the value now and a bar for every sample of the last ten minutes.
 * The bars share one scale that is at least 10 percent high, so a quiet channel does not look busy.
 */
void gauge(lv_obj_t *parent, int32_t x, const char *name, bool known, float percent, const uint8_t *history, size_t count,
           size_t room)
{
    lv_obj_t *panel = box(parent, x, 3, c_gaugeWidth, c_gaugeHeight, c_surface);
    lv_obj_set_style_radius(panel, 5, LV_PART_MAIN);
    label(panel, 6, 4, 60, font12, c_muted, name);
    char buf[16] = "--";
    if (known)
        snprintf(buf, sizeof(buf), percent < 10 ? "%.1f%%" : "%.0f%%", percent);
    label(panel, 6, 22, 60, font20, c_text, buf);

    uint8_t peak = 20; // half percent
    for (size_t i = 0; i < count; i++)
        peak = std::max(peak, history[i]);
    const int32_t left = c_gaugeWidth - 4 - (int32_t)room * c_barPitch;
    for (size_t i = 0; i < count; i++) {
        const int32_t height = std::max<int32_t>(1, history[i] * c_barsHeight / peak);
        box(panel, left + (int32_t)(room - count + i) * c_barPitch, 8 + c_barsHeight - height, c_barPitch - 1, height,
            history[i] >= 50 ? c_focus : c_blue);
    }
    snprintf(buf, sizeof(buf), "10 min, to %u%%", (unsigned)((peak + 1) / 2));
    label(panel, left - 24, 8 + c_barsHeight + 1, (int32_t)room * c_barPitch + 24, font12, c_muted, buf, LV_TEXT_ALIGN_RIGHT);
}

} // namespace

void KeyboardView_320x240::updateLocalStats(uint32_t nodeNum, const meshtastic_LocalStats &stats)
{
    if ((ownNode && nodeNum != ownNode) || demoData)
        return; // the counters of another radio, or of the simulated one behind the sample mesh
    localStats = stats;
    hasLocalStats = true;
    channelUse = stats.channel_utilization;
    txUse = stats.air_util_tx;
    hasAirtime = true;
    dirty = true;
}

/**
 * A point of the ten-minute history. Called with what the radio reports, at most every 20 seconds.
 */
void KeyboardView_320x240::radioSample(float use, float tx)
{
    if (historyCount == c_radioHistory) {
        std::move(useHistory.begin() + 1, useHistory.end(), useHistory.begin());
        std::move(txHistory.begin() + 1, txHistory.end(), txHistory.begin());
        historyCount--;
    }
    useHistory[historyCount] = halfPercent(use);
    txHistory[historyCount] = halfPercent(tx);
    historyCount++;
}

/**
 * Keeps the history going on every screen, and the counters fresh while the RF monitor is on show.
 */
void KeyboardView_320x240::radioTask(void)
{
    if (hasAirtime && (historyCount == 0 || lv_tick_elaps(historyTime) >= c_sampleInterval)) {
        historyTime = lv_tick_get();
        radioSample(channelUse, txUse);
        if (inApp() && app == App::Radio)
            dirty = true;
    }
    if (inApp() && app == App::Radio && !screenOff && ownNode && lv_tick_elaps(statsAsked) >= c_askInterval) {
        statsAsked = lv_tick_get();
        controller->requestLocalStats();
    }
}

/**
 * The rows below the gauges: the counters, then who was heard without a relay, newest first.
 */
std::vector<KeyboardView_320x240::SettingRow> KeyboardView_320x240::radioRows(void) const
{
    std::vector<SettingRow> rows;
    char buf[48];
    auto count = [&rows](const char *name, uint32_t value) {
        rows.push_back({Setting::None, name, std::to_string(value)});
    };
    if (!hasLocalStats) {
        rows.push_back({Setting::None, "Counters", "Waiting for the radio"});
    } else {
        if (localStats.noise_floor != 0) {
            snprintf(buf, sizeof(buf), "%d dBm", (int)localStats.noise_floor);
            rows.push_back({Setting::None, "Noise floor", buf});
        }
        count("Packets sent", localStats.num_packets_tx);
        count("Packets received", localStats.num_packets_rx);
        count("Received bad", localStats.num_packets_rx_bad);
        count("Duplicates heard", localStats.num_rx_dupe);
        count("Relayed", localStats.num_tx_relay);
        count("Relays canceled", localStats.num_tx_relay_canceled);
        count("Dropped", localStats.num_tx_dropped);
        snprintf(buf, sizeof(buf), "%u of %u", (unsigned)localStats.num_online_nodes, (unsigned)localStats.num_total_nodes);
        rows.push_back({Setting::None, "Nodes online", buf});
        rows.push_back({Setting::None, "Uptime", uptimeText(localStats.uptime_seconds)});
        snprintf(buf, sizeof(buf), "%u of %u kB", (unsigned)(localStats.heap_free_bytes / 1024),
                 (unsigned)(localStats.heap_total_bytes / 1024));
        rows.push_back({Setting::None, "Free memory", buf});
    }
    rows.push_back({Setting::None, "Heard directly", heard.empty() ? "Nobody yet" : "newest first"});
    for (auto it = heard.rbegin(); it != heard.rend(); ++it) {
        char time[12] = "";
        if (validTime(it->time))
            clockString(it->time, time, sizeof(time), twelveHour(), false);
        snprintf(buf, sizeof(buf), "%s%s%d dBm · %.1f dB", time, time[0] ? " · " : "", (int)it->rssi, it->snr);
        rows.push_back({Setting::None, chatName(it->node), buf, it->node});
    }
    return rows;
}

void KeyboardView_320x240::radioKey(uint32_t key)
{
    const size_t count = radioRows().size();
    if (key == LV_KEY_UP) {
        if (radioFocus > 0)
            radioFocus--;
    } else if (key == LV_KEY_DOWN) {
        if (radioFocus + 1 < count)
            radioFocus++;
    } else if (key == LV_KEY_LEFT) {
        closeApp();
    } else if (key == LV_KEY_RIGHT) {
        radioActivate();
    }
}

/**
 * OK: on somebody who was heard, what can be done with that node; on a counter, fresh numbers.
 */
void KeyboardView_320x240::radioActivate(void)
{
    const std::vector<SettingRow> rows = radioRows();
    if (radioFocus < rows.size() && rows[radioFocus].arg && nodeStore.find(rows[radioFocus].arg)) {
        openNodeMenu(rows[radioFocus].arg);
        return;
    }
    if (!ownNode)
        return;
    statsAsked = lv_tick_get();
    controller->requestLocalStats();
    toast("Asked the radio for its counters");
}

void KeyboardView_320x240::renderRadio(void)
{
    gauge(content, 5, "Channel", hasAirtime, channelUse, useHistory.data(), historyCount, c_radioHistory);
    gauge(content, 5 + c_gaugeWidth + 6, "Own TX", hasAirtime, txUse, txHistory.data(), historyCount, c_radioHistory);

    const std::vector<SettingRow> rows = radioRows();
    const int32_t top = c_gaugeHeight + 6;
    const size_t shown = (lv_obj_get_height(content) - top - 2) / c_settingHeight;
    radioFocus = std::min(radioFocus, rows.size() - 1);
    radioTop = scrollTop(radioTop, radioFocus, rows.size(), shown);
    for (size_t i = radioTop; i < rows.size() && i < radioTop + shown; i++)
        settingRow(content, top + (int32_t)(i - radioTop) * c_settingHeight, i == radioFocus, rows[i].title.c_str(),
                   rows[i].value.c_str(), false);
    listScrollbar(content, top, radioTop, rows.size(), shown, c_settingHeight);
}

/**
 * The Airtime tile.
 */
KeyboardView_320x240::TileView KeyboardView_320x240::airtimeTile(void) const
{
    TileView view;
    view.value = "--";
    view.sub = "No report from the radio yet";
    if (hasAirtime) {
        char buf[24];
        snprintf(buf, sizeof(buf), channelUse < 10 ? "%.1f%%" : "%.0f%%", channelUse);
        view.value = buf;
        snprintf(buf, sizeof(buf), "Own airtime %.1f%%", txUse);
        view.sub = buf;
    }
    return view;
}

#endif
