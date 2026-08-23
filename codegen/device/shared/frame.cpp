#include "frame.h"

namespace codegen {
namespace {

struct Scanner {
  const char* data;
  std::size_t size;
  std::size_t at = 0;

  bool done() const { return at >= size; }
  char peek() const { return at < size ? data[at] : '\0'; }
  bool take(char expected) {
    if (peek() != expected) return false;
    ++at;
    return true;
  }
  void skip_spaces() {
    while (at < size && (data[at] == ' ' || data[at] == '\n' || data[at] == '\t')) ++at;
  }
};

// A quoted string. Returns false on an unterminated one, which is what a frame cut
// mid-write looks like -- and the reason every loop here is bounded by `size`.
bool read_string(Scanner& s, Value* out) {
  if (!s.take('"')) return false;
  const std::size_t start = s.at;
  while (s.at < s.size && s.data[s.at] != '"') {
    // The bridge guarantees ASCII with no escapes (a test asserts it), so a backslash
    // means the frame is not one of ours.
    if (s.data[s.at] == '\\') return false;
    ++s.at;
  }
  if (s.at >= s.size) return false;
  if (out != nullptr) {
    out->data = s.data + start;
    out->size = s.at - start;
  }
  ++s.at;
  return true;
}

bool read_int(Scanner& s, int* out) {
  bool negative = s.take('-');
  if (s.done() || s.peek() < '0' || s.peek() > '9') return false;
  long value = 0;
  while (!s.done() && s.peek() >= '0' && s.peek() <= '9') {
    value = value * 10 + (s.data[s.at] - '0');
    if (value > 1000000) return false;  // nothing legitimate is this large
    ++s.at;
  }
  if (out != nullptr) *out = static_cast<int>(negative ? -value : value);
  return true;
}

// Steps over any value without interpreting it, so an unknown key costs nothing.
bool skip_value(Scanner& s, int depth = 0) {
  if (depth > 4) return false;
  s.skip_spaces();
  const char c = s.peek();
  if (c == '"') return read_string(s, nullptr);
  if (c == '-' || (c >= '0' && c <= '9')) return read_int(s, nullptr);
  if (c == '[' || c == '{') {
    const char close = (c == '[') ? ']' : '}';
    ++s.at;
    while (!s.done()) {
      s.skip_spaces();
      if (s.take(close)) return true;
      if (s.take(',') || s.take(':')) continue;
      if (!skip_value(s, depth + 1)) return false;
    }
    return false;
  }
  // true / false / null: consume the bare word.
  const std::size_t start = s.at;
  while (!s.done() && s.peek() >= 'a' && s.peek() <= 'z') ++s.at;
  return s.at > start;
}

bool key_is(const Value& key, const char* name) { return key.equals(name); }

bool read_notifications(Scanner& s, Frame* out) {
  if (!s.take('[')) return false;
  out->notification_count = 0;
  while (!s.done()) {
    s.skip_spaces();
    if (s.take(']')) return true;
    if (s.take(',')) continue;
    if (!s.take('{')) return false;

    Frame::Notification item;
    while (!s.done()) {
      s.skip_spaces();
      if (s.take('}')) break;
      if (s.take(',')) continue;
      Value key;
      if (!read_string(s, &key)) return false;
      s.skip_spaces();
      if (!s.take(':')) return false;
      s.skip_spaces();
      if (key_is(key, "k")) {
        if (!read_string(s, &item.kind)) return false;
      } else if (key_is(key, "t")) {
        if (!read_string(s, &item.text)) return false;
      } else if (key_is(key, "b")) {
        if (!read_int(s, &item.volume)) return false;
      } else if (key_is(key, "g")) {
        if (!read_int(s, &item.goto_screen)) return false;
      } else if (!skip_value(s)) {
        return false;
      }
    }
    // A burst larger than the frame can hold is impossible by construction, but
    // dropping the overflow is better than writing past the array.
    if (out->notification_count < Frame::kMaxNotifications) {
      out->notifications[out->notification_count++] = item;
    }
  }
  return false;
}

}  // namespace

bool Value::equals(const char* other) const {
  if (other == nullptr) return false;
  std::size_t index = 0;
  for (; index < size; ++index) {
    if (other[index] == '\0' || other[index] != data[index]) return false;
  }
  return other[index] == '\0';
}

std::size_t Value::copy_to(char* out, std::size_t capacity) const {
  if (out == nullptr || capacity == 0) return 0;
  const std::size_t count = (size < capacity - 1) ? size : capacity - 1;
  for (std::size_t index = 0; index < count; ++index) out[index] = data[index];
  out[count] = '\0';
  return count;
}

ParseResult parse(const char* data, std::size_t size, Frame* out) {
  if (out == nullptr) return ParseResult::kMalformed;
  *out = Frame{};
  if (data == nullptr || size == 0) return ParseResult::kMalformed;

  Scanner s{data, size};
  s.skip_spaces();
  if (!s.take('{')) return ParseResult::kMalformed;

  bool closed = false;
  while (!s.done()) {
    s.skip_spaces();
    if (s.take('}')) {
      closed = true;
      break;
    }
    if (s.take(',')) continue;

    Value key;
    if (!read_string(s, &key)) return ParseResult::kTruncated;
    s.skip_spaces();
    if (!s.take(':')) return ParseResult::kMalformed;
    s.skip_spaces();

    bool ok = true;
    if (key_is(key, "v")) ok = read_int(s, &out->version);
    else if (key_is(key, "s")) ok = read_int(s, &out->screen);
    else if (key_is(key, "next")) ok = read_int(s, &out->next_s);
    else if (key_is(key, "dim")) ok = read_int(s, &out->dim);
    else if (key_is(key, "g")) ok = read_int(s, &out->goto_screen);
    else if (key_is(key, "cc")) ok = read_int(s, &out->colour_class);
    else if (key_is(key, "pct")) ok = read_int(s, &out->percent);
    else if (key_is(key, "cov")) ok = read_int(s, &out->coverage);
    else if (key_is(key, "st")) {
      // ANALYTICS carries a table here and NOW carries a word. Same key, different
      // screens -- the frame is routed by `s`, so this is unambiguous, but it does
      // mean the type has to be sniffed rather than assumed.
      ok = (s.peek() == '"') ? read_string(s, &out->status) : skip_value(s);
    }
    else if (key_is(key, "cur")) ok = read_string(s, &out->current);
    else if (key_is(key, "stp")) ok = read_string(s, &out->step);
    else if (key_is(key, "el")) {
      ok = (s.peek() == '"') ? read_string(s, &out->elapsed) : skip_value(s);
    }
    else if (key_is(key, "ct")) ok = read_string(s, &out->issue_age);
    else if (key_is(key, "eta")) ok = read_string(s, &out->eta);
    else if (key_is(key, "idit")) ok = read_string(s, &out->progress);
    else if (key_is(key, "sp")) ok = read_string(s, &out->spark);
    else if (key_is(key, "vs")) ok = read_string(s, &out->versions);
    else if (key_is(key, "bd")) ok = read_string(s, &out->burndown);
    else if (key_is(key, "n")) ok = read_notifications(s, out);
    else ok = skip_value(s);  // unknown key: a newer bridge must not force a reflash

    if (!ok) return ParseResult::kTruncated;
  }

  if (!closed) return ParseResult::kTruncated;
  if (out->screen < 0) return ParseResult::kMalformed;
  if (out->version != kFrameVersion) return ParseResult::kFutureVersion;
  return ParseResult::kOk;
}

const char* describe(ParseResult result) {
  switch (result) {
    case ParseResult::kOk: return "ok";
    case ParseResult::kMalformed: return "malformed";
    case ParseResult::kTruncated: return "truncated";
    case ParseResult::kFutureVersion: return "firmware too old";
  }
  return "unknown";
}

}  // namespace codegen
