# Favero_OPP2

An ESP32 bridge that turns a **Favero FA-05** fencing scoring apparatus into
a fully connected [OpenPiste](https://github.com/OpenPiste) (OPP2) device —
without any modification to the Favero itself.

It reads the FA-05's built-in 10-byte serial telemetry output (score, clock,
lights, cards, priority) and republishes it live over MQTT as a standard
OPP2 apparatus, and sends commands back to the Favero using the same
infrared remote-control protocol as
[esp32_FaveroRemoteControl](https://github.com/pietwauters/esp32_FaveroRemoteControl).
The result: a legacy standalone box gains network visibility, remote
control, and interoperability with any OPP2-speaking competition
management software (CMS), scoreboard, or display — for the cost of an
ESP32 dev board and a few wires.

## Background: the OpenPiste ecosystem

[OpenPiste](https://github.com/OpenPiste) defines **OPP2**, an open,
vendor-neutral MQTT protocol for fencing piste equipment: scoring
apparatuses, remote controls, scoreboards, and competition management
software all talk to each other over a shared broker using well-defined
topics and JSON payloads, instead of every vendor inventing its own closed
protocol. A piste's live score, clock, cards, and match/fencer identity
become just another set of MQTT topics that any compliant device can
publish or subscribe to.

This project is one piece of that ecosystem, built on
[`opp2-library`](https://github.com/OpenPiste/opp2-library) (topic/payload
handling shared across OpenPiste devices) and sitting alongside sibling
projects such as [`esp32scoringdeviceMqtt`](https://github.com/pietwauters)
(a native OPP2 scoring apparatus) and `esp32_FaveroRemoteControl` (the IR
remote this project's outbound commands are modeled on).

## The goal: connecting a closed apparatus

Favero apparatuses like the FA-05 are widely deployed, reliable, and
completely closed — no network connectivity, and no vendor path to gain
any. The FA-05 does expose two things the manufacturer never intended as an
integration surface:

- an output-only serial telemetry port (2400 baud, 10-byte frames) meant
  for a physical scoreboard, and
- an infrared remote-control receiver, meant for a handheld IR remote.

This project turns those two into a bidirectional bridge: an ESP32 listens
to the telemetry port to know everything the Favero knows (score, clock,
cards, priority, lights) and drives the IR receiver to command it (start/
stop, reset, score, cards, priority) — all without opening the apparatus
or modifying its firmware. On the network side it looks and behaves like
any other OPP2 apparatus, plus a small local web UI for the parts a Favero
has no concept of at all (match/fencer identity, lifecycle), reachable at
the device's IP.

Full protocol and architecture details (frame format, state ownership,
protocol boundaries) are documented in [`CLAUDE.md`](CLAUDE.md).

## Hardware

- ESP32 dev board
- Favero FA-05 telemetry output wired to **GPIO13** (RX only — the port is
  output-only, so no TX connection is needed)
- IR LED (or IR blaster circuit) on **GPIO4**, aimed at the Favero's IR
  receiver

> **Status:** GPIO13 telemetry decoding has not yet been verified against a
> real physical Favero — see "Known gaps" in `CLAUDE.md`.

## Building

Built with [PlatformIO](https://platformio.org/). Two environments:

| Environment | Purpose |
|---|---|
| `esp32dev` | The bridge firmware (mixed `arduino, espidf` framework) |
| `native` | Host-side unit tests for the Favero frame decoder — no hardware needed |

```bash
# Firmware
pio run -e esp32dev
pio run -e esp32dev -t upload
pio run -e esp32dev -t uploadfs   # upload data/ (web UI) to SPIFFS

# Native unit tests (decoder logic only)
pio test -e native
```

### Dependencies

All `lib_deps` resolve directly from public URLs — no local checkouts
needed. One is not upstream `crankyoldgit/IRremoteESP8266` but
[`pietwauters/IRremoteESP8266`](https://github.com/pietwauters/IRremoteESP8266)
(branch `favero-fa05`, pinned to a commit): a fork with Favero FA-05 IR
protocol support (`sendFavero`/`decodeFavero`) added on top of upstream
`v2.8.6`, since that protocol isn't (and likely won't be) part of upstream.

This project relies on several toolchain-specific fixes (task stack sizes,
partition layout, filesystem choice, exact library forks) that took real
debugging to pin down — see `CLAUDE.md` for the full list and the reasons
behind each one before changing `platformio.ini` or `sdkconfig.defaults`.

## First boot / setup

1. Flash firmware and filesystem (`upload` + `uploadfs` above).
2. On first boot (or after a WiFi reset), the device has no saved network
   and opens a WiFi access point named **`Favero_OPP2-setup`** with a
   captive config portal.
3. Connect to that AP and use the portal to set:
   - your WiFi SSID/password,
   - the **piste ID** (identifies this apparatus's OPP2 topics),
   - the **MQTT broker** host (defaults to `openpiste.local`, resolved via
     mDNS; a literal IP also works).
4. The device reboots, joins your network, and connects to the broker.
   From then on it's reachable at its DHCP-assigned IP for:
   - `/` — Favero IR remote control page,
   - `/opp2.html` — live bout view (score/clock/cards) and lifecycle
     controls (fencer entry, weapon, next/prev/begin/halt/pause/end),
   - `/settings.html` — piste ID / MQTT broker, and a "reconfigure WiFi"
     button that re-opens the setup portal.

Match/fencer progression (next/prev) is never guessed locally — the device
publishes an OPP2 `control` command and waits for the competition
management software on the broker to answer with the real
match/fencers data. See "Protocol boundaries" in `CLAUDE.md` for why, and
what will (and won't) work without a CMS present on the broker.
