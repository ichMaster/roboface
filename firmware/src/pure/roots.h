// A square root that works over the whole range, without <cmath>.
//
// The pure layer cannot include <cmath>: it compiles for the host and for an ESP32-S3, and a
// threshold tested on a laptop has to be the threshold that runs on the board. So the roots here are
// computed by Newton's method, which every earlier header in this project wrote out for itself.
//
// **Those private copies hid a real bug, and v2.5's tests found it.** Newton converges from any
// positive start, but it converges *slowly* from a bad one -- each iteration roughly halves the
// exponent's error, so a start of 1.7e20 needs about thirty-five iterations to reach 1.3e10. With
// the customary "32 iterations, break when it stops moving" that loop silently returns a number
// several orders of magnitude wrong, and `coherence()` -- whose input is a product of two energies,
// and so routinely 1e20 -- reported 0.6 for two identical channels.
//
// The fix is not more iterations. It is a **decent starting point**: scale the value into [1, 4) by
// powers of four, take the root there where Newton converges in a handful of steps, and scale the
// result back by the matching powers of two. Exact scaling, no precision lost.
//
// The older Newtons in `envelope.h`, `motion.h` and `stereo.h`'s RMS are deliberately left alone:
// their inputs are bounded well below the range where the naive start misbehaves, and their
// thresholds are tuned against their current output. Changing them would be re-tuning lip-sync to
// fix a bug it does not have.

#pragma once

namespace roboface {

//: Square root of a non-negative float. Returns 0 for 0 and for anything negative -- a negative
//: input here means a caller computed an energy wrongly, and propagating a NaN would turn that into
//: a face that renders as nothing at all.
inline float squareRoot(float value) {
    if (value <= 0.0f) return 0.0f;

    // Scale into [1, 4) by powers of four, remembering the matching power of two. Powers of four
    // because sqrt(4^n * x) == 2^n * sqrt(x) exactly -- no rounding enters through the scaling.
    float scaled = value;
    float factor = 1.0f;
    while (scaled >= 4.0f) {
        scaled *= 0.25f;
        factor *= 2.0f;
    }
    while (scaled < 1.0f) {
        scaled *= 4.0f;
        factor *= 0.5f;
    }

    // From a start inside [1, 4) Newton is within float precision in about five steps; eight is
    // margin, not hope.
    float root = scaled;
    for (int i = 0; i < 8; ++i) {
        const float next = 0.5f * (root + scaled / root);
        if (next == root) break;
        root = next;
    }
    return root * factor;
}

}  // namespace roboface
