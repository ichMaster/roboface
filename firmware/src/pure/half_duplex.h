// One bus, one direction at a time: who is allowed to be hearing right now.
//
// The device's own speaker is the loudest thing in the room. With the microphone open it hears
// itself, the endpointer calls that speech, and the device answers its own reply -- forever, with
// no person involved. Half-duplex until AEC arrives in v3.4 (ARCHITECTURE §Audio).
//
// **Pure**, because the failure it prevents is not acoustic but a state machine getting stuck. The
// dangerous case is not "listening resumed a little late"; it is listening that never resumes,
// because a reply ended by a route nobody thought about -- a cancelled turn, a dropped socket, a
// fault mid-sentence. The device is then permanently deaf, and looks perfectly healthy: connected,
// idle, a face on the screen, and no answer to anything ever said to it again.
//
// So the rule is deliberately blunt: **playback ending resumes listening, by every route.** There
// is exactly one way in and one way out.

#pragma once

namespace roboface {

class HalfDuplexGuard {
  public:
    //: Whether the application wants to be listening at all. Off means PTT-only, and playback
    //: then has nothing to suspend.
    void wantListening(bool wanted) { wanted_ = wanted; }
    bool listeningWanted() const { return wanted_; }

    //: Playback has claimed the bus.
    void playbackStarted() { speaking_ = true; }

    //: Playback has released it, by whatever route -- drained, cancelled, aborted, faulted. Returns
    //: true if listening resumes as a result, which is the caller's cue to clear the endpointer:
    //: the silence during playback belongs to nobody's pause, and the tail of the reply is not the
    //: start of a sentence.
    bool playbackEnded() {
        const bool was_speaking = speaking_;
        speaking_ = false;
        return was_speaking && wanted_;
    }

    bool isSpeaking() const { return speaking_; }

    //: The question every frame asks: should the microphone be feeding the endpointer?
    bool shouldListen() const { return wanted_ && !speaking_; }

  private:
    bool wanted_ = false;
    bool speaking_ = false;
};

}  // namespace roboface
