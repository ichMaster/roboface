// M5-012 -- host tests for the frame parser. No board, no display, no radio.
//
// The cases that matter are the three that will happen: a frame cut mid-write, a
// bridge newer than this firmware, and a key this firmware has never heard of. Each
// has to fail in a way the caller can render, rather than in a way that reads past
// the buffer.
//
// Golden frames are read from tests/fixtures/frames/, so the parser is checked against
// exactly what project() produces rather than against hand-written JSON that has
// drifted.

#include "frame.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("  FAIL %s\n", what);
  }
}

std::string read_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};
  std::string out;
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, got);
  std::fclose(file);
  return out;
}

// Takes the literal directly rather than a std::string.
//
// An earlier version took `const std::string&`, which built a temporary that died at
// the end of the call -- leaving every borrowed Value dangling. AddressSanitizer
// caught it on the first run. That is the parser behaving exactly as documented:
// Value borrows and never owns, so the buffer must outlive the Frame. Parsing from a
// static literal keeps that true without the tests having to think about it.
codegen::ParseResult parse(const char* text, codegen::Frame* out) {
  return codegen::parse(text, std::strlen(text), out);
}

// ── the golden frames ────────────────────────────────────────────────────────

void test_every_golden_frame_parses(const std::string& dir) {
  DIR* handle = opendir(dir.c_str());
  if (handle == nullptr) {
    std::printf("  FAIL cannot open %s\n", dir.c_str());
    ++failures;
    return;
  }
  int seen = 0;
  while (dirent* entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (name.size() < 6 || name.substr(name.size() - 5) != ".json") continue;
    // Named local, so it outlives every Value borrowed from it below.
    const std::string text = read_file(dir + "/" + name);
    codegen::Frame frame;
    const codegen::ParseResult result =
        codegen::parse(text.data(), text.size(), &frame);
    if (result != codegen::ParseResult::kOk) {
      std::printf("  FAIL %s -> %s\n", name.c_str(), codegen::describe(result));
      ++failures;
    }
    ++checks;
    check(frame.version == codegen::kFrameVersion, "golden carries the frame version");
    check(frame.screen >= 0, "golden carries a screen id");
    check(frame.next_s > 0, "golden carries a poll interval");
    ++seen;
  }
  closedir(handle);
  check(seen == 9, "nine golden frames were found");
}

// ── the three that will happen ───────────────────────────────────────────────

void test_a_truncated_frame_is_rejected_without_reading_past_the_buffer() {
  const std::string whole =
      R"({"v":1,"s":3,"next":60,"dim":100,"vs":"##############>","done":"14/15"})";
  // Every prefix, so a cut at any byte is covered rather than one convenient one.
  for (std::size_t length = 1; length < whole.size(); ++length) {
    codegen::Frame frame;
    const codegen::ParseResult result =
        codegen::parse(whole.data(), length, &frame);
    if (result == codegen::ParseResult::kOk) {
      std::printf("  FAIL prefix of %zu bytes parsed as complete\n", length);
      ++failures;
    }
    ++checks;
  }
}

void test_a_future_version_is_reported_as_such_rather_than_as_garbage() {
  codegen::Frame frame;
  const codegen::ParseResult result =
      parse(R"({"v":2,"s":1,"next":15,"dim":100})", &frame);
  check(result == codegen::ParseResult::kFutureVersion, "v:2 is a version problem");
  check(std::strcmp(codegen::describe(result), "firmware too old") == 0,
        "the caller can render it");
}

void test_an_unknown_key_is_ignored_so_a_newer_bridge_needs_no_reflash() {
  codegen::Frame frame;
  const codegen::ParseResult result = parse(
      R"({"v":1,"s":3,"next":60,"dim":100,"vs":"###","done":"3/3",)"
      R"("brandnew":{"nested":[1,2,{"deep":"value"}]},"also":42})",
      &frame);
  check(result == codegen::ParseResult::kOk, "unknown keys do not reject the frame");
  check(frame.versions.equals("###"), "known fields still parse around them");
}

// ── fields ───────────────────────────────────────────────────────────────────

void test_notifications_parse_in_order_with_their_volume() {
  codegen::Frame frame;
  const codegen::ParseResult result = parse(
      R"({"v":1,"s":0,"next":5,"dim":100,"n":[)"
      R"({"k":"release","t":"v05.03 tagged","b":0},)"
      R"({"k":"retry","t":"RF-086 x4","b":2,"g":4}]})",
      &frame);
  check(result == codegen::ParseResult::kOk, "notification frame parses");
  check(frame.notification_count == 2, "both notifications are present");
  check(frame.notifications[0].volume == 0, "the silent one is first");
  check(frame.notifications[1].volume == 2, "the alert is second");
  check(frame.notifications[1].goto_screen == 4, "g reaches the caller");
  check(frame.notifications[0].goto_screen == -1, "absent g stays absent");
  check(frame.notifications[1].text.equals("RF-086 x4"), "text is borrowed intact");
}

void test_an_empty_queue_is_the_common_case() {
  codegen::Frame frame;
  check(parse(R"({"v":1,"s":0,"next":5,"dim":100,"n":[]})", &frame) ==
            codegen::ParseResult::kOk,
        "an empty queue parses");
  check(frame.notification_count == 0, "and carries nothing");
}

void test_more_notifications_than_fit_are_dropped_not_written_past_the_array() {
  codegen::Frame frame;
  const codegen::ParseResult result = parse(
      R"({"v":1,"s":0,"next":5,"dim":100,"n":[)"
      R"({"k":"a","t":"1","b":0},{"k":"b","t":"2","b":0},)"
      R"({"k":"c","t":"3","b":0},{"k":"d","t":"4","b":0}]})",
      &frame);
  check(result == codegen::ParseResult::kOk, "an oversized queue still parses");
  check(frame.notification_count == codegen::Frame::kMaxNotifications,
        "the overflow is dropped rather than written past the array");
}

void test_the_same_key_carries_different_types_on_different_screens() {
  // "st" is a word on NOW and a table on ANALYTICS. The frame is routed by "s", so
  // this is unambiguous -- but the type has to be sniffed rather than assumed.
  codegen::Frame now;
  check(parse(R"({"v":1,"s":1,"next":15,"dim":100,"st":"run"})", &now) ==
            codegen::ParseResult::kOk,
        "NOW's st is a word");
  check(now.status.equals("run"), "and reaches the caller");

  codegen::Frame analytics;
  check(parse(R"({"v":1,"s":5,"next":60,"dim":100,"st":[[6,312,41],[7,49,26]],)"
              R"("cov":42})",
              &analytics) == codegen::ParseResult::kOk,
        "ANALYTICS's st is a table");
  check(analytics.coverage == 42, "and the fields after it still parse");
}

void test_a_frame_that_is_not_a_frame_is_malformed_not_truncated() {
  codegen::Frame frame;
  check(parse("", &frame) == codegen::ParseResult::kMalformed, "empty");
  check(parse("[]", &frame) == codegen::ParseResult::kMalformed, "an array");
  check(parse("not json at all", &frame) == codegen::ParseResult::kMalformed, "prose");
  check(parse(R"({"v":1,"next":15})", &frame) == codegen::ParseResult::kMalformed,
        "no screen id to route by");
}

void test_the_frame_is_left_usable_even_when_parsing_fails() {
  // A caller that ignores the result draws an empty screen rather than garbage.
  codegen::Frame frame;
  frame.screen = 5;
  parse("garbage", &frame);
  check(frame.screen == -1, "the struct is reset before parsing");
  check(frame.notification_count == 0, "and carries no stale notifications");
}

void test_copy_to_clamps_and_terminates() {
  codegen::Frame frame;
  parse(R"({"v":1,"s":1,"next":15,"dim":100,"cur":"v05.03 RF-112"})", &frame);
  char small[8];
  const std::size_t written = frame.current.copy_to(small, sizeof(small));
  check(written == sizeof(small) - 1, "copy is clamped to the buffer");
  check(small[sizeof(small) - 1] == '\0', "and NUL terminated");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string fixtures =
      argc > 1 ? argv[1] : "../../tests/fixtures/frames";

  std::printf("frame parser\n");
  test_every_golden_frame_parses(fixtures);
  test_a_truncated_frame_is_rejected_without_reading_past_the_buffer();
  test_a_future_version_is_reported_as_such_rather_than_as_garbage();
  test_an_unknown_key_is_ignored_so_a_newer_bridge_needs_no_reflash();
  test_notifications_parse_in_order_with_their_volume();
  test_an_empty_queue_is_the_common_case();
  test_more_notifications_than_fit_are_dropped_not_written_past_the_array();
  test_the_same_key_carries_different_types_on_different_screens();
  test_a_frame_that_is_not_a_frame_is_malformed_not_truncated();
  test_the_frame_is_left_usable_even_when_parsing_fails();
  test_copy_to_clamps_and_terminates();

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
