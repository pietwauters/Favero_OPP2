#include "FaveroIR.h"
#include "esp_mac.h"

void FaveroIR::begin() {
    m_irsend.begin();

    uint8_t mac[8] = {};
    esp_efuse_mac_get_default(mac);
    // 11-bit address derived from the MAC, same derivation as
    // esp32_FaveroRemoteControl -- keeps this device's IR address stable
    // across reboots without needing an explicit setting.
    m_address = (mac[1] + mac[2] * 256) & 0b11111111111;
}

void FaveroIR::send(uint8_t command, uint16_t repeats) {
    m_irsend.sendFavero(m_address, command, repeats);
}

void FaveroIR::startStop() { send(kFaveroStartStop, 6); }
void FaveroIR::rearm() { send(kFaveroRearm, 3); }
void FaveroIR::pause() { send(kFaveroPause, 3); }

void FaveroIR::scorePlusLeft() { send(kFaveroPlusLeft, 3); }
void FaveroIR::reset() { send(kFaveroReset, 3); }
void FaveroIR::scorePlusRight() { send(kFaveroPlusRight, 3); }

void FaveroIR::redCardLeft() { send(kFaveroRedLeft, 3); }
void FaveroIR::set() { send(kFaveroSet, 3); }
void FaveroIR::redCardRight() { send(kFaveroRedRight, 3); }

void FaveroIR::yellowCardLeft() { send(kFaveroYellowLeft, 3); }
void FaveroIR::matchCount() { send(kFaveroMatchCount, 3); }
void FaveroIR::yellowCardRight() { send(kFaveroYellowRight, 3); }

void FaveroIR::scoreMinusLeft() { send(kFaveroMinusLeft, 3); }
void FaveroIR::prioMan() { send(kFaveroPrioMan, 3); }
void FaveroIR::scoreMinusRight() { send(kFaveroMinusRight, 3); }

void FaveroIR::block() { send(kFaveroBlock, 3); }
void FaveroIR::prioCas() { send(kFaveroPrioCas, 3); }
void FaveroIR::teleAq() { send(kFaveroTeleAq, 3); }
