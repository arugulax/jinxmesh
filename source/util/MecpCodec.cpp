#include "util/MecpCodec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace mecp
{

namespace
{

const Code c_codes[] = {
    {"M01", "Injury"},
    {"M02", "Unconscious person"},
    {"M03", "Breathing difficulty"},
    {"M04", "Cardiac event"},
    {"M05", "Hypothermia"},
    {"M06", "Severe bleeding"},
    {"M07", "Fracture / immobile"},
    {"M08", "Burns"},
    {"M09", "Multiple casualties"},
    {"M10", "Deceased"},
    {"M11", "Animal bite / sting"},
    {"M12", "Allergic reaction / anaphylaxis"},
    {"M13", "Poisoning / toxic exposure"},
    {"M14", "Persons located alive"},
    {"M15", "Area searched, no victims found"},
    {"M16", "Medical supply drop needed"},
    {"T01", "Road blocked"},
    {"T02", "Bridge out"},
    {"T03", "Building collapsed"},
    {"T04", "Flooding"},
    {"T05", "Landslide"},
    {"T06", "Power out"},
    {"T07", "Fire"},
    {"T08", "Avalanche"},
    {"T09", "Path impassable"},
    {"T10", "Shelter available"},
    {"T11", "Drowning / water rescue needed"},
    {"T12", "Water contamination"},
    {"T13", "Earthquake"},
    {"T14", "Gas leak"},
    {"T15", "Chemical spill / HAZMAT"},
    {"T16", "Vehicle accident"},
    {"T17", "Vehicle fire"},
    {"W01", "Storm approaching"},
    {"W02", "Visibility zero"},
    {"W03", "Extreme cold"},
    {"W04", "Extreme heat"},
    {"W05", "Air quality danger"},
    {"W06", "Tsunami / tidal surge warning"},
    {"S01", "Need water"},
    {"S02", "Need food"},
    {"S03", "Need medication"},
    {"S04", "Need battery / power"},
    {"S05", "Need fuel"},
    {"S06", "Need tools / equipment"},
    {"P01", "Stranded / stuck"},
    {"P02", "Evacuating toward"},
    {"P03", "Sheltering in place"},
    {"P04", "En route to"},
    {"P05", "At GPS coordinates"},
    {"P06", "Lost"},
    {"P07", "Group separated"},
    {"C01", "Send rescue"},
    {"C02", "Need transport"},
    {"C03", "Relay this message"},
    {"C04", "Confirm received"},
    {"C05", "How many people"},
    {"C06", "What is status"},
    {"C07", "Can you reach"},
    {"C08", "Rendezvous at"},
    {"R01", "Acknowledged"},
    {"R02", "Help coming"},
    {"R03", "ETA [time]"},
    {"R04", "Cannot assist"},
    {"R05", "Redirecting to"},
    {"R06", "Stand by"},
    {"R07", "Situation resolved / all clear"},
    {"D01", "This is a drill"},
    {"D02", "This is a test"},
    {"D03", "End of drill"},
    {"D04", "Ignore previous \xE2\x80\x94 sent in error"},
    {"L01", "Beer / drinks"},
    {"L02", "Coffee"},
    {"L03", "Food ready"},
    {"L04", "Summit reached"},
    {"L05", "At camp"},
    {"L06", "Running late"},
    {"L07", "Good signal here"},
    {"L08", "Photo opportunity"},
    {"L09", "Wildlife spotted"},
    {"L10", "Beautiful view"},
    {"L11", "Trail conditions good"},
    {"L12", "Trail conditions bad"},
    {"L13", "Need a break"},
    {"L14", "Heading home"},
    {"L15", "Good morning / check-in"},
    {"L16", "Good night"},
    {"L17", "Thank you"},
    {"L18", "Having fun"},
    {"L19", "Festival / event here"},
    {"L20", "Node test / ping"},
    {"X01", "Dangerous person / threat nearby"},
    {"X02", "Area unsafe \xE2\x80\x94 avoid"},
    {"X03", "Gunfire / explosions heard"},
    {"X04", "Civil unrest / crowd danger"},
    {"X05", "Theft / looting reported"},
    {"X06", "Authorities / emergency services present"},
    {"X07", "Checkpoint / road closure (authority)"},
    {"H01", "Have water available"},
    {"H02", "Have food available"},
    {"H03", "Have medical supplies"},
    {"H04", "Have power / charging"},
    {"H05", "Have fuel"},
    {"H06", "Have tools / equipment"},
    {"H07", "Have shelter / space for [N]pax"},
    {"H08", "Have transport / vehicle"},
    {"B01", "Automated distress beacon active \xE2\x80\x94 sender may be unresponsive"},
    {"B02", "Beacon acknowledged \xE2\x80\x94 reduce transmission rate"},
    {"B03", "Cancel beacon \xE2\x80\x94 I am OK"},
};

const struct {
    char letter;
    const char *name;
} c_categories[] = {
    {'M', "Medical"},      {'T', "Terrain / Infrastructure"}, {'W', "Weather / Environment"}, {'S', "Supplies"},
    {'P', "Position / Movement"}, {'C', "Coordination"},      {'R', "Response"},              {'D', "Drill / Test"},
    {'L', "Life / Leisure"},      {'X', "Threat / Security"}, {'H', "Have / Offer Resources"}, {'B', "Beacon"},
};

// -------- text as code points --------

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isUpper(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool isLower(char c)
{
    return c >= 'a' && c <= 'z';
}

bool isAlpha(char c)
{
    return isUpper(c) || isLower(c);
}

bool isAlnum(char c)
{
    return isAlpha(c) || isDigit(c);
}

char lower(char c)
{
    return isUpper(c) ? (char)(c - 'A' + 'a') : c;
}

char upper(char c)
{
    return isLower(c) ? (char)(c - 'a' + 'A') : c;
}

/**
 * The code point that starts at pos, and how many bytes it takes. A byte that is not UTF-8 counts as one
 * character of its own.
 */
uint32_t codePoint(const std::string &s, size_t pos, size_t &length)
{
    const auto b = [&s](size_t i) { return (uint8_t)s[i]; };
    const uint8_t first = b(pos);
    length = 1;
    if (first < 0x80)
        return first;
    size_t need = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 0;
    if (!need || pos + need > s.size())
        return 0xFFFD;
    uint32_t cp = first & (0xFF >> (need + 1));
    for (size_t i = 1; i < need; i++) {
        if ((b(pos + i) & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (b(pos + i) & 0x3F);
    }
    length = need;
    return cp;
}

/**
 * Where the code point in front of pos starts.
 */
size_t previousStart(const std::string &s, size_t pos, size_t floor)
{
    size_t at = pos - 1;
    while (at > floor && pos - at < 4 && ((uint8_t)s[at] & 0xC0) == 0x80)
        at--;
    size_t length;
    codePoint(s, at, length);
    return at + length == pos ? at : pos - 1;
}

/**
 * What may stand on either side of a detail: white space and punctuation, each one code point.
 */
bool isBoundary(uint32_t cp)
{
    switch (cp) {
    case '\t':
    case '\n':
    case '\f':
    case '\r':
    case ' ':
    case ',':
    case '.':
    case ';':
    case ':':
    case '!':
    case '?':
    case '\'':
    case '"':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case 0x00A0:
    case 0x1680:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0x060C:
    case 0x061B:
    case 0x061F:
    case 0x06D4:
    case 0x00AB:
    case 0x00BB:
    case 0x2013:
    case 0x2014:
    case 0x2026:
    case 0x3001:
    case 0x3002:
    case 0x3010:
    case 0x3011:
    case 0xFF01:
    case 0xFF08:
    case 0xFF09:
    case 0xFF0C:
    case 0xFF0E:
    case 0xFF1A:
    case 0xFF1B:
    case 0xFF1F:
        return true;
    default:
        return (cp >= 0x2000 && cp <= 0x200A) || (cp >= 0x2018 && cp <= 0x201D) || (cp >= 0x300C && cp <= 0x300F);
    }
}

bool isLineBreak(uint32_t cp)
{
    return cp == '\n' || cp == '\f' || cp == '\r' || cp == 0x0085 || cp == 0x2028 || cp == 0x2029;
}

// -------- searching the free text --------

struct Span {
    size_t from = 0, to = 0;
    bool empty(void) const { return to <= from; }
    std::string of(const std::string &s) const { return s.substr(from, to - from); }
};

struct Match {
    size_t start = 0;     // of the match with the boundary in front of it: what "first by position" compares
    Span token;           // without the boundaries
    Span first, second;   // what the pattern captures
};

/**
 * A pattern: tries to match at pos and offers every end it could have, the one a regular expression would try
 * first coming first, until accept() takes one. Fills in what it captured for that end.
 */
using Accept = std::function<bool(size_t end)>;
using Pattern = std::function<bool(const std::string &s, size_t pos, Match &m, const Accept &accept)>;

/**
 * The first match of a pattern from pos on that stands between boundaries (or the ends of the text) and passes
 * valid(). A match that fails valid() is skipped and the search goes on behind it. anchored: only at pos itself.
 */
bool findFirst(const std::string &s, const Pattern &pattern, const std::function<bool(const Match &)> &valid, Match &found,
               bool anchored = false)
{
    const Accept atBoundary = [&s](size_t end) {
        if (end == s.size())
            return true;
        size_t length;
        return isBoundary(codePoint(s, end, length));
    };
    size_t from = 0;
    while (from <= s.size()) {
        bool matched = false;
        Match m;
        for (size_t at = from; at <= s.size();) {
            size_t start = at;
            bool allowed = at == from;
            if (!allowed) {
                const size_t before = previousStart(s, at, from);
                size_t length;
                allowed = isBoundary(codePoint(s, before, length));
                start = before;
            }
            if (allowed) {
                m = Match();
                if (pattern(s, at, m, atBoundary)) {
                    m.start = start;
                    m.token.from = at;
                    matched = true;
                    break;
                }
            }
            if (anchored || at == s.size())
                break;
            size_t length;
            codePoint(s, at, length);
            at += length;
        }
        if (!matched)
            return false;
        if (!valid || valid(m)) {
            found = m;
            return true;
        }
        from = std::max(m.token.to, m.start + 1);
    }
    return false;
}

size_t digitsEnd(const std::string &s, size_t pos)
{
    while (pos < s.size() && isDigit(s[pos]))
        pos++;
    return pos;
}

bool wordAt(const std::string &s, size_t pos, const char *lowercase)
{
    const size_t length = strlen(lowercase);
    if (pos + length > s.size())
        return false;
    for (size_t i = 0; i < length; i++)
        if (lower(s[pos + i]) != lowercase[i])
            return false;
    return true;
}

/**
 * A number of any length: what does not fit counts as the largest.
 */
uint64_t number(const std::string &s, const Span &span)
{
    uint64_t value = 0;
    for (size_t i = span.from; i < span.to; i++) {
        if (value > (UINT64_MAX - 9) / 10)
            return UINT64_MAX;
        value = value * 10 + (uint64_t)(s[i] - '0');
    }
    return value;
}

// 0*([1-9][0-9]*) ?[Pp][Aa][Xx]
bool peoplePattern(const std::string &s, size_t pos, Match &m, const Accept &accept)
{
    size_t at = pos;
    while (at < s.size() && s[at] == '0')
        at++;
    if (at >= s.size() || !isDigit(s[at]))
        return false;
    m.first.from = at;
    m.first.to = at = digitsEnd(s, at);
    if (at < s.size() && s[at] == ' ' && wordAt(s, at + 1, "pax"))
        at++;
    if (!wordAt(s, at, "pax"))
        return false;
    m.token.to = at + 3;
    return accept(m.token.to);
}

// [+-]?[0-9]+\.[0-9]+
bool decimalAt(const std::string &s, size_t pos, Span &span)
{
    size_t at = pos;
    if (at < s.size() && (s[at] == '+' || s[at] == '-'))
        at++;
    const size_t whole = digitsEnd(s, at);
    if (whole == at || whole >= s.size() || s[whole] != '.')
        return false;
    const size_t fraction = digitsEnd(s, whole + 1);
    if (fraction == whole + 1)
        return false;
    span.from = pos;
    span.to = fraction;
    return true;
}

// ([+-]?[0-9]+\.[0-9]+), ?([+-]?[0-9]+\.[0-9]+)
bool positionPattern(const std::string &s, size_t pos, Match &m, const Accept &accept)
{
    if (!decimalAt(s, pos, m.first))
        return false;
    size_t at = m.first.to;
    if (at >= s.size() || s[at] != ',')
        return false;
    at++;
    // the space is optional, and a number cannot start with one
    if (!decimalAt(s, at, m.second) && !(at < s.size() && s[at] == ' ' && decimalAt(s, at + 1, m.second)))
        return false;
    // a shorter second number would end in front of a digit, which is no boundary
    m.token.to = m.second.to;
    return accept(m.token.to);
}

// @([01]?[0-9]|2[0-3]):?([0-5][0-9])
bool timePattern(const std::string &s, size_t pos, Match &m, const Accept &accept)
{
    if (pos >= s.size() || s[pos] != '@')
        return false;
    const size_t at = pos + 1;
    const auto digit = [&s](size_t i) { return i < s.size() && isDigit(s[i]); };
    // the hours as the expression tries them: two digits from 00 to 19, one digit, 20 to 23
    size_t hourEnds[3];
    size_t count = 0;
    if (digit(at) && (s[at] == '0' || s[at] == '1') && digit(at + 1))
        hourEnds[count++] = at + 2;
    if (digit(at))
        hourEnds[count++] = at + 1;
    if (digit(at) && s[at] == '2' && digit(at + 1) && s[at + 1] <= '3')
        hourEnds[count++] = at + 2;
    for (size_t i = 0; i < count; i++) {
        for (int colon = 1; colon >= 0; colon--) {
            size_t minute = hourEnds[i];
            if (colon) {
                if (minute >= s.size() || s[minute] != ':')
                    continue;
                minute++;
            }
            if (!digit(minute) || s[minute] > '5' || !digit(minute + 1))
                continue;
            m.first = {at, hourEnds[i]};
            m.second = {minute, minute + 2};
            m.token.to = minute + 2;
            if (accept(m.token.to))
                return true;
        }
    }
    return false;
}

// (?:[-_][A-Za-z0-9]{2,8})* from at on: the longest way first
bool languageParts(const std::string &s, size_t at, const Accept &accept, size_t &end)
{
    if (at < s.size() && (s[at] == '-' || s[at] == '_')) {
        size_t run = 0;
        while (at + 1 + run < s.size() && isAlnum(s[at + 1 + run]) && run < 8)
            run++;
        for (size_t length = run; length >= 2; length--)
            if (languageParts(s, at + 1 + length, accept, end))
                return true;
    }
    if (!accept(at))
        return false;
    end = at;
    return true;
}

// @([A-Za-z]{2,3}(?:[-_][A-Za-z0-9]{2,8})*)
bool languagePattern(const std::string &s, size_t pos, Match &m, const Accept &accept)
{
    if (pos >= s.size() || s[pos] != '@')
        return false;
    size_t letters = 0;
    while (pos + 1 + letters < s.size() && isAlpha(s[pos + 1 + letters]) && letters < 3)
        letters++;
    for (size_t length = letters; length >= 2; length--) {
        size_t end;
        if (languageParts(s, pos + 1 + length, accept, end)) {
            m.first = {pos + 1, end};
            m.token.to = end;
            return true;
        }
    }
    return false;
}

// ~([A-Za-z0-9/]+)
bool callSignPattern(const std::string &s, size_t pos, Match &m, const Accept &accept)
{
    if (pos >= s.size() || s[pos] != '~')
        return false;
    size_t at = pos + 1;
    while (at < s.size() && (isAlnum(s[at]) || s[at] == '/'))
        at++;
    if (at == pos + 1)
        return false;
    m.first = {pos + 1, at};
    m.token.to = at;
    return accept(at);
}

// #([A-Za-z0-9_-]+)
bool referencePattern(const std::string &s, size_t pos, Match &m, const Accept &accept)
{
    if (pos >= s.size() || s[pos] != '#')
        return false;
    size_t at = pos + 1;
    while (at < s.size() && (isAlnum(s[at]) || s[at] == '_' || s[at] == '-'))
        at++;
    if (at == pos + 1)
        return false;
    m.first = {pos + 1, at};
    m.token.to = at;
    return accept(at);
}

// (?:[Ee][Tt][Aa] ?)?([0-9]+): where the number starts, or npos
size_t etaNumber(const std::string &s, size_t pos, Span &digits)
{
    size_t at = pos;
    if (wordAt(s, at, "eta")) {
        at += 3;
        if (at < s.size() && s[at] == ' ' && at + 1 < s.size() && isDigit(s[at + 1]))
            at++;
    }
    const size_t end = digitsEnd(s, at);
    if (end == at)
        return std::string::npos;
    digits = {at, end};
    return end;
}

size_t skipSpace(const std::string &s, size_t at)
{
    return at < s.size() && s[at] == ' ' ? at + 1 : at;
}

enum class Unit { HoursMinutes, Hours, Minutes, Days, Bare };

/**
 * The five ways a duration is read. An optional space is tried with and without; for these patterns only one of
 * the two can go on.
 */
Pattern durationPattern(Unit unit)
{
    return [unit](const std::string &s, size_t pos, Match &m, const Accept &accept) {
        size_t at = etaNumber(s, pos, m.first);
        if (at == std::string::npos)
            return false;
        switch (unit) {
        case Unit::HoursMinutes: {
            at = skipSpace(s, at);
            if (!wordAt(s, at, "h"))
                return false;
            at = skipSpace(s, at + 1);
            const size_t end = digitsEnd(s, at);
            if (end == at)
                return false;
            m.second = {at, end};
            at = skipSpace(s, end);
            if (!wordAt(s, at, "min"))
                return false;
            m.token.to = at + 3;
            return accept(m.token.to);
        }
        case Unit::Hours:
        case Unit::Days:
            at = skipSpace(s, at);
            if (!wordAt(s, at, unit == Unit::Hours ? "h" : "d"))
                return false;
            m.token.to = at + 1;
            return accept(m.token.to);
        case Unit::Minutes:
            at = skipSpace(s, at);
            if (!wordAt(s, at, "min"))
                return false;
            m.token.to = at + 3;
            return accept(m.token.to);
        case Unit::Bare:
        default:
            // ([0-9]+)([.,][0-9]+)? : with the fraction first, a number with one is refused by the caller
            if (at < s.size() && (s[at] == '.' || s[at] == ',')) {
                const size_t end = digitsEnd(s, at + 1);
                if (end > at + 1) {
                    m.second = {at, end};
                    m.token.to = end;
                    if (accept(end))
                        return true;
                    m.second = {};
                }
            }
            m.token.to = at;
            return accept(at);
        }
    };
}

uint64_t minutesOf(Unit unit, const std::string &s, const Match &m)
{
    const uint64_t a = number(s, m.first);
    const uint64_t cap = UINT32_MAX;
    switch (unit) {
    case Unit::HoursMinutes: {
        const uint64_t b = number(s, m.second);
        return a > cap / 60 || b > cap ? cap : std::min<uint64_t>(cap, a * 60 + b);
    }
    case Unit::Hours:
        return a > cap / 60 ? cap : a * 60;
    case Unit::Days:
        return a > cap / 1440 ? cap : a * 1440;
    default:
        return std::min<uint64_t>(cap, a);
    }
}

/**
 * The free text without the details that were read out of it. Each goes with one of the spaces around it.
 */
std::string without(const std::string &text, std::vector<Span> spans)
{
    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.from < b.from; });
    std::string out;
    size_t at = 0;
    for (const Span &span : spans) {
        if (span.from < at)
            continue;
        out.append(text, at, span.from - at);
        at = span.to;
        if (at < text.size() && text[at] == ' ' && (out.empty() || out.back() == ' '))
            at++;
    }
    out.append(text, at, std::string::npos);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

/**
 * True for what may follow the codes: no line break in it, no white space at either end.
 */
bool validFreetext(const std::string &s, size_t from)
{
    if (from >= s.size())
        return false;
    uint32_t first = 0, last = 0;
    for (size_t at = from; at < s.size();) {
        size_t length;
        const uint32_t cp = codePoint(s, at, length);
        if (isLineBreak(cp))
            return false;
        if (at == from)
            first = cp;
        last = cp;
        at += length;
    }
    return first != ' ' && first != '\t' && last != ' ' && last != '\t';
}

} // namespace

bool isCode(const std::string &text)
{
    return text.size() == 3 && isUpper(text[0]) && isDigit(text[1]) && isDigit(text[2]);
}

const Code *codeTable(size_t &count)
{
    count = sizeof(c_codes) / sizeof(c_codes[0]);
    return c_codes;
}

const char *phrase(const std::string &code)
{
    for (const Code &entry : c_codes)
        if (code == entry.code)
            return entry.phrase;
    return nullptr;
}

const char *categoryName(char letter)
{
    for (const auto &category : c_categories)
        if (category.letter == letter)
            return category.name;
    return nullptr;
}

const char *severityName(uint8_t severity)
{
    static const char *const names[] = {"MAYDAY", "URGENT", "SAFETY", "ROUTINE"};
    return severity < 4 ? names[severity] : "";
}

bool decode(const std::string &input, Message &out)
{
    // one line ending at the very end does not count
    std::string line = input;
    if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0)
        line.resize(line.size() - 2);
    else if (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();

    if (line.size() < 10 || line.compare(0, 5, "MECP/") != 0 || line[5] < '0' || line[5] > '3' || line[6] != '/')
        return false;

    // every place where the codes could end: behind each code that follows the last by one space
    std::vector<size_t> ends;
    for (size_t at = 7; at + 3 <= line.size() && isCode(line.substr(at, 3));) {
        ends.push_back(at + 3);
        if (at + 3 >= line.size() || line[at + 3] != ' ')
            break;
        at += 4;
    }
    // The longest run of codes that leaves a valid rest wins. "M01 P01  x" has one code and the free text "P01  x".
    size_t codesEnd = 0;
    for (size_t i = ends.size(); i-- > 0;) {
        const size_t end = ends[i];
        if (end == line.size() || (line[end] == ' ' && validFreetext(line, end + 1))) {
            codesEnd = end;
            break;
        }
    }
    if (!codesEnd)
        return false;

    Message message;
    message.severity = (uint8_t)(line[5] - '0');
    for (size_t at = 7; at < codesEnd; at += 4)
        message.codes.push_back(line.substr(at, 3));
    if (codesEnd < line.size())
        message.freetext = line.substr(codesEnd + 1);
    message.drill = std::find(message.codes.begin(), message.codes.end(), "D01") != message.codes.end();

    const std::string &text = message.freetext;
    std::vector<Span> taken;
    Match m;

    if (findFirst(text, peoplePattern, nullptr, m)) {
        message.hasPeople = true;
        message.people = (uint32_t)std::min<uint64_t>(UINT32_MAX, number(text, m.first));
        taken.push_back(m.token);
    }
    const auto onEarth = [&text](const Match &found) {
        const double latitude = strtod(found.first.of(text).c_str(), nullptr);
        const double longitude = strtod(found.second.of(text).c_str(), nullptr);
        return latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
    };
    if (findFirst(text, positionPattern, onEarth, m)) {
        message.hasPosition = true;
        message.latitude = strtod(m.first.of(text).c_str(), nullptr);
        message.longitude = strtod(m.second.of(text).c_str(), nullptr);
        taken.push_back(m.token);
    }
    if (findFirst(text, timePattern, nullptr, m)) {
        message.hasTime = true;
        message.hour = (uint8_t)number(text, m.first);
        message.minute = (uint8_t)number(text, m.second);
        taken.push_back(m.token);
    }
    if (findFirst(text, languagePattern, nullptr, m)) {
        for (const char c : m.first.of(text))
            message.language += c == '_' ? '-' : lower(c);
        taken.push_back(m.token);
    }
    if (findFirst(text, callSignPattern, [](const Match &found) { return found.first.to - found.first.from <= 12; }, m)) {
        for (const char c : m.first.of(text))
            message.callSign += upper(c);
        taken.push_back(m.token);
    }
    if (findFirst(text, referencePattern, nullptr, m)) {
        message.reference = m.first.of(text);
        taken.push_back(m.token);
    }
    if (std::find(message.codes.begin(), message.codes.end(), "R03") != message.codes.end()) {
        // Of the ways with a unit the first by position counts, the earlier way on a tie; a bare number at the
        // start only when none of them is there.
        static const Unit units[] = {Unit::HoursMinutes, Unit::Hours, Unit::Minutes, Unit::Days, Unit::Bare};
        bool have = false;
        Match best;
        Unit bestUnit = Unit::Bare;
        for (const Unit unit : units) {
            if (have && unit == Unit::Bare)
                break;
            const auto valid = [&text, unit](const Match &found) {
                return minutesOf(unit, text, found) > 0 && (unit != Unit::Bare || found.second.empty());
            };
            if (findFirst(text, durationPattern(unit), valid, m, unit == Unit::Bare) && (!have || m.start < best.start)) {
                have = true;
                best = m;
                bestUnit = unit;
            }
        }
        if (have) {
            message.hasDuration = true;
            message.minutes = (uint32_t)minutesOf(bestUnit, text, best);
            taken.push_back(best.token);
        }
    }
    message.note = without(text, taken);
    out = std::move(message);
    return true;
}

// -------- writing --------

std::string cleanCallSign(const std::string &text)
{
    std::string out;
    for (const char raw : text) {
        const char c = upper(raw);
        if (isUpper(c) || isDigit(c))
            out += c;
        else if (c == '/' && !out.empty() && out.back() != '/')
            out += c;
        if (out.size() == 12)
            break;
    }
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

std::string cleanReference(const std::string &text)
{
    std::string out;
    for (const char raw : text) {
        const char c = lower(raw);
        if (isLower(c) || isDigit(c))
            out += c;
        else if ((c == '-' || c == ' ' || c == '_') && !out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out;
}

std::string durationText(uint32_t minutes)
{
    char buf[32];
    if (minutes == 0)
        return "";
    if (minutes % 1440 == 0)
        snprintf(buf, sizeof(buf), "%ud", (unsigned)(minutes / 1440));
    else if (minutes < 60)
        snprintf(buf, sizeof(buf), "%umin", (unsigned)minutes);
    else if (minutes % 60 == 0)
        snprintf(buf, sizeof(buf), "%uh", (unsigned)(minutes / 60));
    else
        snprintf(buf, sizeof(buf), "%uh%umin", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
    return buf;
}

std::string encode(const Draft &draft, bool *cut)
{
    if (cut)
        *cut = false;
    std::string line = "MECP/";
    line += (char)('0' + std::min<uint8_t>(draft.severity, 3));
    line += '/';
    bool eta = false, any = false;
    for (const std::string &code : draft.codes) {
        if (!isCode(code))
            continue;
        if (any)
            line += ' ';
        line += code;
        any = true;
        eta = eta || code == "R03";
    }
    if (!any)
        return "";

    char buf[48];
    const auto add = [&line](const std::string &part) {
        if (!part.empty() && line.size() + 1 + part.size() <= c_maxBytes)
            line += " " + part;
    };
    if (eta)
        add(durationText(draft.minutes));
    if (draft.people) {
        snprintf(buf, sizeof(buf), "%upax", (unsigned)draft.people);
        add(buf);
    }
    if (draft.hasPosition && draft.latitude >= -90 && draft.latitude <= 90 && draft.longitude >= -180 &&
        draft.longitude <= 180) {
        snprintf(buf, sizeof(buf), "%.5f,%.5f", draft.latitude, draft.longitude);
        add(buf);
    }
    if (draft.hasTime && draft.hour < 24 && draft.minute < 60) {
        snprintf(buf, sizeof(buf), "@%02u%02u", (unsigned)draft.hour, (unsigned)draft.minute);
        add(buf);
    }
    const std::string callSign = cleanCallSign(draft.callSign);
    if (!callSign.empty())
        add("~" + callSign);
    const std::string reference = cleanReference(draft.reference);
    if (!reference.empty())
        add("#" + reference);

    // the note: one line, nothing but single spaces between its words, and the only part that gives way
    std::string note;
    for (size_t at = 0; at < draft.note.size();) {
        size_t length;
        const uint32_t cp = codePoint(draft.note, at, length);
        if (isLineBreak(cp) || cp == '\t' || cp == ' ') {
            if (!note.empty() && note.back() != ' ')
                note += ' ';
        } else {
            note.append(draft.note, at, length);
        }
        at += length;
    }
    while (!note.empty() && note.back() == ' ')
        note.pop_back();
    if (!note.empty()) {
        const size_t room = line.size() + 1 < c_maxBytes ? c_maxBytes - line.size() - 1 : 0;
        if (note.size() > room) {
            size_t keep = room;
            while (keep > 0 && ((uint8_t)note[keep] & 0xC0) == 0x80)
                keep--;
            note.resize(keep);
            while (!note.empty() && note.back() == ' ')
                note.pop_back();
            if (cut)
                *cut = true;
        }
        if (!note.empty())
            line += " " + note;
    }
    return line;
}

} // namespace mecp
