// Frame parser — the whole of device/shared/.
//
// Vision section 3.1 left nothing else to share. The alert state machine went to the
// bridge, the staleness timer became a radio event, the sparkline buffer became a
// finished series in the frame. What remains is turning bytes into a struct.
//
// No display, no radio, no board: this compiles and runs on the host, which is what
// makes the one genuinely fiddly part of the firmware testable in CI.
//
// Deliberately small. A JSON library on an ESP32 is a heap allocator and a parser to
// audit; a frame is flat, ASCII-only, under 182 bytes and validated by the bridge
// before it is sent, so a scanner over the buffer is enough. Nothing here allocates.

#ifndef CODEGEN_DEVICE_SHARED_FRAME_H
#define CODEGEN_DEVICE_SHARED_FRAME_H

#include <cstddef>
#include <cstdint>

namespace codegen {

// Frame schema this firmware understands. Unrelated to the event log's `v`
// (architecture section 11.1): different wire, different owner, independent evolution.
constexpr int kFrameVersion = 1;

// Longest value the panel will ever draw. The bridge truncates to the screen's budget,
// so this is a ceiling rather than a working size.
constexpr std::size_t kMaxValue = 64;

enum class ParseResult : std::uint8_t {
  kOk = 0,
  kMalformed,      // not a frame at all
  kTruncated,      // cut mid-write; never read past the buffer
  kFutureVersion,  // a bridge newer than this firmware -- say so, do not guess
};

// One string field, borrowed from the caller's buffer. No allocation, no ownership.
//
// **The buffer must outlive the Frame.** Nothing here copies, so parsing from a
// temporary leaves every Value dangling -- which is not theoretical: it is the first
// thing AddressSanitizer caught when the tests did exactly that. Use `copy_to` if a
// value has to survive the buffer it came from.
struct Value {
  const char* data = nullptr;
  std::size_t size = 0;

  bool empty() const { return size == 0; }
  bool equals(const char* other) const;
  // Copies into `out` with a NUL terminator, clamped. Returns the characters written.
  std::size_t copy_to(char* out, std::size_t capacity) const;
};

// A parsed frame. Screens read the fields they need and ignore the rest, which is why
// an unknown key is skipped rather than rejected: a bridge adding one must not force a
// reflash.
struct Frame {
  int version = 0;
  int screen = -1;
  int next_s = 0;
  int dim = 100;
  int goto_screen = -1;  // -1 when absent. Never null on the wire.

  // Present only on the screens that carry them; empty otherwise.
  Value status;
  Value current;
  Value step;
  Value elapsed;
  Value issue_age;
  Value eta;
  Value progress;  // "42/46"
  int colour_class = 0;
  int percent = 0;

  Value spark;    // digits 0-7, one per bucket
  Value versions; // one ASCII flag per version
  Value burndown; // two digits per sample

  int coverage = -1;

  // Notifications, in the order the bridge queued them.
  static constexpr std::size_t kMaxNotifications = 3;
  struct Notification {
    Value kind;
    Value text;
    int volume = 0;
    int goto_screen = -1;
  };
  Notification notifications[kMaxNotifications];
  std::size_t notification_count = 0;
};

// Parses `data[0..size)` into `out`.
//
// `out` is left in a defined state on every path, so a caller that ignores the result
// draws an empty screen rather than garbage.
ParseResult parse(const char* data, std::size_t size, Frame* out);

// Human-readable, for the "firmware too old" screen and for test failures.
const char* describe(ParseResult result);

}  // namespace codegen

#endif  // CODEGEN_DEVICE_SHARED_FRAME_H
