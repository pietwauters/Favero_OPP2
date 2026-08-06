// Thin wrapper around the Favero-enabled IRremoteESP8266 fork, carrying
// over the command table and repeat counts from esp32_FaveroRemoteControl
// (github.com/pietwauters/esp32_FaveroRemoteControl) verbatim. This is the
// only way to command a real Favero apparatus -- its data port is
// output-only, so every one of these mimics a button press on the
// physical FA-05 remote.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <Arduino.h>
#include <IRsend.h>
#include <ir_Favero.h>

class FaveroIR {
public:
    explicit FaveroIR(uint8_t irPin) : m_irsend(irPin) {}

    /// Derives the 11-bit remote address from the ESP32's MAC, same as the
    /// original project, and starts the IR peripheral.
    void begin();

    // Row 1
    void startStop();  // kFaveroStartStop, repeated 6x (matches original --
                        // this is the command most worth the redundancy)
    void rearm();
    void pause();
    // Row 2
    void scorePlusLeft();
    void reset();
    void scorePlusRight();
    // Row 3
    void redCardLeft();
    void set();
    void redCardRight();
    // Row 4
    void yellowCardLeft();
    void matchCount();
    void yellowCardRight();
    // Row 5
    void scoreMinusLeft();
    void prioMan();
    void scoreMinusRight();
    // Row 6
    void block();
    void prioCas();
    void teleAq();

    uint32_t address() const { return m_address; }

private:
    void send(uint8_t command, uint16_t repeats);

    IRsend   m_irsend;
    uint32_t m_address = 0;
};
