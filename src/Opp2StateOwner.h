// Single source of truth for this bridge's OPP2 state -- mirrors the
// Opp2Handler SSOT pattern from esp32scoringdeviceMqtt (one owner, publish
// on every accepted change), simplified: everything here runs from the
// Arduino loop() on one core, so no mutex is needed.
//
// Three distinct kinds of update come in:
//   - updateFromFavero(): score/clock/lights/priority/cards, decoded from
//     the real apparatus's telemetry. Favero is authoritative for these;
//     this bridge never predicts or overrides them.
//   - setApparatusState()/setFencer(): match lifecycle and identity that
//     Favero has no concept of at all. These come from this device's own
//     web UI.
//   - nextMatch()/prevMatch(): standard OPP2 apparatus behavior, per spec
//     (Section 19) -- publish a `control` command (NEXT/PREV) and let the
//     competition-management software decide what "next" means and push
//     back the real match/fencers data. This bridge never guesses at pool
//     progression itself; handleSoftwareMatch()/handleSoftwareFencers()
//     mirror whatever the CMS sends in response (see main.cpp's MQTT
//     subscription to software/match + software/fencers).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <opp2.h>

#include "Esp32MqttClient.h"
#include "FaveroSerialDecoder.h"

using favero_ir_side_cb_t = std::function<void(OPP2::Side)>;
using favero_ir_cb_t      = std::function<void()>;

class Opp2StateOwner {
public:
    void begin(const char* pisteId, Esp32MqttClient* mqtt);

    /// Wires this bridge's "please send this IR command" callbacks --
    /// called synchronously from updateFromFavero() (same task as
    /// loop(), no queue needed unlike the MQTT callbacks -- see
    /// main.cpp's loop(), which feeds the decoder directly). Needed for
    /// two corrective actions the real Favero can't do itself: clearing
    /// a yellow-card LED left over from a red card's side effect (see
    /// undoRedCard()) or from FaveroReset not clearing it at all, and
    /// clearing an active priority that FaveroReset also doesn't clear
    /// (see armPostResetCleanup()). This class deliberately has no
    /// FaveroIR dependency of its own -- main.cpp wires these to the
    /// actual FaveroIR calls.
    void setYellowClearCallback(favero_ir_side_cb_t cb);
    void setPriorityClearCallback(favero_ir_cb_t cb);

    /// Wires the two additional IR sends the long-press "full reset"
    /// sequence needs beyond the two above -- see armFullReset(). Same
    /// reasoning: this class has no FaveroIR dependency of its own.
    void setSetCommandCallback(favero_ir_cb_t cb);
    void setMatchCountCallback(favero_ir_cb_t cb);

    void updateFromFavero(const FaveroFrame& frame);

    void setApparatusState(OPP2::ApparatusState state);
    void setWeapon(OPP2::Weapon weapon);
    void setFencer(OPP2::Side side, const char* name, const char* noc);

    /// Requests the next/previous match from the CMS by publishing a
    /// control command (NEXT/PREV) -- does not touch local match state at
    /// all. Whatever the CMS pushes back arrives via handleSoftwareMatch()/
    /// handleSoftwareFencers().
    void nextMatch();
    void prevMatch();

    /// Signals end-of-match: sets local state to ENDING (spec: "awaiting
    /// ACK from software") AND publishes a `control{command:END}` request
    /// -- the actual request to the CMS, without which no ACK/NAK can ever
    /// arrive and the apparatus is stuck in ENDING forever. Always
    /// re-publishes the request even if already ENDING, so pressing End
    /// again retries a request that was dropped or never answered.
    void endMatch();

    /// Called from main.cpp's MQTT callback on a software-published
    /// control message. ACK moves the apparatus on to WAITING (match
    /// over, ready for the next one); NAK means the CMS rejected the end
    /// request (e.g. score/priority doesn't actually satisfy a valid
    /// end-of-match condition) -- falls back to HALT so the referee
    /// decides what happens next, same as if End had never been pressed.
    void handleSoftwareControl(const OPP2::Control& control);

    /// Called from main.cpp's MQTT callback when a software/match or
    /// software/fencers message arrives -- mirrors it into local state
    /// AND relays it under apparatus/match + apparatus/fencers, so other
    /// devices on this piste (displays, repeaters) get correct data by
    /// subscribing only to apparatus/* -- the same topic tree they already
    /// need for lights/score/clock, and the only one every device is
    /// guaranteed able to read (software/* may be ACL-restricted).
    void handleSoftwareMatch(const OPP2::Match& match);
    void handleSoftwareFencers(const OPP2::Fencers& fencers);

    /// Undoes a mistakenly-given red card on `side`: decrements that
    /// side's locally-tracked red card count (never below 0) and returns
    /// true if there was one to undo. The Favero itself has no "remove
    /// card" command and no memory of a card once its telemetry bit
    /// clears (see updateFromFavero()), so this is purely a bridge-side
    /// correction -- the caller is responsible for also telling the real
    /// Favero to decrement the *opposite* side's score via IR when this
    /// returns true, since a red card awards the opponent a point that
    /// this method alone can't touch.
    bool undoRedCard(OPP2::Side side);

    /// Clears both sides' locally-tracked red card counts (and the
    /// "had yellow before first red" flags that go with them) -- called
    /// when FaveroReset (Mise a zero) is pressed, and when the CMS pushes
    /// a genuinely new match_num via handleSoftwareMatch(). A red card is
    /// valid for the whole match, not just one period, so nothing else
    /// resets it.
    void resetRedCards();

    /// Arms the post-reset cleanup: the real Favero's Mise a zero resets
    /// score/clock but leaves a lit yellow LED and an active priority
    /// completely untouched. Call this (in addition to resetRedCards())
    /// specifically when FaveroReset was just pressed -- NOT from
    /// handleSoftwareMatch()'s CMS-driven reset, which has no physical
    /// reset behind it to compensate for. updateFromFavero() watches for
    /// the reset actually taking effect (both scores confirmed at 0, not
    /// assumed after a fixed delay) and only then checks whether yellow
    /// or priority are still asserted, firing the corresponding callback
    /// if so.
    void armPostResetCleanup();

    /// Arms the long-press "full reset" sequence (compact remote layout's
    /// Reset button, /FaveroFullReset in main.cpp -- NOT the classic
    /// grid's plain Mise a zero button, which stays a single one-shot
    /// FaveroReset press): once Mise a zero is confirmed via telemetry
    /// (score reads 0-0, reusing armPostResetCleanup()'s existing
    /// yellow/priority-clear logic), also resets red cards and P-cards;
    /// once yellow/priority settle, sends Set (confirmed against the
    /// clock reading 3:00) and then MatchCount repeatedly (confirmed
    /// against the round reading 1). Every step is confirmed against live
    /// telemetry before advancing -- same no-feedback-path caution as
    /// every other corrective IR action in this class. Re-arming while a
    /// previous sequence is still in progress simply restarts it.
    void armFullReset();

    /// Zeroes both sides' UW2F P-card counts. Nothing else ever resets
    /// these (undoPCard() only decrements one at a time) -- called from
    /// the full-reset sequence above, since a fresh bout shouldn't carry
    /// over stale passivity cards from before the reset.
    void resetPCards();

    /// Gives/clears a black card on `side` -- OPP2::ScoreState::black_card
    /// is a plain per-side bool (Section 11), set/cleared purely as local
    /// state. The Favero has no black-card IR command at all (no such
    /// button exists on the physical remote), so unlike yellow/red there
    /// is nothing to also send over IR here -- same reasoning as the
    /// UW2F P-card methods below.
    void giveBlackCard(OPP2::Side side);
    void undoBlackCard(OPP2::Side side);

    /// Gives a UW2F P-card to `side`: increments m_state.uw2f.<side>.p_card
    /// (capped at 5, per opp2_types.h's "1-5 ordinal position per
    /// rulebook") and resets the passivity timer, same as a hit does --
    /// explicit decision: giving a card is itself an enforcement event.
    /// Purely local/OPP2 state -- the Favero has no way to display this at
    /// all, so unlike every IR-backed action in this class, there is
    /// nothing for a caller to also send over IR here.
    void incrementPCard(OPP2::Side side);

    /// Undoes the last P-card given on `side` (decrements, never below 0).
    /// Does NOT restore the passivity timer -- that's the separate
    /// restoreUW2FTimer() below, matching how giving/undoing a card and
    /// undoing a timer reset are two distinct controls on the physical
    /// remote this mirrors.
    void undoPCard(OPP2::Side side);

    /// Restores the passivity timer to what it was immediately before the
    /// most recent reset (from a hit or incrementPCard()) -- a
    /// single-level undo. No-op if nothing has reset the timer since the
    /// last restore (or since boot).
    void restoreUW2FTimer();

    /// Publishes connection (LWT counterpart) -- call with true once MQTT
    /// connects, the broker publishes {"online":false} itself via LWT on
    /// unexpected disconnect.
    void publishConnection(bool online);

    /// Republishes every retained topic -- call once after (re)connecting
    /// to MQTT so a fresh/reconnected broker has the full current state.
    void publishAll();

    const OPP2::SystemState& state() const { return m_state; }

    /// Fills buf with a JSON snapshot of current state for the web UI's
    /// GET /api/state polling endpoint. Not an OPP2 wire message -- just
    /// whatever the UI needs to render.
    void writeStateJson(char* buf, size_t bufSize) const;

private:
    void publish(OPP2::Publisher pub, OPP2::MessageType mt, const char* json, int qos,
                 bool retained);

    void publishScore();
    void publishClock();
    void publishLights();
    void publishApparatusState();
    void publishMatch();
    void publishFencers();
    void publishUW2F();
    void publishControl(OPP2::Command command);

    /// Derives uw2f.time_ms: elapsed wall time since the last valid hit,
    /// accruing only while `running` (the Favero-reported clock state) is
    /// true. Favero has no concept of this at all -- see updateFromFavero().
    void tickUW2F(bool running, bool hitOccurred);

    /// Banks the current elapsed passivity time into
    /// m_uw2fBaseMsBeforeReset and arms m_uw2fHasSnapshot, right before
    /// something is about to zero the timer (a hit, in tickUW2F(), or
    /// incrementPCard()) -- the single piece of state restoreUW2FTimer()
    /// needs to undo exactly one such reset.
    void snapshotUW2FBeforeReset();

    static OPP2::Priority derivePriority(const FaveroFrame& f);
    static bool checkSerialize(OPP2::SerializeError err, const char* what);

    /// IR has no feedback path (project-wide limitation), so a corrective
    /// send (clearing a stuck yellow LED or priority) isn't guaranteed to
    /// land on the first try -- confirmed by observation (2026-08-08: "the
    /// reset doesn't always clear the yellow card"). Armed once, then
    /// driven every frame against live telemetry: resolved as soon as the
    /// condition is confirmed cleared, otherwise re-sent at
    /// kIrRetryIntervalMs (deliberately longer than a single logical
    /// press's own realistic round-trip, so a not-yet-reflected earlier
    /// attempt doesn't get double-toggled back on by an over-eager retry)
    /// up to kIrMaxAttempts before giving up.
    struct PendingIrRetry {
        bool     armed    = false;
        uint8_t  attempts = 0;
        uint32_t nextRetryAtMs = 0;
    };
    static constexpr uint32_t kIrRetryIntervalMs = 700;
    static constexpr uint8_t  kIrMaxAttempts     = 5;

    void armYellowClear(OPP2::Side side);
    void armPriorityClear();
    void driveYellowClearRetry(OPP2::Side side, bool currentlyLit);
    void drivePriorityClearRetry(bool currentlyAsserted);

    /// See armFullReset(). AWAITING_ZERO_SCORE/AWAITING_CARDS_AND_
    /// PRIORITY_CLEAR are driven directly in updateFromFavero() (the
    /// conditions they wait on -- score, and the existing yellow/priority
    /// retry state -- are already right there); AWAITING_SET_CONFIRM and
    /// CYCLING_ROUND each get their own small driver below since their
    /// confirm conditions and give-up behavior differ from the generic
    /// PendingIrRetry pattern above.
    enum class FullResetPhase : uint8_t {
        IDLE,
        AWAITING_ZERO_SCORE,
        AWAITING_CARDS_AND_PRIORITY_CLEAR,
        AWAITING_SET_CONFIRM,
        CYCLING_ROUND,
    };
    /// Unlike driveYellowClearRetry()/drivePriorityClearRetry() (skip
    /// sending entirely if the condition doesn't currently need
    /// correcting), this always sends Set at least once -- the referee
    /// asked for it to be sent as part of the sequence, not only if the
    /// clock doesn't already happen to read 3:00.
    void driveFullResetSetStep(bool clockAtThreeMinutes);
    /// This one DOES skip sending if the round already reads 1 -- "send
    /// MatchCount until round is 1" is naturally satisfied with zero
    /// sends if it's already there.
    void driveFullResetRoundStep(uint8_t currentRound);

    FullResetPhase m_fullResetPhase = FullResetPhase::IDLE;
    PendingIrRetry m_setRetry;
    PendingIrRetry m_roundCycleRetry;
    // Generous safety valve against a runaway loop (e.g. a decode
    // desync that never reflects numMatches changing at all) -- NOT a
    // verified real bound on how many MatchCount presses the Favero's
    // own round-cycle actually needs to wrap back to 1, unlike
    // kIrMaxAttempts above (measured against real corrective-send
    // behavior). Worth revisiting against real hardware if this ever
    // gives up before reaching 1 in practice.
    static constexpr uint8_t kRoundCycleMaxAttempts = 12;

    favero_ir_cb_t m_onNeedSet;
    favero_ir_cb_t m_onNeedMatchCount;

    Esp32MqttClient* m_mqtt = nullptr;
    uint32_t         m_seq  = 0;

    OPP2::SystemState m_state;
    char              m_payload[OPP2::JSON_SIZE_MAX] = {};
    char              m_topic[64]                    = {};

    // UW2F passivity timer bookkeeping (millis()-based, independent of
    // Favero's own clock resolution).
    uint32_t m_uw2fBaseMs     = 0;      // banked elapsed ms from prior running segments
    uint32_t m_uw2fRunSinceMs = 0;      // millis() when the current running segment started
    bool     m_uw2fRunning    = false;  // running state as of the last tick

    // See snapshotUW2FBeforeReset()/restoreUW2FTimer(): single-level undo
    // for the most recent timer reset (hit or P-card given).
    uint32_t m_uw2fBaseMsBeforeReset = 0;
    bool     m_uw2fHasSnapshot       = false;

    // Previous frame's raw Favero red-card bits, to detect a false->true
    // edge (a card being given) independently of m_state.score.*.red_cards,
    // which is now a persistent accumulator decoupled from the live bit --
    // see updateFromFavero().
    bool m_prevRedCardLeft  = false;
    bool m_prevRedCardRight = false;

    // Most recently observed *raw* yellow telemetry bit per side, updated
    // every frame regardless of whether a red card currently has it
    // frozen -- this is the only way to know the real Favero's physical
    // LED state while m_state.score.*.yellow_card is frozen (see
    // updateFromFavero()). undoRedCard() needs this, not the frozen
    // value, to know whether the LED needs clearing once the last red
    // card for a side is undone.
    bool m_lastRawYellowLeft  = false;
    bool m_lastRawYellowRight = false;

    // Per side: was a genuine yellow card already active the instant
    // before that side's first currently-active red card was given?
    // Captured once per red-card "series" (0->1 transition) in
    // updateFromFavero(), consulted in undoRedCard() when that side's
    // count returns to 0, to know whether the (possibly still-lit)
    // yellow LED represents a real yellow card or just the red card's
    // side effect.
    bool m_hadYellowBeforeFirstRedLeft  = false;
    bool m_hadYellowBeforeFirstRedRight = false;

    // See armPostResetCleanup().
    bool m_awaitingResetConfirmation = false;

    favero_ir_side_cb_t m_onNeedYellowClear;
    favero_ir_cb_t      m_onNeedPriorityClear;

    // See PendingIrRetry above.
    PendingIrRetry m_yellowClearRetryLeft;
    PendingIrRetry m_yellowClearRetryRight;
    PendingIrRetry m_priorityClearRetry;
};
