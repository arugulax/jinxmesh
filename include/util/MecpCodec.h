#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * MECP, the Mesh Emergency Communication Protocol, version 1.4.0 (https://github.com/mecp-org/MECP).
 * A message is one line of text, "MECP/<severity>/<codes> <free text>": any mesh client carries it and every
 * client shows it in its own language. Written from the specification (CC0); the English phrases of the codes
 * are those of the project's languages/en.json (CC BY 4.0).
 *
 * No display code in here: the unit tests run the specification's conformance cases against decode().
 */
namespace mecp
{

constexpr size_t c_maxBytes = 200; // of a message that is written; one that is read may be longer

enum Severity : uint8_t { Mayday = 0, Urgent = 1, Safety = 2, Routine = 3 };

struct Message {
    uint8_t severity = Routine;
    std::vector<std::string> codes; // as sent; a well formed code that is not in the table stays one
    std::string freetext;           // everything behind the codes, untouched

    // details found in the free text, each the first valid one by position
    bool hasPeople = false;
    uint32_t people = 0;
    bool hasPosition = false;
    double latitude = 0, longitude = 0;
    bool hasTime = false; // the sender's local time
    uint8_t hour = 0, minute = 0;
    std::string language;  // lowercase, "-" between the parts
    std::string callSign;  // uppercase
    std::string reference; // as written
    bool hasDuration = false; // only looked for with code R03
    uint32_t minutes = 0;

    bool drill = false; // carries D01
    std::string note;   // the free text without the details above
};

/**
 * Reads a line. False when it is not an MECP message; a message is never refused for its length.
 */
bool decode(const std::string &line, Message &out);

/**
 * True for a letter and two digits.
 */
bool isCode(const std::string &text);

/**
 * The English phrase of a code, or nullptr when the table does not have it.
 */
const char *phrase(const std::string &code);

/**
 * The name of a category ('M' is "Medical"), or nullptr.
 */
const char *categoryName(char letter);

/**
 * The severity in a word: "MAYDAY", "URGENT", "SAFETY", "ROUTINE".
 */
const char *severityName(uint8_t severity);

/**
 * The code table in the order of the specification.
 */
struct Code {
    const char *code;
    const char *phrase;
};
const Code *codeTable(size_t &count);

/**
 * What a message is written from. Values that cannot be written are left out, they never spoil the message.
 */
struct Draft {
    uint8_t severity = Routine;
    std::vector<std::string> codes;
    uint32_t people = 0; // 0: not given
    bool hasPosition = false;
    double latitude = 0, longitude = 0;
    bool hasTime = false;
    uint8_t hour = 0, minute = 0;
    std::string callSign;
    std::string reference;
    uint32_t minutes = 0; // written only with code R03
    std::string note;
};

/**
 * The line to send, at most c_maxBytes long. Only the note is ever shortened to make it fit; cut says so.
 * Empty when there is no code.
 */
std::string encode(const Draft &draft, bool *cut = nullptr);

/**
 * A call sign as it may be written: uppercase letters, digits and single slashes, at most 12. Empty if nothing is left.
 */
std::string cleanCallSign(const std::string &text);

/**
 * A reference as it may be written: lowercase letters, digits and single hyphens.
 */
std::string cleanReference(const std::string &text);

/**
 * A duration as it is written: "45min", "1h30min", "2d".
 */
std::string durationText(uint32_t minutes);

} // namespace mecp
