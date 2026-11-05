// Which of a quest's strings holds the line a finished quest shows.
//
// SMSG_QUEST_QUERY_RESPONSE carries Title, Objectives, Details, EndText and -
// since 3.3.0 - CompletedText. Two of those can hold "what to do now", and the
// rule used to be "whichever is longer", which is a different question: a quest
// carrying both got the wrong one whenever the wrong one ran longer, and a
// quest whose fifth string was empty got a blank line rather than its fourth.
#include <catch_amalgamated.hpp>

#include "game/quest_text.hpp"

#include <string>

using wowee::game::questCompletedText;
using wowee::game::questTextIsReadable;

TEST_CASE("the fifth string wins when it says anything", "[quest][text]") {
    CHECK(questCompletedText("Return to Marshal Dughan at Goldshire in Elwynn Forest.",
                             "Speak with Marshal Dughan.") ==
          "Speak with Marshal Dughan.");
}

TEST_CASE("the fourth stands in when the fifth is empty", "[quest][text]") {
    // Which is every quest on a client that has no fifth string, and most
    // vanilla-era quests on one that does.
    CHECK(questCompletedText("Return to Bath'rah the Windwatcher.", "") ==
          "Return to Bath'rah the Windwatcher.");
}

TEST_CASE("a quest with neither has no completed line", "[quest][text]") {
    CHECK(questCompletedText("", "").empty());
}

TEST_CASE("packet bytes are not a completed line", "[quest][text]") {
    // The strings are read at an offset this client works out, so a read that
    // lands on the numbers after them must be dropped rather than shown.
    const std::string binary("\x01\x02\x00\x40\x05\x00\x00\x00\x0f", 9);
    CHECK(questCompletedText(binary, binary).empty());
    // Nor is a run of digits with no letter in it.
    CHECK_FALSE(questTextIsReadable("12345678", 8, 600));
    // And a sentence shorter than the minimum is not one either - the bound is
    // what keeps a stray two-byte read out of the tracker.
    CHECK_FALSE(questTextIsReadable("Go.", 8, 600));
}

TEST_CASE("a localized line is text", "[quest][text]") {
    // UTF-8 lead and continuation bytes are allowed.
    CHECK(questTextIsReadable("R\xc3\xbc""ckkehr zu Marschall Dughan.", 8, 600));
}

TEST_CASE("a title with no Latin letter in it is text", "[quest][text]") {
    // "Волки за рекой" - a ruRU realm's quests have no ASCII letter at all, and
    // asking for one showed every quest as "Quest #15".
    const std::string wolves = "\xd0\x92\xd0\xbe\xd0\xbb\xd0\xba\xd0\xb8 \xd0\xb7\xd0\xb0 "
                               "\xd1\x80\xd0\xb5\xd0\xba\xd0\xbe\xd0\xb9";
    CHECK(questTextIsReadable(wolves, 6, 72));
    CHECK(wowee::game::questTextFirstLetterCase(wolves) > 0);
    CHECK(wowee::game::questTextFirstLetterCase("\xd0\xb2\xd0\xbe\xd0\xbb\xd0\xba") < 0);  // "волк"
}

TEST_CASE("high bytes that are not UTF-8 are not text", "[quest][text]") {
    // A stray continuation byte, and a lead byte with nothing after it.
    CHECK_FALSE(questTextIsReadable("Abcdef\x80ghij", 8, 600));
    CHECK_FALSE(questTextIsReadable("Abcdefghij\xd0", 8, 600));
}
