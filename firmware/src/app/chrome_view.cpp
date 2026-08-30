#include "app/chrome_view.h"

#include "pure/level.h"

#include <algorithm>

namespace app {
namespace {

constexpr uint16_t kBandBackground = 0x0000;
constexpr uint16_t kIndicatorColour = 0x07FF;
constexpr uint16_t kAmber = 0xFD20;
constexpr uint16_t kFaultColour = 0xF800;
//: A dim rail so the meter is visible as a meter even in a silent room -- an unlit band and a band
//: with no meter at all look the same, and only one of them is correct.
constexpr uint16_t kMeterIdleColour = 0x2124;
constexpr int kMeterMargin = 8;
constexpr int kMeterMinHeight = 4;
constexpr int kMeterMaxHeight = 18;

// The status cluster, laid out right to left from the documented anchor.
constexpr int kBatteryWidth = 22;
constexpr int kBatteryHeight = 11;
constexpr int kLinkWidth = 16;
constexpr int kClusterGap = 8;

uint16_t withAlpha(uint16_t colour565, uint8_t alpha) {
    // The fade, done by scaling toward the background rather than with real alpha: the sprite is
    // opaque 16-bit and the band behind is black, so this is the same result for a fraction of the
    // cost -- and cost matters on a panel redrawn every loop.
    const float keep = alpha / 255.0f;
    const int r = static_cast<int>(((colour565 >> 11) & 0x1F) * keep);
    const int g = static_cast<int>(((colour565 >> 5) & 0x3F) * keep);
    const int b = static_cast<int>((colour565 & 0x1F) * keep);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// How far through a fade the indicator is, from the tested timings in pure/chrome.h. No literals
// here: DEVICE_UI's 120 ms in / 400 ms out live in one place and this reads them.
uint8_t fadeAlpha(bool visible, uint32_t settled_for_ms) {
    if (visible) {
        const uint32_t t = std::min(settled_for_ms, roboface::kChromeFadeInMs);
        return static_cast<uint8_t>(255u * t / roboface::kChromeFadeInMs);
    }
    if (settled_for_ms >= roboface::kSettleHideMs + roboface::kChromeFadeOutMs) return 0;
    if (settled_for_ms <= roboface::kSettleHideMs) return 255;
    const uint32_t into = settled_for_ms - roboface::kSettleHideMs;
    return static_cast<uint8_t>(255u - (255u * into / roboface::kChromeFadeOutMs));
}

}  // namespace

void ChromeView::draw(M5Canvas* canvas, const roboface::Chrome& chrome, float level) {
    if (canvas == nullptr) return;

    const roboface::ChromeVisibility shown = chrome.visibility();
    const uint32_t settled = chrome.settledForMs();

    // Clear the two bands only. The face safe area belongs to the renderer, and clearing it here
    // would erase the face every frame.
    static_assert(roboface::clearOfFace(0, 0, roboface::kScreenWidth, roboface::kBandHeight),
                  "the top band must not overlap the face");
    static_assert(roboface::clearOfFace(0, roboface::kFaceBottom, roboface::kScreenWidth,
                                        roboface::kBandHeight),
                  "the bottom band must not overlap the face");
    canvas->fillRect(0, 0, roboface::kScreenWidth, roboface::kBandHeight, kBandBackground);
    canvas->fillRect(0, roboface::kFaceBottom, roboface::kScreenWidth, roboface::kBandHeight,
                     kBandBackground);

    const uint8_t alpha = fadeAlpha(shown.link, settled);
    if (alpha > 0) drawLink(*canvas, chrome.link(), alpha);
    if (shown.battery) {
        drawBattery(*canvas, chrome.batteryPercent(), chrome.charging(), 255);
    }

    // **Always drawn, in both states, with no fade** -- and it is the one indicator that is also a
    // control (`pure/layout.h` §kMicButtonLeft). DEVICE_UI already exempted a *muted* microphone
    // from fading; a button has to go further, because a control that fades out is a control nobody
    // can find, and mute is reached for in a hurry.
    drawMicButton(*canvas, shown.mic_muted);

    if (shown.band == roboface::BandTenant::kFault) drawFaultLine(*canvas, chrome.fault());
    if (shown.band == roboface::BandTenant::kLevel) drawLevelMeter(*canvas, level);
}

void ChromeView::drawLink(M5Canvas& canvas, roboface::LinkState state, uint8_t alpha) {
    const int right = roboface::kStatusClusterRight;
    const int top = roboface::kStatusClusterTop;
    const uint16_t colour =
        withAlpha(state == roboface::LinkState::kConnected ? kIndicatorColour : kAmber, alpha);

    // Three arcs, smallest first. `offline` crosses them out instead -- DEVICE_UI §Indicators.
    const int cx = right - kLinkWidth / 2;
    const int cy = top + roboface::kGlyphSize;
    for (int arc = 1; arc <= 3; ++arc) {
        canvas.drawArc(cx, cy, arc * 4, arc * 4 - 2, 210, 330, colour);
    }
    if (state == roboface::LinkState::kOffline) {
        canvas.drawWideLine(cx - 8, cy - 8, cx + 8, cy + 2, 2, withAlpha(kAmber, alpha));
    }
}

void ChromeView::drawMicButton(M5Canvas& canvas, bool muted) {
    // A microphone glyph in the top-left corner: capsule, stand, base -- plus a slash when muted.
    //
    // **Amber when muted, the indicator cyan when live.** Not red either way: mute is a state
    // someone chose, not a fault, and this repo spends red on `kFaultColour` alone. The colour is
    // the only thing that changes between the two states besides the slash, so the button reads at
    // a glance without reading as an alarm.
    const int x = roboface::kMicButtonLeft;
    const int y = roboface::kMicButtonTop;
    const int w = roboface::kMicButtonWidth;
    const int h = roboface::kMicButtonHeight;
    const uint16_t colour = muted ? kAmber : kIndicatorColour;

    // The **glyph**, not the touch target: the target reaches into the face on purpose and this
    // assert would be wrong to cover it. What DEVICE_UI forbids is drawing over the face.
    static_assert(roboface::clearOfFace(roboface::kMicButtonLeft, roboface::kMicButtonTop,
                                        roboface::kMicButtonWidth, roboface::kMicButtonHeight),
                  "the microphone glyph must not overlap the face");

    // The capsule -- filled when live, outlined when muted, so the two differ in weight as well as
    // in hue. A person who cannot pick cyan from amber can still tell a solid shape from a hollow.
    const int capsule_w = w - 6;
    const int capsule_h = h - 6;
    if (muted) {
        canvas.drawRoundRect(x + 3, y, capsule_w, capsule_h, capsule_w / 2, colour);
    } else {
        canvas.fillRoundRect(x + 3, y, capsule_w, capsule_h, capsule_w / 2, colour);
    }

    // The cradle under it, then the stand down to the base.
    canvas.drawFastHLine(x + 1, y + h - 5, w - 2, colour);
    canvas.drawFastVLine(x + w / 2, y + h - 5, 4, colour);
    canvas.drawFastHLine(x + w / 2 - 3, y + h - 1, 7, colour);

    if (muted) canvas.drawLine(x, y + h, x + w, y - 1, kAmber);
}

void ChromeView::drawBattery(M5Canvas& canvas, int percent, bool charging, uint8_t alpha) {
    const int x = roboface::kStatusClusterRight - kLinkWidth - kClusterGap - kBatteryWidth;
    const int y = roboface::kStatusClusterTop + 2;
    const uint16_t colour = withAlpha(charging ? kIndicatorColour : kAmber, alpha);

    canvas.drawRoundRect(x, y, kBatteryWidth, kBatteryHeight, 2, colour);
    canvas.fillRect(x + kBatteryWidth, y + 3, 2, kBatteryHeight - 6, colour);
    const int fill = std::clamp((kBatteryWidth - 4) * percent / 100, 0, kBatteryWidth - 4);
    if (fill > 0) canvas.fillRect(x + 2, y + 2, fill, kBatteryHeight - 4, colour);
}

void ChromeView::drawFaultLine(M5Canvas& canvas, roboface::ErrorCode code) {
    // The enumerated code **verbatim**. DEVICE_UI §Screens: it is the one string worth typing into
    // a search, and it is the exception to "no words on screen" precisely because it is not a
    // state -- it is an identifier. `kUnknown` still prints something searchable rather than a
    // blank band.
    canvas.setTextColor(kFaultColour, kBandBackground);
    // Set the face explicitly rather than inheriting whatever drew last. Nothing used to set a font
    // at all, so once v0.5's console had drawn its 20 px Cyrillic face into this sprite, the fault
    // code rendered in it too -- in a 28 px band, and for the rest of the session, because the
    // sprite keeps the setting. (v0.5 review, finding 2.)
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(middle_left);
    canvas.drawString(roboface::toString(code), 8,
                      roboface::kFaceBottom + roboface::kBandHeight / 2);
}

void ChromeView::drawLevelMeter(M5Canvas& canvas, float level) {
    // The only signal that the device is listening. DEVICE_UI and MISSION both say a device state
    // is a face or an indicator and never a word, so **no text is drawn here** -- not "listening",
    // not a dB figure, nothing. The bar is the message.
    constexpr int kBars = 16;
    constexpr int kGap = 2;
    const int span = roboface::kScreenWidth - 2 * kMeterMargin;
    const int bar_width = (span - (kBars - 1) * kGap) / kBars;
    const std::size_t lit = roboface::barsForLevel(level, kBars);

    const int base = roboface::kFaceBottom + roboface::kBandHeight - kMeterMargin;
    for (int index = 0; index < kBars; ++index) {
        // Height rises across the band so a quiet room still shows the meter *exists* -- an
        // all-dark band and a band with no meter look identical, and one of them is a fault.
        const int height = kMeterMinHeight +
                           (kMeterMaxHeight - kMeterMinHeight) * index / (kBars - 1);
        const int x = kMeterMargin + index * (bar_width + kGap);
        const uint16_t colour =
            static_cast<std::size_t>(index) < lit ? kIndicatorColour : kMeterIdleColour;
        canvas.fillRect(x, base - height, bar_width, height, colour);
    }
}

}  // namespace app
