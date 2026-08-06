#include "Opp2StateOwner.h"

#include <Arduino.h>
#include <ArduinoJson.h>

void Opp2StateOwner::begin(const char* pisteId, Esp32MqttClient* mqtt) {
    m_mqtt = mqtt;
    strncpy(m_state.piste_id, pisteId, OPP2::PISTE_ID_MAX - 1);

    // Match::weapon/type/phase_type are mandatory enum fields for the
    // library's serializer (UNKNOWN fails serialization outright -- see
    // checkSerialize). type/phase_type get harmless defaults so `match`
    // can publish before the user touches the OPP2 page; weapon is
    // deliberately left UNKNOWN (and match stays unpublishable) until the
    // user actually picks one via setWeapon() -- guessing a weapon would
    // be actively misleading.
    m_state.match.type = OPP2::MatchType::INDIVIDUAL;
    m_state.match.phase_type = OPP2::PhaseType::DE;
}

void Opp2StateOwner::publish(OPP2::Publisher pub, OPP2::MessageType mt,
                              const char* json, int qos, bool retained) {
    if (!m_mqtt) return;
    OPP2::TopicParser::buildFrom(m_state.piste_id, pub, mt, m_topic,
                                 sizeof(m_topic));
    m_mqtt->publish(m_topic, json, qos, retained);
}

OPP2::Priority Opp2StateOwner::derivePriority(const FaveroFrame& f) {
    if (f.prioRight && !f.prioLeft) return OPP2::Priority::RIGHT;
    if (f.prioLeft && !f.prioRight) return OPP2::Priority::LEFT;
    return OPP2::Priority::NONE;
}

void Opp2StateOwner::updateFromFavero(const FaveroFrame& f) {
    // ── Score, cards, priority ──────────────────────────────────────────
    // Favero only gives a boolean "red card present" bit, not a count --
    // OPP2::ScoreState.red_cards is a 0-9 count, so this mapping is
    // lossy (any red card shows as 1, never more). No way around that
    // with what the data port provides.
    const OPP2::Priority prio = derivePriority(f);
    const bool hitScored =
        m_state.score.right.score != f.rightScore || m_state.score.left.score != f.leftScore;
    const bool scoreChanged =
        hitScored ||
        m_state.score.right.yellow_card != f.yellowCardRight ||
        m_state.score.left.yellow_card != f.yellowCardLeft ||
        (m_state.score.right.red_cards > 0) != f.redCardRight ||
        (m_state.score.left.red_cards > 0) != f.redCardLeft ||
        m_state.score.priority != prio;

    if (scoreChanged) {
        m_state.score.right.score = f.rightScore;
        m_state.score.left.score = f.leftScore;
        m_state.score.right.yellow_card = f.yellowCardRight;
        m_state.score.left.yellow_card = f.yellowCardLeft;
        m_state.score.right.red_cards = f.redCardRight ? 1 : 0;
        m_state.score.left.red_cards = f.redCardLeft ? 1 : 0;
        m_state.score.priority = prio;
        publishScore();
    }

    // ── Clock ────────────────────────────────────────────────────────────
    const uint32_t time_ms =
        (static_cast<uint32_t>(f.minutes) * 60 + f.seconds) * 1000;
    const bool clockChanged =
        m_state.clock.time_ms != time_ms || m_state.clock.running != f.chronoRunning;
    if (clockChanged) {
        m_state.clock.time_ms = time_ms;
        m_state.clock.running = f.chronoRunning;
        publishClock();
    }

    // ── Lights ───────────────────────────────────────────────────────────
    // Favero's yellow lamp bits (byte 5, D4/D5) have no OPP2::Lights
    // field -- the yellow *card* state above (byte 9) is what's
    // semantically meaningful, so the lamp bits are deliberately unused.
    const bool lightsChanged = m_state.lights.left.on_target != f.redOn ||
                               m_state.lights.left.white != f.leftWhite ||
                               m_state.lights.right.on_target != f.greenOn ||
                               m_state.lights.right.white != f.rightWhite;
    if (lightsChanged) {
        m_state.lights.left.on_target = f.redOn;
        m_state.lights.left.white = f.leftWhite;
        m_state.lights.right.on_target = f.greenOn;
        m_state.lights.right.white = f.rightWhite;
        publishLights();
    }

    // ── Apparatus state ──────────────────────────────────────────────────
    // The chrono running bit only drives Fencing<->Halt while a match is
    // actively in progress (i.e. already Halt or Fencing). Waiting/Pause/
    // Ending are explicit decisions made from the web UI -- Favero's
    // telemetry never gets to override those.
    const OPP2::ApparatusState current = m_state.apparatus_state.state;
    if (current == OPP2::ApparatusState::FENCING ||
        current == OPP2::ApparatusState::HALT) {
        const OPP2::ApparatusState derived = f.chronoRunning
                                                 ? OPP2::ApparatusState::FENCING
                                                 : OPP2::ApparatusState::HALT;
        if (derived != current) setApparatusState(derived);
    }

    // ── UW2F passivity timer ────────────────────────────────────────────
    // Favero's data port has no concept of this at all -- derived entirely
    // here from the running clock and whether a hit landed. p_card is
    // deliberately never touched: that's a referee's call, this bridge
    // only ever reports elapsed time.
    tickUW2F(f.chronoRunning, hitScored);
}

void Opp2StateOwner::setApparatusState(OPP2::ApparatusState state) {
    if (m_state.apparatus_state.state == state) return;
    m_state.apparatus_state.state = state;
    publishApparatusState();
}

void Opp2StateOwner::setWeapon(OPP2::Weapon weapon) {
    if (m_state.match.weapon == weapon) return;
    m_state.match.weapon = weapon;
    publishMatch();
}

void Opp2StateOwner::nextMatch() { publishControl(OPP2::Command::NEXT); }
void Opp2StateOwner::prevMatch() { publishControl(OPP2::Command::PREV); }

void Opp2StateOwner::endMatch() {
    setApparatusState(OPP2::ApparatusState::ENDING);
    publishControl(OPP2::Command::END);
}

void Opp2StateOwner::handleSoftwareControl(const OPP2::Control& control) {
    switch (control.command) {
        case OPP2::Command::ACK:
            setApparatusState(OPP2::ApparatusState::WAITING);
            break;
        case OPP2::Command::NAK:
            setApparatusState(OPP2::ApparatusState::HALT);
            break;
        default:
            break;
    }
}

void Opp2StateOwner::handleSoftwareMatch(const OPP2::Match& match) {
    m_state.match = match;
    publishMatch();  // relay under apparatus/match -- see handleSoftwareFencers
}

void Opp2StateOwner::handleSoftwareFencers(const OPP2::Fencers& fencers) {
    m_state.fencers = fencers;
    // Relayed under apparatus/fencers so other devices on this piste
    // (displays, repeaters) get correct fencer data without needing to
    // subscribe to software/* themselves -- which, in practice, they may
    // not even be able to do (software/* turned out to be ACL-restricted
    // to authorized publishers on the real broker this was tested
    // against; apparatus/* is the one topic tree every device on a piste
    // is expected to be able to read).
    publishFencers();
}

void Opp2StateOwner::setFencer(OPP2::Side side, const char* name,
                               const char* noc) {
    OPP2::Person& fencer = (side == OPP2::Side::LEFT) ? m_state.fencers.left.fencer
                                                       : m_state.fencers.right.fencer;
    fencer.present = true;
    strncpy(fencer.name, name, sizeof(fencer.name) - 1);
    strncpy(fencer.nation, noc, sizeof(fencer.nation) - 1);
    publishFencers();
}

// Every serialize call MUST be checked before publishing: on failure (a
// mandatory enum field still UNKNOWN, or the buffer too small),
// OPP2::Serializer::serialize() writes nothing to m_payload at all -- since
// m_payload is a shared, reused buffer, an unchecked failure silently
// republishes whatever the *previous* message type serialized into it,
// under the wrong topic. (This is exactly how the match topic briefly
// published fencers-shaped content during testing -- Match::weapon/type/
// phase_type default to UNKNOWN, which the library treats as invalid.)
bool Opp2StateOwner::checkSerialize(OPP2::SerializeError err, const char* what) {
    if (err == OPP2::SerializeError::OK) return true;
    Serial.printf("[opp2] %s serialize failed (err=%d) -- not publishing\n", what,
                  static_cast<int>(err));
    return false;
}

void Opp2StateOwner::publishScore() {
    m_state.score.seq = ++m_seq;
    auto err = OPP2::Serializer::serialize(m_state.score, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "score")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::SCORE, m_payload, 1, true);
}

void Opp2StateOwner::publishClock() {
    m_state.clock.ts = OPP2::Timestamp::fromMillis(millis());
    auto err = OPP2::Serializer::serialize(m_state.clock, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "clock")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::CLOCK, m_payload, 0, true);
}

void Opp2StateOwner::publishLights() {
    m_state.lights.seq = ++m_seq;
    m_state.lights.ts = OPP2::Timestamp::fromMillis(millis());
    auto err = OPP2::Serializer::serialize(m_state.lights, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "lights")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::LIGHTS, m_payload, 1, true);
}

void Opp2StateOwner::publishApparatusState() {
    m_state.apparatus_state.seq = ++m_seq;
    auto err = OPP2::Serializer::serialize(m_state.apparatus_state, m_payload,
                                           sizeof(m_payload));
    if (!checkSerialize(err, "apparatus_state")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::APPARATUS_STATE, m_payload, 1,
            true);
}

void Opp2StateOwner::publishMatch() {
    // Called from setWeapon() (weapon is the one piece of match metadata
    // this bridge genuinely knows on its own) and from handleSoftwareMatch()
    // (relaying the CMS's competition/phase/poule/match_num onward under
    // apparatus/match, for displays/repeaters that only read apparatus/*).
    m_state.match.seq = ++m_seq;
    auto err = OPP2::Serializer::serialize(m_state.match, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "match")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::MATCH, m_payload, 1, true);
}

void Opp2StateOwner::publishFencers() {
    m_state.fencers.seq = ++m_seq;
    auto err = OPP2::Serializer::serialize(m_state.fencers, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "fencers")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::FENCERS, m_payload, 1, true);
}

void Opp2StateOwner::publishUW2F() {
    m_state.uw2f.seq = ++m_seq;
    auto err = OPP2::Serializer::serialize(m_state.uw2f, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "uw2f")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::UW2F, m_payload, 1, true);
}

// Elapsed time since the last valid hit, counting only while the clock is
// actually running (Favero-reported) -- paused/halted time doesn't accrue
// passivity. p_card is never touched here (see updateFromFavero); that's
// left entirely to whoever officiates.
void Opp2StateOwner::tickUW2F(bool running, bool hitOccurred) {
    const uint32_t now = millis();

    if (hitOccurred) {
        m_uw2fBaseMs     = 0;
        m_uw2fRunSinceMs = now;
    } else if (running && !m_uw2fRunning) {
        m_uw2fRunSinceMs = now;  // just started running
    } else if (!running && m_uw2fRunning) {
        m_uw2fBaseMs += now - m_uw2fRunSinceMs;  // just paused -- bank elapsed time
    }
    m_uw2fRunning = running;

    const uint32_t elapsedMs = m_uw2fBaseMs + (running ? (now - m_uw2fRunSinceMs) : 0);
    const bool     crossedSecond = elapsedMs / 1000 != m_state.uw2f.time_ms / 1000;
    m_state.uw2f.time_ms = elapsedMs;
    if (crossedSecond || hitOccurred) {
        publishUW2F();
    }
}

void Opp2StateOwner::publishControl(OPP2::Command command) {
    OPP2::Control msg;
    msg.seq = ++m_seq;
    msg.ts = OPP2::Timestamp::fromMillis(millis());
    msg.command = command;
    auto err = OPP2::Serializer::serialize(msg, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "control")) return;
    // QoS 1, NOT retained (Section 19) -- a one-shot request, not state.
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::CONTROL, m_payload, 1, false);
}

void Opp2StateOwner::publishConnection(bool online) {
    OPP2::Connection msg;
    msg.seq = ++m_seq;
    msg.online = online;
    if (online) {
        msg.device_present = true;
        strncpy(msg.device, "Favero_OPP2", sizeof(msg.device) - 1);
        msg.fw_version_present = true;
        strncpy(msg.fw_version, "0.1.0", sizeof(msg.fw_version) - 1);
    }
    auto err = OPP2::Serializer::serialize(msg, m_payload, sizeof(m_payload));
    if (!checkSerialize(err, "connection")) return;
    publish(OPP2::Publisher::APPARATUS, OPP2::MessageType::CONNECTION, m_payload, 1, true);
}

void Opp2StateOwner::publishAll() {
    publishConnection(true);
    publishApparatusState();
    publishScore();
    publishClock();
    publishLights();
    publishMatch();
    publishFencers();
    publishUW2F();
}

void Opp2StateOwner::writeStateJson(char* buf, size_t bufSize) const {
    StaticJsonDocument<768> doc;

    doc["apparatus_state"] = static_cast<int>(m_state.apparatus_state.state);
    doc["weapon"] = static_cast<int>(m_state.match.weapon);
    doc["match_num"] = m_state.match.match_num;

    JsonObject clock = doc["clock"].to<JsonObject>();
    clock["running"] = m_state.clock.running;
    clock["time_ms"] = m_state.clock.time_ms;

    JsonObject right = doc["right"].to<JsonObject>();
    right["score"] = m_state.score.right.score;
    right["yellow_card"] = m_state.score.right.yellow_card;
    right["red_card"] = m_state.score.right.red_cards > 0;
    right["on_target"] = m_state.lights.right.on_target;
    right["white"] = m_state.lights.right.white;
    right["name"] = m_state.fencers.right.fencer.name;
    right["noc"] = m_state.fencers.right.fencer.nation;

    JsonObject left = doc["left"].to<JsonObject>();
    left["score"] = m_state.score.left.score;
    left["yellow_card"] = m_state.score.left.yellow_card;
    left["red_card"] = m_state.score.left.red_cards > 0;
    left["on_target"] = m_state.lights.left.on_target;
    left["white"] = m_state.lights.left.white;
    left["name"] = m_state.fencers.left.fencer.name;
    left["noc"] = m_state.fencers.left.fencer.nation;

    doc["priority"] = static_cast<int>(m_state.score.priority);

    serializeJson(doc, buf, bufSize);
}
