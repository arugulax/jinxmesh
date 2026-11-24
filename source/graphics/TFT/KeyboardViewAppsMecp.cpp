#if HAS_TFT && defined(USE_KEYBOARD_VIEW)

#include "graphics/view/TFT/KeyboardViewKit.h"
#include "graphics/view/TFT/KeyboardView_320x240.h"
#include "util/GeoMath.h"
#include "util/MecpCodec.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

using namespace kbview;

/**
 * MECP: emergency messages that every mesh client can carry and every language can read (util/MecpCodec.h).
 * Here they are shown as cards in plain words wherever a chat shows them, listed in Apps, and written in four
 * steps without anybody having to know a code.
 */

namespace
{

constexpr uint32_t c_severityColors[] = {0xE5484D, 0xF76B15, 0xFFC53D, 0x6CB6FF}; // Mayday, Urgent, Safety, Routine
const char *const c_severityWords[] = {"Mayday: life in danger", "Urgent", "Safety", "Routine"};
constexpr int32_t c_headHeight = 24; // the line above the rows of a composer step
constexpr size_t c_stepRows = 7;     // rows of a composer step on screen
constexpr int32_t c_listRowHeight = 38;
constexpr size_t c_listRows = 5;
constexpr size_t c_searchMax = 16;

// what people type when they look for a phrase and do not know its words
const struct {
    const char *code;
    const char *words;
} c_synonyms[] = {
    {"M01", "hurt wound injured"},      {"M02", "passed out fainted coma"},   {"M03", "choking asthma air"},
    {"M04", "heart attack chest"},      {"M05", "cold freezing frozen"},      {"M06", "blood"},
    {"M07", "broken leg arm bone"},     {"M08", "burn scald"},                {"M09", "many hurt"},
    {"M10", "dead body died"},          {"M11", "snake dog bee tick"},        {"M12", "allergy epipen"},
    {"M13", "poison overdose"},         {"M14", "found alive survivors"},     {"M16", "medicine drop"},
    {"T01", "road closed tree down"},   {"T02", "bridge closed washed"},      {"T04", "flood high water"},
    {"T05", "mudslide rockslide"},      {"T06", "outage electricity blackout"}, {"T07", "burning smoke wildfire"},
    {"T09", "trail blocked"},           {"T10", "refuge"},                    {"T11", "drown river lake"},
    {"T16", "crash car wreck"},         {"W01", "thunder wind rain snow"},    {"W02", "fog whiteout"},
    {"W03", "freezing"},                {"W04", "hot"},                       {"W05", "smoke"},
    {"S01", "thirsty drink"},           {"S02", "hungry"},                    {"S03", "medicine meds pills insulin"},
    {"S04", "charge charger"},          {"S05", "gas petrol diesel"},         {"P01", "stuck trapped"},
    {"P02", "leaving"},                 {"P03", "staying"},                   {"P04", "on my way going"},
    {"P05", "here position"},           {"P06", "where am i"},                {"P07", "missing split"},
    {"C01", "help sos"},                {"C02", "ride car pickup"},           {"C03", "pass on forward"},
    {"C04", "copy"},                    {"C08", "meet"},                      {"R01", "ok ack roger copy yes"},
    {"R02", "on my way"},               {"R03", "arrive minutes time"},       {"R04", "no sorry"},
    {"R06", "wait hold"},               {"R07", "ok safe fine over"},         {"D04", "mistake oops"},
    {"L20", "check radio"},             {"X03", "shots shooting"},            {"X07", "police"},
    {"H04", "charger outlet"},          {"H08", "car ride"},                  {"B03", "ok safe"},
};

/**
 * A phrase as the fonts can draw it: they have no long dash.
 */
std::string drawn(const char *phrase)
{
    std::string out = phrase;
    for (size_t at; (at = out.find(" \xE2\x80\x94 ")) != std::string::npos;)
        out.replace(at, 5, ", ");
    return out;
}

std::string durationWords(uint32_t minutes)
{
    char buf[32];
    if (minutes >= 1440 && minutes % 1440 == 0)
        snprintf(buf, sizeof(buf), "%u d", (unsigned)(minutes / 1440));
    else if (minutes >= 60 && minutes % 60)
        snprintf(buf, sizeof(buf), "%u h %u min", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
    else if (minutes >= 60)
        snprintf(buf, sizeof(buf), "%u h", (unsigned)(minutes / 60));
    else
        snprintf(buf, sizeof(buf), "%u min", (unsigned)minutes);
    return buf;
}

/**
 * A code in plain words, with what the message says about it put in.
 */
std::string phraseOf(const std::string &code, const mecp::Message *message)
{
    const char *phrase = mecp::phrase(code);
    if (!phrase)
        return code + " (a code this version does not know)";
    if (code == "R03")
        return message && message->hasDuration ? "ETA " + durationWords(message->minutes) : "ETA";
    if (code == "H07") {
        return message && message->hasPeople ? "Have shelter / space for " + std::to_string(message->people)
                                             : std::string("Have shelter / space");
    }
    return drawn(phrase);
}

bool matches(const mecp::Code &entry, const std::string &search)
{
    if (search.empty())
        return true;
    if (folded(entry.phrase).find(search) != std::string::npos || folded(entry.code).find(search) != std::string::npos)
        return true;
    const char *category = mecp::categoryName(entry.code[0]);
    if (category && folded(category).find(search) != std::string::npos)
        return true;
    for (const auto &synonym : c_synonyms)
        if (strcmp(synonym.code, entry.code) == 0 && strstr(synonym.words, search.c_str()))
            return true;
    return false;
}

// a row of the "What" step
struct PhraseRow {
    enum Kind : uint8_t { Continue, Category, Phrase } kind;
    size_t index; // into the code table
};

std::vector<PhraseRow> phraseRows(const std::string &search, size_t picked)
{
    std::vector<PhraseRow> rows;
    if (picked)
        rows.push_back({PhraseRow::Continue, 0});
    size_t count;
    const mecp::Code *table = mecp::codeTable(count);
    const std::string wanted = folded(search.c_str());
    char category = 0;
    for (size_t i = 0; i < count; i++) {
        if (!matches(table[i], wanted))
            continue;
        if (table[i].code[0] != category) {
            category = table[i].code[0];
            rows.push_back({PhraseRow::Category, i});
        }
        rows.push_back({PhraseRow::Phrase, i});
    }
    return rows;
}

/**
 * A row with a colored mark in front, for the severities and the list of messages.
 */
void markRow(lv_obj_t *parent, int32_t y, int32_t height, bool focused, uint32_t color, const char *title, const char *sub,
             const char *right)
{
    lv_obj_t *row = plain(parent, 2, y, c_width - 6, height);
    if (focused) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        focusFrame(row, true, 5);
    } else {
        outline(row, c_raised, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 1, LV_PART_MAIN);
    }
    lv_obj_set_style_radius(box(row, 6, 5, 6, height - 12, color), 2, LV_PART_MAIN);
    const int32_t rightWidth = right && right[0] ? 86 : 0;
    const int32_t width = c_width - 6 - 20 - 8 - rightWidth;
    label(row, 20, sub ? 1 : (height - lv_font_get_line_height(font14)) / 2 - 1, width, font14, c_text, title);
    if (sub)
        label(row, 20, 18, width + rightWidth, font12, focused ? c_focusMuted : c_muted, sub);
    if (rightWidth)
        label(row, c_width - 6 - 8 - rightWidth, 5, rightWidth, font12, focused ? c_focusMuted : c_muted, right,
              LV_TEXT_ALIGN_RIGHT);
}

} // namespace

// -------- reading --------

/**
 * "URGENT: Injury, Stranded / stuck" for a line that is an emergency message; empty for any other text.
 */
std::string KeyboardView_320x240::mecpSummary(const char *text) const
{
    mecp::Message message;
    if (!text || strncmp(text, "MECP/", 5) != 0 || !mecp::decode(text, message))
        return "";
    std::string out = message.drill ? "DRILL " : "";
    out += mecp::severityName(message.severity);
    out += ": ";
    for (size_t i = 0; i < message.codes.size(); i++)
        out += (i ? ", " : "") + phraseOf(message.codes[i], &message);
    return out;
}

/**
 * A Mayday or an Urgent message that is no drill: it alerts even in a muted chat.
 */
bool KeyboardView_320x240::mecpUrgent(const char *text) const
{
    mecp::Message message;
    return text && strncmp(text, "MECP/", 5) == 0 && mecp::decode(text, message) && message.severity <= mecp::Urgent &&
           !message.drill;
}

/**
 * The card: a bar in the color of the severity, what is reported one phrase per line, then the details.
 * parent lays its children out in a column.
 */
void KeyboardView_320x240::renderMecpCard(lv_obj_t *parent, const char *text, int32_t width)
{
    mecp::Message message;
    if (!mecp::decode(text, message))
        return;
    char buf[80];

    lv_obj_t *bar = box(parent, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, c_severityColors[message.severity]);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 1, LV_PART_MAIN);
    snprintf(buf, sizeof(buf), "%s%s", message.drill ? "DRILL  " : "", mecp::severityName(message.severity));
    label(bar, 0, 0, 0, font12, c_ground, buf);

    const auto line = [parent, width](const lv_font_t *font, uint32_t color, const std::string &words) {
        lv_obj_t *obj = label(parent, 0, 0, 0, font, color, words.c_str());
        lv_obj_set_style_max_width(obj, width, LV_PART_MAIN);
    };
    for (const std::string &code : message.codes)
        line(font14, c_text, phraseOf(code, &message));

    if (message.hasPeople && std::find(message.codes.begin(), message.codes.end(), "H07") == message.codes.end())
        line(font12, c_muted, std::to_string(message.people) + (message.people == 1 ? " person" : " people"));
    if (message.hasPosition) {
        snprintf(buf, sizeof(buf), "%.5f, %.5f", message.latitude, message.longitude);
        std::string where = buf;
        const NodeRecord *own = nodeStore.find(ownNode);
        if (own && own->position.hasCoordinates()) {
            const geo::Leg leg = geo::leg(own->position.latitude, own->position.longitude, (int32_t)lround(message.latitude * 1e7),
                                          (int32_t)lround(message.longitude * 1e7));
            geo::distanceText(leg.meters, imperialUnits(), buf, sizeof(buf));
            where += std::string(" · ") + buf + " " + geo::cardinal(leg.bearing);
        }
        line(font12, c_muted, where);
        const std::string town = townOf((int32_t)lround(message.latitude * 1e7), (int32_t)lround(message.longitude * 1e7));
        if (!town.empty())
            line(font12, c_muted, "In " + town);
    }
    if (message.hasTime) {
        struct tm tm = {};
        tm.tm_hour = message.hour;
        tm.tm_min = message.minute;
        char when[16];
        clockString(tm, when, sizeof(when), twelveHour());
        line(font12, c_muted, std::string("Sender's clock: ") + when);
    }
    if (message.hasDuration && std::find(message.codes.begin(), message.codes.end(), "R03") == message.codes.end())
        line(font12, c_muted, "For " + durationWords(message.minutes));
    if (!message.callSign.empty())
        line(font12, c_muted, "Call sign " + message.callSign);
    if (!message.reference.empty())
        line(font12, c_muted, "Reference " + message.reference);
    if (!message.note.empty())
        line(font14, c_text, message.note);
}

/**
 * The emergency messages of all chats, newest first.
 */
std::vector<KeyboardView_320x240::Jump> KeyboardView_320x240::mecpMessages(void) const
{
    std::vector<Jump> found;
    for (const auto &[key, chat] : conversations) {
        for (const Message &msg : chat.messages) {
            if (strncmp(msg.text.c_str(), "MECP/", 5) == 0 && !mecpSummary(msg.text.c_str()).empty())
                found.push_back(Jump{key, msg.id, msg.time});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Jump &a, const Jump &b) { return a.time > b.time; });
    if (found.size() > c_maxJumps)
        found.resize(c_maxJumps);
    return found;
}

/**
 * What OK held on a card offers on top of what every message offers.
 */
void KeyboardView_320x240::mecpMessageMenu(const Message &msg, std::vector<std::string> &items, std::vector<uint32_t> &values) const
{
    mecp::Message message;
    if (strncmp(msg.text.c_str(), "MECP/", 5) != 0 || !mecp::decode(msg.text.c_str(), message))
        return;
    std::vector<std::string> first;
    std::vector<uint32_t> firstValues;
    if (message.hasPosition) {
        first.insert(first.end(), {"Show on the map", "Point to it"});
        firstValues.insert(firstValues.end(), {10, 11});
    }
    if (!msg.own) {
        first.insert(first.end(), {"Answer: Acknowledged", "Answer with other phrases"});
        firstValues.insert(firstValues.end(), {12, 13});
    }
    first.push_back("The line as it was sent");
    firstValues.push_back(14);
    items.insert(items.begin(), first.begin(), first.end());
    values.insert(values.begin(), firstValues.begin(), firstValues.end());
}

bool KeyboardView_320x240::mecpMessagePick(uint32_t value, const Message &msg)
{
    if (value < 10 || value > 14)
        return false;
    mecp::Message message;
    if (!mecp::decode(msg.text.c_str(), message))
        return true;
    switch (value) {
    case 10:
        if (message.hasPosition)
            showPlace(message.latitude, message.longitude);
        break;
    case 11:
        if (message.hasPosition)
            pointTo(message.latitude, message.longitude,
                    (message.codes.empty() ? std::string("Message") : phraseOf(message.codes[0], &message)) +
                        (msg.own ? std::string(" (mine)") : ", " + shortName(msg.from)));
        break;
    case 12: {
        // the same severity, so that it is read with the same weight; a drill is answered as a drill
        mecp::Draft draft;
        draft.severity = message.severity;
        draft.codes = {"R01"};
        if (message.drill)
            draft.codes.push_back("D01");
        const uint32_t key = convKey;
        sendText(key, mecp::encode(draft), msg.id);
        convSelect = -1;
        toast("Acknowledged");
        break;
    }
    case 13:
        openMecpComposer(convKey);
        mecpSeverity = message.severity;
        mecpDrill = message.drill;
        mecpStep = 1;
        break;
    default:
        openInfo("As sent", {msg.text.c_str()});
        break;
    }
    return true;
}

// -------- writing --------

/**
 * Starts a new emergency message. From a chat it goes to that chat; from Apps to the open channel: the one with
 * the default key and the lowest number, or the primary channel when there is none.
 */
void KeyboardView_320x240::openMecpComposer(uint32_t fromChat)
{
    openApp(App::MecpNew, app == App::Mecp && appFromList);
    mecpFrom = fromChat;
    mecpTo = fromChat;
    if (fromChat == UINT32_MAX) {
        mecpTo = 0;
        for (uint8_t i = 0; i < c_max_channels; i++) {
            if (channelInfo[i].enabled && publicKey(i)) {
                mecpTo = i;
                break;
            }
        }
    }
    mecpStep = 0;
    mecpSeverity = mecp::Routine; // the first row
    mecpFocus = mecpTop = 0;
    mecpCodes.clear();
    mecpSearch.clear();
    mecpPeople = 0;
    mecpMinutes = 0;
    mecpPosition = mecpTime = true;
    mecpReference.clear();
    mecpNote.clear();
}

/**
 * The line that would go on air now.
 */
std::string KeyboardView_320x240::mecpLine(bool *cut) const
{
    mecp::Draft draft;
    draft.severity = mecpSeverity;
    draft.codes = mecpCodes;
    if (mecpDrill && std::find(draft.codes.begin(), draft.codes.end(), "D01") == draft.codes.end())
        draft.codes.push_back("D01");
    draft.people = mecpPeople;
    const NodeRecord *own = nodeStore.find(ownNode);
    if (mecpPosition && own && own->position.hasCoordinates()) {
        draft.hasPosition = true;
        draft.latitude = own->position.latitude * 1e-7;
        draft.longitude = own->position.longitude * 1e-7;
    }
    if (mecpTime && validTime(now())) {
        const time_t t = now();
        struct tm tm;
        localtime_r(&t, &tm);
        draft.hasTime = true;
        draft.hour = (uint8_t)tm.tm_hour;
        draft.minute = (uint8_t)tm.tm_min;
    }
    draft.callSign = prefs.callSign;
    draft.reference = mecpReference;
    draft.minutes = mecpMinutes;
    draft.note = mecpNote;
    return mecp::encode(draft, cut);
}

namespace
{
// rows of the details step; "how long" is only there with the phrase that takes it
enum DetailRow : uint8_t { DetailPeople, DetailPosition, DetailTime, DetailCallSign, DetailReference, DetailDuration, DetailNote, DetailNext };

std::vector<DetailRow> detailRows(const std::vector<std::string> &codes)
{
    std::vector<DetailRow> rows = {DetailPeople, DetailPosition, DetailTime, DetailCallSign, DetailReference};
    if (std::find(codes.begin(), codes.end(), "R03") != codes.end())
        rows.push_back(DetailDuration);
    rows.push_back(DetailNote);
    rows.push_back(DetailNext);
    return rows;
}
} // namespace

void KeyboardView_320x240::mecpKey(uint32_t key)
{
    if (app == App::Mecp) {
        const size_t count = mecpMessages().size() + 1;
        if (key == LV_KEY_UP && mecpListFocus > 0)
            mecpListFocus--;
        else if (key == LV_KEY_DOWN && mecpListFocus + 1 < count)
            mecpListFocus++;
        else if (key == LV_KEY_LEFT)
            closeApp();
        else if (key == LV_KEY_RIGHT)
            mecpActivate();
        return;
    }

    size_t count = 0;
    if (mecpStep == 0) {
        count = 5;
    } else if (mecpStep == 1) {
        const std::vector<PhraseRow> rows = phraseRows(mecpSearch, mecpCodes.size());
        count = rows.size();
        if (key == LV_KEY_BACKSPACE) {
            if (!mecpSearch.empty())
                mecpSearch.pop_back();
            mecpFocus = mecpTop = 0;
            return;
        }
        if (printable(key) && mecpSearch.size() < c_searchMax) {
            mecpSearch.push_back((char)key);
            mecpFocus = mecpTop = 0;
            return;
        }
        if ((key == LV_KEY_LEFT || key == LV_KEY_RIGHT) && !rows.empty()) {
            // to the first phrase of the category before or after
            size_t at = std::min(mecpFocus, rows.size() - 1);
            if (key == LV_KEY_RIGHT) {
                for (at++; at < rows.size() && rows[at].kind != PhraseRow::Category; at++) {
                }
                if (at >= rows.size())
                    return;
            } else {
                // the heading of this category, then the one before it
                while (at > 0 && rows[at].kind != PhraseRow::Category)
                    at--;
                if (at > 0)
                    at--;
                while (at > 0 && rows[at].kind != PhraseRow::Category)
                    at--;
            }
            mecpFocus = rows[at].kind == PhraseRow::Category && at + 1 < rows.size() ? at + 1 : at;
            return;
        }
        if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
            // headings are passed over
            size_t at = std::min(mecpFocus, count ? count - 1 : 0);
            const int step = key == LV_KEY_UP ? -1 : 1;
            while ((step < 0 && at > 0) || (step > 0 && at + 1 < count)) {
                at += step;
                if (rows[at].kind != PhraseRow::Category) {
                    mecpFocus = at;
                    break;
                }
            }
            if (step < 0 && at == 0 && !rows.empty() && rows[0].kind == PhraseRow::Category)
                mecpTop = 0; // show the first heading again
            return;
        }
        return;
    } else if (mecpStep == 2) {
        count = detailRows(mecpCodes).size();
    } else {
        count = 2;
    }
    if (key == LV_KEY_UP && mecpFocus > 0)
        mecpFocus--;
    else if (key == LV_KEY_DOWN && mecpFocus + 1 < count)
        mecpFocus++;
    else if (key == LV_KEY_LEFT)
        closeApp(); // a step back
    else if (key == LV_KEY_RIGHT && mecpStep != 3)
        mecpActivate();
}

void KeyboardView_320x240::mecpActivate(void)
{
    if (app == App::Mecp) {
        if (mecpListFocus == 0) {
            openMecpComposer(UINT32_MAX);
            return;
        }
        const std::vector<Jump> found = mecpMessages();
        if (mecpListFocus - 1 < found.size())
            openChatAt(found[mecpListFocus - 1]);
        return;
    }
    const auto next = [this](void) {
        mecpStep++;
        mecpFocus = mecpTop = 0;
    };
    char buf[16];
    switch (mecpStep) {
    case 0:
        if (mecpFocus < 4) {
            mecpSeverity = (uint8_t)(3 - mecpFocus);
            next();
        } else {
            mecpDrill = !mecpDrill;
        }
        break;
    case 1: {
        const std::vector<PhraseRow> rows = phraseRows(mecpSearch, mecpCodes.size());
        if (mecpFocus >= rows.size())
            break;
        const PhraseRow &row = rows[mecpFocus];
        if (row.kind == PhraseRow::Continue) {
            mecpSearch.clear();
            next();
        } else if (row.kind == PhraseRow::Phrase) {
            size_t count;
            const std::string code = mecp::codeTable(count)[row.index].code;
            const auto it = std::find(mecpCodes.begin(), mecpCodes.end(), code);
            const bool hadRow = !mecpCodes.empty();
            if (it != mecpCodes.end())
                mecpCodes.erase(it);
            else if (mecpCodes.size() < 8)
                mecpCodes.push_back(code);
            else
                toast("Eight phrases are the most");
            // the row "Continue" comes and goes at the top: the focus stays on its phrase
            if (!hadRow && !mecpCodes.empty())
                mecpFocus++;
            else if (hadRow && mecpCodes.empty() && mecpFocus > 0)
                mecpFocus--;
        }
        break;
    }
    case 2: {
        const std::vector<DetailRow> rows = detailRows(mecpCodes);
        switch (rows[std::min(mecpFocus, rows.size() - 1)]) {
        case DetailPeople:
            snprintf(buf, sizeof(buf), "%u", (unsigned)mecpPeople);
            openEditor(Setting::MecpPeople, "How many people", mecpPeople ? buf : "", 4, "A number", "Empty leaves it out");
            break;
        case DetailPosition: {
            const NodeRecord *own = nodeStore.find(ownNode);
            if (own && own->position.hasCoordinates())
                mecpPosition = !mecpPosition;
            else
                toast("No GPS fix to send");
            break;
        }
        case DetailTime:
            mecpTime = !mecpTime;
            break;
        case DetailCallSign:
            openEditor(Setting::MecpCallSign, "Call sign", prefs.callSign, 12, "Such as W1ABC", "Kept for the next message");
            break;
        case DetailReference:
            openEditor(Setting::MecpReference, "Reference", mecpReference, 24, "A short tag", "Letters, digits and hyphens");
            break;
        case DetailDuration:
            openEditor(Setting::MecpDuration, "How long until you arrive", mecpMinutes ? mecp::durationText(mecpMinutes) : "", 10,
                       "45min, 1h30min or 2d", "A bare number counts as minutes");
            break;
        case DetailNote:
            openEditor(Setting::MecpNote, "Note", mecpNote, 160, "Anything else", "Cut when the message gets too long");
            break;
        default:
            next();
            mecpFocus = 1; // on "Send"
            break;
        }
        break;
    }
    default:
        if (mecpFocus == 0) {
            // every chat can take it
            std::vector<std::string> items;
            std::vector<uint32_t> values;
            int32_t current = -1;
            for (const uint32_t key : chatOrder()) {
                if (key == mecpTo)
                    current = (int32_t)items.size();
                items.push_back(key < c_max_channels ? "# " + chatName(key) : chatName(key));
                values.push_back(key);
            }
            openPicker(Setting::MecpTo, "Send to", std::move(items), std::move(values), current);
        } else if (mecpSeverity <= mecp::Urgent && !mecpDrill) {
            toast(mecpSeverity == mecp::Mayday ? "Hold OK to send a Mayday" : "Hold OK to send an Urgent message", 3000);
        } else {
            mecpSend();
        }
        break;
    }
}

/**
 * OK held: on the last step it sends, whatever the severity; on the phrases it goes on.
 */
void KeyboardView_320x240::mecpHold(void)
{
    if (app != App::MecpNew)
        return;
    if (mecpStep == 3 && mecpFocus == 1) {
        mecpSend();
    } else if (mecpStep == 1 && !mecpCodes.empty()) {
        mecpSearch.clear();
        mecpStep = 2;
        mecpFocus = mecpTop = 0;
    } else {
        mecpActivate();
    }
}

void KeyboardView_320x240::mecpSend(void)
{
    const std::string line = mecpLine();
    if (line.empty()) {
        toast("Pick what to report first");
        return;
    }
    const uint32_t to = mecpTo;
    sendText(to, line);
    app = App::None;
    appFromList = false;
    openChat(to);
    toast((std::string("Sent to ") + (to < c_max_channels ? "# " : "") + chatName(to)).c_str(), 3000);
}

/**
 * Back inside the composer: out of the search, then a step back; from the first step to where it was opened.
 */
bool KeyboardView_320x240::mecpBack(void)
{
    if (app != App::MecpNew)
        return false;
    if (mecpStep == 1 && !mecpSearch.empty()) {
        mecpSearch.clear();
        mecpFocus = mecpTop = 0;
        return true;
    }
    if (mecpStep > 0) {
        mecpStep--;
        mecpFocus = mecpStep == 0 ? 3 - std::min<uint8_t>(mecpSeverity, 3) : 0;
        mecpTop = 0;
        return true;
    }
    if (mecpFrom != UINT32_MAX) {
        const uint32_t chat = mecpFrom;
        app = App::None;
        appFromList = false;
        openChat(chat);
        return true;
    }
    app = App::Mecp; // from where the list of Apps is one more step back
    return true;
}

bool KeyboardView_320x240::mecpPick(Setting what, uint32_t value, uint32_t)
{
    if (what != Setting::MecpTo)
        return false;
    mecpTo = value;
    return true;
}

bool KeyboardView_320x240::mecpEdit(Setting what, const std::string &text, uint32_t)
{
    switch (what) {
    case Setting::MecpPeople:
        mecpPeople = (uint32_t)std::min<unsigned long>(9999, strtoul(text.c_str(), nullptr, 10));
        return true;
    case Setting::MecpCallSign:
        prefs.callSign = mecp::cleanCallSign(text);
        savePrefs();
        if (prefs.callSign.empty() && !text.empty())
            toast("Letters, digits and / only");
        return true;
    case Setting::MecpReference:
        mecpReference = mecp::cleanReference(text);
        return true;
    case Setting::MecpDuration: {
        // read the way a receiver would read it
        mecp::Message message;
        mecpMinutes = mecp::decode("MECP/3/R03 " + text, message) && message.hasDuration ? message.minutes : 0;
        if (!mecpMinutes && !text.empty())
            toast("Not a time: try 45min or 1h30min");
        return true;
    }
    case Setting::MecpNote:
        mecpNote = text;
        return true;
    default:
        return false;
    }
}

// -------- screens --------

void KeyboardView_320x240::renderMecpList(void)
{
    const std::vector<Jump> found = mecpMessages();
    const size_t count = found.size() + 1;
    mecpListFocus = std::min(mecpListFocus, count - 1);
    mecpListTop = scrollTop(mecpListTop, mecpListFocus, count, c_listRows);
    for (size_t i = mecpListTop; i < count && i < mecpListTop + c_listRows; i++) {
        const int32_t y = 2 + (int32_t)(i - mecpListTop) * c_listRowHeight;
        if (i == 0) {
            markRow(content, y, c_listRowHeight, mecpListFocus == 0, c_focus, "New message",
                    found.empty() ? "No emergency message has come in" : "Written in plain words, sent as MECP", "");
            continue;
        }
        const Jump &jump = found[i - 1];
        const auto chat = conversations.find(jump.key);
        const Message *msg = nullptr;
        if (chat != conversations.end()) {
            for (const Message &candidate : chat->second.messages) {
                if (candidate.id == jump.id && candidate.time == jump.time) {
                    msg = &candidate;
                    break;
                }
            }
        }
        if (!msg)
            continue;
        mecp::Message message;
        mecp::decode(msg->text.c_str(), message);
        std::string title = mecpSummary(msg->text.c_str());
        char when[16] = "";
        if (validTime(msg->time))
            chatTimeString(msg->time, now(), when, sizeof(when), twelveHour());
        const std::string sub = (msg->own ? std::string("Me") : shortName(msg->from)) + " in " +
                                (jump.key < c_max_channels ? "# " : "") + chatName(jump.key);
        markRow(content, y, c_listRowHeight, i == mecpListFocus, c_severityColors[message.severity], title.c_str(), sub.c_str(), when);
    }
    listScrollbar(content, 2, mecpListTop, count, c_listRows, c_listRowHeight);
}

void KeyboardView_320x240::renderMecpComposer(void)
{
    static const char *const steps[] = {"1 of 4  How serious is it?", "2 of 4  What is it?", "3 of 4  Details, all optional",
                                        "4 of 4  Check and send"};
    char buf[64];
    bool cut = false;
    const std::string line = mecpLine(&cut);

    // the line above the rows: where we are, and on the right what matters for the step
    label(content, 8, 4, 190, font12, mecpDrill ? c_focus : c_muted, steps[mecpStep]);
    const int32_t top = c_headHeight;
    const auto rows = [this, top](size_t count, const std::function<void(size_t, int32_t, bool)> &draw) {
        mecpFocus = std::min(mecpFocus, count ? count - 1 : 0);
        mecpTop = scrollTop(mecpTop, mecpFocus, count, c_stepRows);
        for (size_t i = mecpTop; i < count && i < mecpTop + c_stepRows; i++)
            draw(i, top + (int32_t)(i - mecpTop) * c_settingHeight, i == mecpFocus);
        listScrollbar(content, top, mecpTop, count, c_stepRows, c_settingHeight);
    };

    switch (mecpStep) {
    case 0:
        rows(5, [this](size_t i, int32_t y, bool focused) {
            if (i < 4)
                markRow(content, y, c_settingHeight, focused, c_severityColors[3 - i], c_severityWords[3 - i], nullptr, "");
            else
                settingRow(content, y, focused, "This is a drill", mecpDrill ? "On" : "Off", mecpDrill);
        });
        label(content, 8, 196, c_width - 16, font12, c_focusMuted, "OK: choose   Back: leave", LV_TEXT_ALIGN_CENTER);
        break;
    case 1: {
        if (mecpSearch.empty())
            snprintf(buf, sizeof(buf), "%u picked · type to search", (unsigned)mecpCodes.size());
        else
            snprintf(buf, sizeof(buf), "Search: %s", mecpSearch.c_str());
        label(content, 150, 4, c_width - 158, font12, mecpSearch.empty() ? c_muted : c_focus, buf, LV_TEXT_ALIGN_RIGHT);
        const std::vector<PhraseRow> list = phraseRows(mecpSearch, mecpCodes.size());
        if (list.empty()) {
            emptyNote(content, "No phrase has these letters");
            break;
        }
        // a focus that came to rest on a heading moves on to its first phrase
        mecpFocus = std::min(mecpFocus, list.size() - 1);
        if (list[mecpFocus].kind == PhraseRow::Category && mecpFocus + 1 < list.size())
            mecpFocus++;
        size_t count;
        const mecp::Code *table = mecp::codeTable(count);
        rows(list.size(), [this, &list, table](size_t i, int32_t y, bool focused) {
            const PhraseRow &row = list[i];
            if (row.kind == PhraseRow::Continue) {
                const std::string title = "Continue with " + std::to_string(mecpCodes.size()) + " picked";
                settingRow(content, y, focused, title.c_str(), "Hold OK anywhere", false);
            } else if (row.kind == PhraseRow::Category) {
                label(content, 10, y + 8, c_width - 24, font12, c_blue, drawn(mecp::categoryName(table[row.index].code[0])).c_str());
            } else {
                const bool picked = std::find(mecpCodes.begin(), mecpCodes.end(), table[row.index].code) != mecpCodes.end();
                settingRow(content, y, focused, phraseOf(table[row.index].code, nullptr).c_str(), picked ? "Picked" : "", picked);
            }
        });
        break;
    }
    case 2: {
        snprintf(buf, sizeof(buf), "%u of %u bytes%s", (unsigned)line.size(), (unsigned)mecp::c_maxBytes, cut ? " · note cut" : "");
        label(content, 170, 4, c_width - 178, font12, cut ? c_focus : c_muted, buf, LV_TEXT_ALIGN_RIGHT);
        const std::vector<DetailRow> list = detailRows(mecpCodes);
        const NodeRecord *own = nodeStore.find(ownNode);
        const bool fix = own && own->position.hasCoordinates();
        rows(list.size(), [this, &list, own, fix](size_t i, int32_t y, bool focused) {
            char value[48];
            switch (list[i]) {
            case DetailPeople:
                settingRow(content, y, focused, "People", mecpPeople ? std::to_string(mecpPeople).c_str() : "Not given", false);
                break;
            case DetailPosition:
                if (fix && mecpPosition)
                    snprintf(value, sizeof(value), "%.5f, %.5f", own->position.latitude * 1e-7, own->position.longitude * 1e-7);
                else
                    snprintf(value, sizeof(value), "%s", fix ? "Left out" : "No GPS fix");
                settingRow(content, y, focused, "My position", value, !fix);
                break;
            case DetailTime:
                settingRow(content, y, focused, "Time now", mecpTime && validTime(now()) ? "Sent along" : "Left out", false);
                break;
            case DetailCallSign:
                settingRow(content, y, focused, "Call sign", prefs.callSign.empty() ? "None" : prefs.callSign.c_str(), false);
                break;
            case DetailReference:
                settingRow(content, y, focused, "Reference", mecpReference.empty() ? "None" : mecpReference.c_str(), false);
                break;
            case DetailDuration:
                settingRow(content, y, focused, "How long (ETA)", mecpMinutes ? durationWords(mecpMinutes).c_str() : "Not given",
                           !mecpMinutes);
                break;
            case DetailNote:
                settingRow(content, y, focused, "Note", mecpNote.empty() ? "None" : mecpNote.c_str(), false);
                break;
            default:
                settingRow(content, y, focused, "Continue", "Check and send", false);
                break;
            }
        });
        break;
    }
    default: {
        // the message as the receiver's screen shows it, the line that goes on air, where it goes
        const int32_t rowsTop = lv_obj_get_height(content) - 2 * c_settingHeight - 2;
        lv_obj_t *card = box(content, 6, top, c_width - 12, rowsTop - top - 34, c_raised);
        lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(card, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(card, 3, LV_PART_MAIN);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);
        renderMecpCard(card, line.c_str(), c_width - 28);
        lv_obj_t *raw = label(content, 8, rowsTop - 32, 0, font12, cut ? c_focus : c_muted, line.c_str());
        lv_obj_set_width(raw, c_width - 16);
        lv_obj_set_height(raw, 2 * lv_font_get_line_height(font12));
        lv_label_set_long_mode(raw, LV_LABEL_LONG_MODE_DOTS);
        mecpFocus = std::min<size_t>(mecpFocus, 1);
        const std::string to = (mecpTo < c_max_channels ? "# " : "") + chatName(mecpTo);
        const bool open = mecpTo < c_max_channels && publicKey((uint8_t)mecpTo);
        settingRow(content, rowsTop, mecpFocus == 0, "To", (to + (open ? " · open channel" : "")).c_str(), false);
        const bool hold = mecpSeverity <= mecp::Urgent && !mecpDrill;
        snprintf(buf, sizeof(buf), "%s%s", mecpDrill ? "DRILL " : "", mecp::severityName(mecpSeverity));
        settingRow(content, rowsTop + c_settingHeight, mecpFocus == 1, hold ? "Send: hold OK" : "Send", buf, hold);
        break;
    }
    }
}

#endif
