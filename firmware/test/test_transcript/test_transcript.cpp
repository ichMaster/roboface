// Host tests for the transcript: wrapping, and accumulating a streamed reply.
//
// The DoD clause these stand behind is "no boxes, no mojibake, no half-characters at a line
// break". A person checks that by looking at the panel; what a laptop can check is the half
// underneath -- that no line this module produces is invalid UTF-8, that a character split across
// two frames is never rendered until it is whole, and that the buffer cannot grow forever.

#include <unity.h>

#include <string>
#include <vector>

#include "pure/transcript.h"

using namespace roboface;

void setUp() {}
void tearDown() {}

// The Ukrainian letters absent from Russian, which a font or a wrap can lose without anyone
// noticing until the exact word that needs them shows up.
static const char* const kUkrainian = "Привіт! Їжак, ґанок,єдність.";

// Is every byte of this string part of a well-formed UTF-8 sequence? This is the assertion the
// whole file exists for: a split character is exactly what produces a box on the panel.
static bool isWellFormedUtf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t len = utf8SequenceLength(static_cast<unsigned char>(text[i]));
        if (len == 0) return false;               // continuation or invalid byte in lead position
        if (i + len > text.size()) return false;  // truncated at the end
        for (std::size_t k = 1; k < len; ++k) {
            if (!isUtf8Continuation(static_cast<unsigned char>(text[i + k]))) return false;
        }
        i += len;
    }
    return true;
}

void test_length_counts_characters_not_bytes() {
    const std::string word = "Привіт!";
    TEST_ASSERT_EQUAL_UINT32(7, utf8Length(word));
    TEST_ASSERT_TRUE(word.size() > utf8Length(word));
}

void test_length_survives_a_malformed_byte() {
    // A lone continuation byte is not a character, but refusing to measure the string would mean
    // refusing to draw a reply that arrived slightly damaged.
    const std::string damaged = std::string("ab") + static_cast<char>(0x80) + "c";
    TEST_ASSERT_EQUAL_UINT32(4, utf8Length(damaged));
}

void test_empty_and_whitespace_produce_no_lines() {
    TEST_ASSERT_EQUAL_UINT32(0, wrapUtf8("", 10).size());
    TEST_ASSERT_EQUAL_UINT32(0, wrapUtf8("     ", 10).size());
    TEST_ASSERT_EQUAL_UINT32(0, wrapUtf8("\t \r", 10).size());
}

void test_zero_columns_produces_no_lines_rather_than_looping() {
    TEST_ASSERT_EQUAL_UINT32(0, wrapUtf8("anything at all", 0).size());
}

void test_a_short_line_stays_one_line() {
    const std::vector<std::string> lines = wrapUtf8("hello there", 20);
    TEST_ASSERT_EQUAL_UINT32(1, lines.size());
    TEST_ASSERT_EQUAL_STRING("hello there", lines[0].c_str());
}

void test_wrapping_breaks_between_words() {
    const std::vector<std::string> lines = wrapUtf8("alpha beta gamma", 11);
    TEST_ASSERT_EQUAL_UINT32(2, lines.size());
    TEST_ASSERT_EQUAL_STRING("alpha beta", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("gamma", lines[1].c_str());
}

void test_no_line_exceeds_the_column_budget() {
    const std::vector<std::string> lines = wrapUtf8(kUkrainian, 10);
    for (const std::string& line : lines) TEST_ASSERT_TRUE(utf8Length(line) <= 10);
}

void test_a_word_longer_than_a_line_is_hard_broken() {
    const std::vector<std::string> lines = wrapUtf8("supercalifragilistic", 6);
    TEST_ASSERT_TRUE(lines.size() >= 4);
    std::string rejoined;
    for (const std::string& line : lines) {
        TEST_ASSERT_TRUE(utf8Length(line) <= 6);
        rejoined += line;
    }
    // Hard-broken, not truncated: every character is still there.
    TEST_ASSERT_EQUAL_STRING("supercalifragilistic", rejoined.c_str());
}

void test_a_hard_break_lands_on_a_character_boundary() {
    // One long Cyrillic word, wrapped narrow enough that the break must fall inside it. A
    // byte-based wrap would split a two-byte letter here and every line would end in a box.
    const std::vector<std::string> lines = wrapUtf8("невідповідальність", 5);
    TEST_ASSERT_TRUE(lines.size() > 1);
    for (const std::string& line : lines) {
        TEST_ASSERT_TRUE(isWellFormedUtf8(line));
        TEST_ASSERT_TRUE(utf8Length(line) <= 5);
    }
}

void test_columns_are_characters_so_cyrillic_fills_a_line_like_latin() {
    // Ten Cyrillic letters is twenty bytes. With columns counted in characters this is one line;
    // counted in bytes it would be two, and the panel would use half the width it has.
    const std::vector<std::string> lines = wrapUtf8("абвгдеєжзи", 10);
    TEST_ASSERT_EQUAL_UINT32(1, lines.size());
}

void test_newline_forces_a_break_and_keeps_a_blank_line() {
    const std::vector<std::string> lines = wrapUtf8("a\n\nb", 10);
    TEST_ASSERT_EQUAL_UINT32(3, lines.size());
    TEST_ASSERT_EQUAL_STRING("a", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("", lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING("b", lines[2].c_str());
}

void test_no_line_carries_a_trailing_space() {
    for (const std::string& line : wrapUtf8("alpha beta gamma delta ", 11)) {
        TEST_ASSERT_TRUE(line.empty() || line.back() != ' ');
    }
}

void test_complete_prefix_holds_back_a_truncated_tail() {
    const std::string letter = "і";  // two bytes
    std::string buffer = "ok";
    buffer += letter[0];  // only the lead byte has arrived
    TEST_ASSERT_EQUAL_UINT32(2, utf8CompletePrefix(buffer));
}

void test_complete_prefix_keeps_a_whole_string() {
    TEST_ASSERT_EQUAL_UINT32(std::string(kUkrainian).size(), utf8CompletePrefix(kUkrainian));
}

void test_complete_prefix_does_not_stall_on_a_malformed_byte() {
    // A byte that is not a lead and not a continuation is wrong rather than in flight; holding it
    // back would freeze the transcript on one bad byte for the rest of the turn.
    const std::string damaged = std::string("ok") + static_cast<char>(0xFF);
    TEST_ASSERT_EQUAL_UINT32(damaged.size(), utf8CompletePrefix(damaged));
}

void test_fragments_produce_the_same_lines_as_the_whole() {
    const std::string whole = kUkrainian;

    Transcript one(12, 9);
    one.startTurn("q");
    one.appendReply(whole);

    // Split at every byte offset, which guarantees some splits land inside a two-byte letter --
    // exactly what a `reply` frame boundary does.
    Transcript piecemeal(12, 9);
    piecemeal.startTurn("q");
    for (std::size_t i = 0; i < whole.size(); ++i) piecemeal.appendReply(whole.substr(i, 1));

    TEST_ASSERT_EQUAL_STRING(one.reply().c_str(), piecemeal.reply().c_str());
    const std::vector<std::string> a = one.replyLines();
    const std::vector<std::string> b = piecemeal.replyLines();
    TEST_ASSERT_EQUAL_UINT32(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) TEST_ASSERT_EQUAL_STRING(a[i].c_str(), b[i].c_str());
}

void test_a_partial_character_is_never_visible() {
    const std::string letter = "ї";
    Transcript t(12, 9);
    t.startTurn("q");
    t.appendReply(letter.substr(0, 1));
    // The lead byte alone must not reach the rendered text.
    TEST_ASSERT_TRUE(isWellFormedUtf8(t.reply()));
    TEST_ASSERT_EQUAL_UINT32(0, t.reply().size());
    t.appendReply(letter.substr(1));
    TEST_ASSERT_TRUE(isWellFormedUtf8(t.reply()));
    TEST_ASSERT_EQUAL_STRING(letter.c_str(), t.reply().c_str());
}

void test_the_transcript_is_bounded() {
    Transcript t(20, 5);
    t.startTurn("question");
    for (int i = 0; i < 500; ++i) t.appendReply("слово ");
    TEST_ASSERT_TRUE(t.lines().size() <= 5);
    TEST_ASSERT_TRUE(t.reply().size() <= kTranscriptMaxReplyBytes);
    TEST_ASSERT_TRUE(isWellFormedUtf8(t.reply()));
}

void test_trimming_the_buffer_leaves_valid_utf8_at_the_front() {
    Transcript t(20, 5);
    t.startTurn("q");
    // Two-byte letters only, so a byte-aligned trim would land mid-character roughly half the time.
    for (int i = 0; i < 400; ++i) t.appendReply("абвгд");
    TEST_ASSERT_TRUE(isWellFormedUtf8(t.reply()));
}

void test_a_new_turn_drops_the_previous_reply() {
    Transcript t(20, 9);
    t.startTurn("first");
    t.appendReply("answer one");
    t.startTurn("second");
    TEST_ASSERT_EQUAL_UINT32(0, t.reply().size());
    TEST_ASSERT_EQUAL_STRING("second", t.outgoing().c_str());
}

void test_a_new_turn_drops_a_character_left_in_flight() {
    // Without clearing the pending bytes, the orphaned lead byte of an abandoned turn would prefix
    // the next reply and corrupt its first character.
    Transcript t(20, 9);
    t.startTurn("first");
    t.appendReply(std::string("ї").substr(0, 1));
    t.startTurn("second");
    t.appendReply("ok");
    TEST_ASSERT_EQUAL_STRING("ok", t.reply().c_str());
    TEST_ASSERT_TRUE(isWellFormedUtf8(t.reply()));
}

void test_clear_empties_everything() {
    Transcript t;
    t.startTurn("q");
    t.appendReply("a");
    TEST_ASSERT_FALSE(t.empty());
    t.clear();
    TEST_ASSERT_TRUE(t.empty());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_length_counts_characters_not_bytes);
    RUN_TEST(test_length_survives_a_malformed_byte);
    RUN_TEST(test_empty_and_whitespace_produce_no_lines);
    RUN_TEST(test_zero_columns_produces_no_lines_rather_than_looping);
    RUN_TEST(test_a_short_line_stays_one_line);
    RUN_TEST(test_wrapping_breaks_between_words);
    RUN_TEST(test_no_line_exceeds_the_column_budget);
    RUN_TEST(test_a_word_longer_than_a_line_is_hard_broken);
    RUN_TEST(test_a_hard_break_lands_on_a_character_boundary);
    RUN_TEST(test_columns_are_characters_so_cyrillic_fills_a_line_like_latin);
    RUN_TEST(test_newline_forces_a_break_and_keeps_a_blank_line);
    RUN_TEST(test_no_line_carries_a_trailing_space);
    RUN_TEST(test_complete_prefix_holds_back_a_truncated_tail);
    RUN_TEST(test_complete_prefix_keeps_a_whole_string);
    RUN_TEST(test_complete_prefix_does_not_stall_on_a_malformed_byte);
    RUN_TEST(test_fragments_produce_the_same_lines_as_the_whole);
    RUN_TEST(test_a_partial_character_is_never_visible);
    RUN_TEST(test_the_transcript_is_bounded);
    RUN_TEST(test_trimming_the_buffer_leaves_valid_utf8_at_the_front);
    RUN_TEST(test_a_new_turn_drops_the_previous_reply);
    RUN_TEST(test_a_new_turn_drops_a_character_left_in_flight);
    RUN_TEST(test_clear_empties_everything);
    return UNITY_END();
}
