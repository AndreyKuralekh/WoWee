#pragma once

// The strings in SMSG_QUEST_QUERY_RESPONSE, and which of them is which.
//
// The response carries a run of C-strings after its numeric block - Title,
// Objectives, Details, EndText, CompletedText - and this client reads them at
// an offset it has to work out. So every read is checked for being text at all
// rather than trusted, and the two that can hold the "what to do now" line are
// chosen between rather than merged.
//
// Header-only and free of the packet reader so it can be tested: the choice
// between two strings is the part that was getting answered wrong, and it is
// pure.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::game {

/// The code point of the well-formed UTF-8 sequence that starts at s[i], and
/// its length in bytes; a length of 0 when no well-formed sequence starts
/// there (a stray continuation byte, a cut-off or overlong sequence).
inline uint32_t questTextCodePoint(const std::string& s, size_t i, size_t& length) {
    length = 0;
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        length = 1;
        return lead;
    }
    size_t more = 0;
    uint32_t cp = 0;
    if ((lead & 0xE0) == 0xC0) { more = 1; cp = lead & 0x1F; }
    else if ((lead & 0xF0) == 0xE0) { more = 2; cp = lead & 0x0F; }
    else if ((lead & 0xF8) == 0xF0) { more = 3; cp = lead & 0x07; }
    else return 0;
    if (i + more >= s.size()) return 0;
    for (size_t k = 1; k <= more; ++k) {
        const unsigned char c = byte(i + k);
        if ((c & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (c & 0x3F);
    }
    constexpr uint32_t kLeast[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < kLeast[more] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    length = more + 1;
    return cp;
}

/// +1 for an upper-case letter, -1 for a lower-case one, 0 for anything else -
/// for the alphabets the game's locales write in: Latin with its accents,
/// Greek, Cyrillic; Hangul and the CJK ideographs are letters without case.
/// `letter` says whether it is a letter at all.
inline int questTextLetterCase(uint32_t cp, bool& letter) {
    letter = true;
    if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z') return 1;
        if (cp >= 'a' && cp <= 'z') return -1;
        letter = false;
        return 0;
    }
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return 1;    // Latin-1 capitals
    if (cp >= 0xDF && cp <= 0xFF && cp != 0xF7) return -1;   // Latin-1 small
    if (cp >= 0x100 && cp <= 0x24F) return (cp % 2 == 0) ? 1 : -1;  // Latin Extended, mostly in pairs
    if (cp >= 0x391 && cp <= 0x3AB) return 1;                // Greek capitals
    if (cp >= 0x3AC && cp <= 0x3CE) return -1;               // Greek small
    if (cp >= 0x400 && cp <= 0x42F) return 1;                // Cyrillic capitals, Ё and the rest
    if (cp >= 0x430 && cp <= 0x45F) return -1;               // Cyrillic small, ё and the rest
    if (cp >= 0x460 && cp <= 0x52F) return (cp % 2 == 0) ? 1 : -1;  // Cyrillic Supplement, in pairs
    if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0x3040 && cp <= 0x30FF)) {
        return 0;  // Hangul, CJK, kana
    }
    letter = false;
    return 0;
}

/// The case of a string's first letter, as questTextLetterCase; 0 when the
/// string does not start with a letter.
inline int questTextFirstLetterCase(const std::string& s) {
    if (s.empty()) return 0;
    size_t length = 0;
    const uint32_t cp = questTextCodePoint(s, 0, length);
    if (length == 0) return 0;
    bool letter = false;
    return questTextLetterCase(cp, letter);
}

/// Whether a string read out of the packet is text rather than the numbers on
/// either side of it: inside the length the field allows, free of control
/// characters, well-formed UTF-8, and carrying at least one letter.
///
/// A letter in any of the locales' alphabets: a Russian title has no ASCII
/// letter in it at all, and asking for one turned every quest on a ruRU realm
/// into "Quest #15". Bytes over 0x7F must make whole UTF-8 sequences, which a
/// run of packet numbers almost never does - that is what separates a sentence
/// from them now.
inline bool questTextIsReadable(const std::string& s, size_t minLen, size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) return false;
    bool hasLetter = false;
    for (size_t i = 0; i < s.size();) {
        size_t length = 0;
        const uint32_t cp = questTextCodePoint(s, i, length);
        if (length == 0) return false;
        if (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') return false;
        bool letter = false;
        questTextLetterCase(cp, letter);
        if (letter) hasLetter = true;
        i += length;
    }
    return hasLetter;
}

/// The line a finished quest shows, out of the two strings that can hold one.
///
/// CompletedText is the fifth string and is what the game shows for a quest
/// that is done. EndText is the fourth, and on the clients that have no fifth
/// it is where "Return to Marshal Dughan at Goldshire in Elwynn Forest." lives.
/// So the fifth wins whenever it says anything and the fourth stands in when it
/// does not.
///
/// This used to take whichever of the two was longer, which is a different
/// question: a quest carrying both got its EndText wherever that ran longer
/// than its CompletedText, and neither read as text left the line blank rather
/// than falling back.
inline std::string questCompletedText(const std::string& endText,
                                      const std::string& completedText) {
    // The same bounds the reader uses for a one-line string: long enough to be
    // a sentence, short enough that a run of packet bytes cannot pass.
    constexpr size_t kMin = 8;
    constexpr size_t kMax = 600;
    if (questTextIsReadable(completedText, kMin, kMax)) return completedText;
    if (questTextIsReadable(endText, kMin, kMax)) return endText;
    return {};
}

} // namespace wowee::game
