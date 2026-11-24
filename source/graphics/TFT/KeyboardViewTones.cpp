#include "graphics/common/Ringtones.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "storage/IStorage.h"
#include "util/ILog.h"
#include <algorithm>
#include <atomic>
#include <cstring>

/**
 * A tune per kind of notification and per channel. The radio (the firmware) is what sounds the buzzer for a message, with
 * its one ringtone; it asks jinxmesh_tone() here whether this kind of message has another.
 *
 * Tunes are the ones of Ringtones.h and the user's own: files /tones/<name>.rtttl on the card, each one
 * tune as RTTTL text ("name:d=4,o=5,b=120:c,e,g"). The flasher page has an editor that writes them.
 *
 * The radio's thread reads the tunes at any time, so the chosen ones are copied into buffers that stay.
 */
namespace
{
constexpr size_t c_tuneBytes = 400;
constexpr size_t c_maxOwn = 40;
constexpr const char *c_folder = "/tones";
constexpr const char *c_ending = ".rtttl";

char directText[c_tuneBytes], emergencyText[c_tuneBytes], playText[c_tuneBytes];
std::atomic<const char *> directTune{nullptr}, emergencyTune{nullptr};
char channelText[c_max_channels][c_tuneBytes];
std::atomic<const char *> channelTune[c_max_channels];

const char *builtIn(const std::string &name)
{
    for (int i = 0; i < numRingtones; i++) {
        if (name == ringtone[i].name)
            return ringtone[i].rtttl;
    }
    return nullptr;
}

// into a buffer that stays; null when there is no such tune
const char *kept(char *buffer, const std::string &text)
{
    if (text.empty())
        return nullptr;
    snprintf(buffer, c_tuneBytes, "%s", text.c_str());
    return buffer;
}
} // namespace

extern "C" {
// of the firmware; stays null where it has none (the simulator, other firmware)
int jinxmesh_play(const char *rtttl) __attribute__((weak));

// an emergency message has its tune first, then a direct message, then the channel it came on
const char *jinxmesh_tone(int direct, int channel, const char *text, unsigned length)
{
    if (text && length >= 5 && memcmp(text, "MECP/", 5) == 0) {
        if (const char *tune = emergencyTune.load())
            return tune;
    }
    if (direct)
        return directTune.load();
    return channel >= 0 && channel < c_max_channels ? channelTune[channel].load() : nullptr;
}
}

/**
 * The names of the tunes in /tones, sorted.
 */
std::vector<std::string> KeyboardView_320x240::ownTones(void)
{
    std::vector<std::string> names;
    if (!ensureStorage())
        return names;
    const size_t ending = strlen(c_ending);
    storage->list(c_folder, [&names, ending](const char *name, bool isDir) {
        const size_t length = strlen(name);
        if (!isDir && length > ending && strcasecmp(name + length - ending, c_ending) == 0 && names.size() < c_maxOwn)
            names.emplace_back(name, length - ending);
    });
    std::sort(names.begin(), names.end());
    return names;
}

/**
 * A tune by its name as RTTTL text: one of Ringtones.h, or else the user's own. Empty when there is none.
 */
std::string KeyboardView_320x240::tuneText(const std::string &name)
{
    if (name.empty())
        return "";
    if (const char *tune = builtIn(name))
        return tune;
    if (name.find('/') != std::string::npos || !ensureStorage())
        return "";
    auto file = storage->open((std::string(c_folder) + "/" + name + c_ending).c_str(), OpenMode::Read);
    if (!file)
        return "";
    char buf[c_tuneBytes];
    const int32_t got = file->read(buf, sizeof(buf) - 1);
    std::string text(buf, got > 0 ? (size_t)got : 0);
    // editors leave line ends and blanks; the player wants none
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return (uint8_t)c <= ' '; }), text.end());
    // name, defaults, notes
    return std::count(text.begin(), text.end(), ':') == 2 ? text : "";
}

void KeyboardView_320x240::applyTones(void)
{
    // off first: the radio must not read a buffer while it is written
    directTune = nullptr;
    emergencyTune = nullptr;
    for (uint8_t i = 0; i < c_max_channels; i++) {
        channelTune[i] = nullptr;
        channelTune[i] = kept(channelText[i], tuneText(prefs.toneChannel[i]));
    }
    directTune = kept(directText, tuneText(prefs.toneDirect));
    emergencyTune = kept(emergencyText, tuneText(prefs.toneEmergency));
}

bool KeyboardView_320x240::playTone(const std::string &name)
{
    const char *tune = kept(playText, tuneText(name));
    const bool sounds = tune && jinxmesh_play && jinxmesh_play(tune);
    ILOG_INFO("tune %s: %s", name.c_str(), sounds ? "plays" : !tune ? "no such tune" : jinxmesh_play ? "no sound" : "no player");
    return sounds;
}

/**
 * The sound of a warning: the tune of emergencies when one is picked, three short notes and a long one
 * otherwise. None while the device is silent.
 */
void KeyboardView_320x240::playAlarm(void)
{
    if (silent() || !jinxmesh_play)
        return;
    std::string text = tuneText(prefs.toneEmergency);
    if (text.empty())
        text = "warn:d=8,o=6,b=200:c,p,c,p,c,p,2g";
    if (const char *tune = kept(playText, text))
        jinxmesh_play(tune);
}

std::string &KeyboardView_320x240::toneOf(Setting which)
{
    return which == Setting::ToneDirect ? prefs.toneDirect : which == Setting::ToneEmergency ? prefs.toneEmergency : prefs.toneTimer;
}

/**
 * The lists of tunes carry numbers: the tunes of Ringtones.h first, then the user's own as the list found
 * them. The three lists of the kinds of notification have "same" (the timer: "beeps") in front, as 0.
 */
std::string KeyboardView_320x240::toneNameAt(Setting list, uint32_t value) const
{
    if (list != Setting::AlertTone) {
        if (value == 0)
            return "";
        value--;
    }
    if (value < (uint32_t)numRingtones)
        return ringtone[value].name;
    value -= numRingtones;
    return value < pickTones.size() ? pickTones[value] : "";
}
