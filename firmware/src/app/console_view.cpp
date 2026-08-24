#include "app/console_view.h"

#include <string>
#include <vector>

#include "app/console_font.h"
#include "pure/layout.h"

namespace app {
namespace {

constexpr uint16_t kBackground = 0x0000;
// The question is dimmer than the answer. That is the whole distinction between them: DEVICE_UI's
// rule is that the screen never labels what it is showing, so "you said" and "it said" are told
// apart by weight, not by a word or a prefix character.
constexpr uint16_t kOutgoingColour = 0x8410;  // mid grey
constexpr uint16_t kReplyColour = 0xFFFF;     // white

}  // namespace

void ConsoleView::draw(M5Canvas* canvas, const roboface::Transcript& transcript) const {
    if (canvas == nullptr) return;

    // The face area only. Chrome fills its own bands afterwards, over the top.
    canvas->fillRect(roboface::kFaceLeft, roboface::kFaceTop, roboface::kFaceWidth,
                     roboface::kFaceHeight, kBackground);

    canvas->setFont(&kConsoleFont);
    canvas->setTextDatum(top_left);

    int y = roboface::kFaceTop;
    const int limit = roboface::kFaceTop + roboface::kConsoleLines * roboface::kConsoleLineHeight;

    const auto drawLines = [&](const std::vector<std::string>& lines, uint16_t colour) {
        canvas->setTextColor(colour, kBackground);
        for (const std::string& line : lines) {
            // Stop at the last line that fits rather than trusting the caller's bound. The
            // transcript already limits itself, but the two limits are computed from different
            // things, and the failure mode of disagreeing is text in the chrome band.
            if (y + roboface::kConsoleLineHeight > limit) return;
            canvas->drawString(line.c_str(), roboface::kFaceLeft, y);
            y += roboface::kConsoleLineHeight;
        }
    };

    drawLines(transcript.outgoingLines(), kOutgoingColour);
    drawLines(transcript.replyLines(), kReplyColour);
}

}  // namespace app
