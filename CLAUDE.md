# CLAUDE.md — Favero_OPP2
## Briefing for Claude Code sessions

> Read this before touching code. It encodes hard-won findings — several are
> from real crash/protocol debugging, not guesses. Re-deriving them will
> waste time or reintroduce fixed bugs.

## What This Project Is

An ESP32 bridge that plugs into a real Favero fencing apparatus's telemetry
port (10-byte broadcast, 2400-N-8-1, output-only) and exposes it as a
first-class OPP2 apparatus on MQTT, commanding the Favero back via the same
IR remote protocol as `esp32_FaveroRemoteControl`
(github.com/pietwauters/esp32_FaveroRemoteControl). Built on `opp2-library`.
Sibling project: `esp32scoringdeviceMqtt` (the flagship open apparatus) —
same toolchain, same author, and the first place to look when something on
this toolchain misbehaves (see "When stuck" below).

**Author:** Piet Wauters — FIE SEMI Commission member, EFC SEMI Commission
member. Deep fencing-rules and CMS-architecture knowledge; treat domain
claims (officiating rules, protocol intent) as authoritative.

## Architecture

```
Favero apparatus ──2400 baud UART (GPIO13)──► FaveroSerialDecoder ──► Opp2StateOwner (SSOT) ──► Esp32MqttClient (esp_mqtt_client)
                                                                              │
Favero apparatus ◄──IR (GPIO4, FaveroIR)◄── Web UI (remote page) ───────────┤
                                                                              │
                    Web UI (opp2.html: lifecycle, fencer entry, live state) ─┘
```

`Opp2StateOwner` owns one `OPP2::SystemState`; every mutating call publishes
the affected topic only when the value actually changed. Three distinct
input classes, never conflated:
- **Favero telemetry** (`updateFromFavero`): score/clock/lights/cards/
  priority. Favero is authoritative; never overridden or predicted locally.
- **Local web UI** (`setApparatusState`/`setFencer`/`setWeapon`): lifecycle
  and identity Favero has no concept of at all.
- **CMS round-trip** (`nextMatch`/`prevMatch` → publish `control{NEXT/PREV}`
  → CMS decides pool progression → `handleSoftwareMatch`/
  `handleSoftwareFencers` mirror the answer). **This bridge never guesses
  at pool/tableau progression itself** — that's the CMS's job, by design
  and by explicit instruction. See "Protocol boundaries" below.

## Toolchain gotchas (mixed `framework = arduino, espidf`)

All required to even boot on `espressif32@6.5.0` / arduino-esp32 2.0.14 /
ESP-IDF 4.4.6 — every one confirmed by a real failure, not precaution:

- `sdkconfig.defaults` needs `CONFIG_FREERTOS_HZ=1000` (esp32-arduino hard
  requires it) and `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n` (idle-task
  watchdog starves during WiFiManager's blocking portal otherwise).
- `-DESP_TIMER_TASK_CORE=1` build flag required — the shared
  `framework-espidf` package is pre-patched by `esp32scoringdeviceMqtt`'s
  `patch_esp_timer.py` (patches the *global* PlatformIO package cache, not
  per-project) and errors out without this flag.
- **No auto-generated `app_main()`** in this framework mode (unlike plain
  `framework = arduino`). Must provide one explicitly, AND it must not run
  `setup()`/`loop()` directly on the `main` task — that task only gets
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE` (3584 bytes), nowhere near enough.
  **Confirmed by a real crash** (`stack overflow in task main`,
  crash-reboot looping). Fix: spawn a dedicated task —
  `xTaskCreatePinnedToCore(arduinoTask, "arduino_task", 16384, ..., prio 3,
  core 0)` — matching `esp32scoringdeviceMqtt/src/main.cpp` exactly
  (`RTOSSettings.h`: `STACK_ARDUINO_TASK`/`PRIORITY_ARDUINO_TASK`/
  `CORE_ARDUINO_TASK`).
- `loop()` needs an explicit `delay(1)` per iteration — this custom
  `app_main()` path has no built-in yield, and without it the AsyncTCP/lwIP
  task gets starved of CPU (observed as ~0.5-1.2s time-to-first-byte on
  every HTTP request despite <10ms ping RTT).
- Filesystem: **SPIFFS, not LittleFS** — LittleFS compiled fine but silently
  failed to *link* (`undefined reference to LittleFS`) in this exact
  framework mode. Matches what `esp32_FaveroRemoteControl`'s actual code
  uses too (despite its `.ini` saying littlefs).
- Partition table: default 1MB app partition overflows, because
  IRremoteESP8266 compiles its *entire* multi-vendor protocol set
  regardless of `-DSEND_FAVERO`. Using `partitions.csv` (copied from
  arduino-esp32's `huge_app.csv`) — trades away the OTA slot for a ~3MB app
  partition. OTA isn't implemented, so this is free for now, not free later.

## Web server: library choice and route order both matter

- Use `me-no-dev/AsyncTCP` + `me-no-dev/ESPAsyncWebServer` (originals), **not**
  the `esp32async` fork. The fork targets newer arduino-esp32 3.x cores;
  measured **~700ms added to every request's time-to-first-byte** on this
  project's 2.0.14 core, reproduced even on plain static file serving with
  zero app logic. `esp32scoringdeviceMqtt` uses the same originals.
- `server.serveStatic("/", ...)` must be registered **last**, after every
  dynamic route. This ESPAsyncWebServer fork matches routes by *prefix*,
  not exact equality — registered first, it silently swallows every
  dynamic request behind a failed SPIFFS lookup first (~330ms tax, measured).
- No two routes may be a literal string-prefix of one another regardless of
  registration order — confirmed a **real correctness bug**, not just
  latency: `/api/settings` (registered first) silently absorbed every
  request to `/api/settings/save`, so settings writes were being served by
  the *read* handler and never actually persisted. Fixed by renaming to
  `/api/settings-info` / `/api/settings-save` (no shared prefix at all).
  Check any new route against this before adding it.

## mDNS hostname is per-piste, not hardcoded

`MDNS.begin()` used to advertise a single fixed `"favero-opp2"` for every
board — fine for one device, but two boards on the same LAN/event would
fight over `favero-opp2.local`. Fixed 2026-08-06: `buildMdnsHostname()`
(`main.cpp`) derives `favero-opp2-<pisteId>` from the existing `pisteId`
setting instead, sanitized to a legal DNS label (lowercased, non
alnum→hyphen) since `pisteId` is free text typed into the settings page,
not guaranteed clean. Checked `esp32scoringdeviceMqtt` first per usual —
its `friendly_name`/numeric-ID split (`Opp2Handler.cpp`) only feeds the
*MQTT topic's* `piste_id` field, not its mDNS hostname (which is always
plain `Piste_XXX`, numeric-only, `network.cpp`); don't conflate the two
when working on either project. Like the sibling, a hostname change only
takes effect on next boot — settings-save already forces a reboot, so
this needed no live `mdns_hostname_set()` re-call. The resulting hostname
is echoed back on the settings page (`/api/settings-info`'s
`mdnsHostname` field) since there'd otherwise be no way to find a given
board's address on a multi-piste LAN without a serial monitor.

## MQTT: esp_mqtt_client, not PubSubClient — and callback stack safety

- `PubSubClient`/`WiFiClient` was tried first. `subscribe()` reported
  success but **the callback never fired for any message**, including a
  trivial throwaway test topic subscribed and published live from the same
  process. Root cause never fully pinned to the library itself (broker-side
  ACL testing muddied the picture), but switching to `esp_mqtt_client`
  (ESP-IDF native, event-driven, own task — `Esp32MqttClient.h/.cpp`,
  trimmed from `esp32scoringdeviceMqtt/src/AtlasAsyncMqttClient.cpp`) fixed
  it immediately and is the proven approach on this toolchain. No
  `lib_deps` entry needed — `mqtt_client.h` is a core ESP-IDF component,
  picked up automatically by the CMake build.
- **`esp_mqtt_client`'s own task has a small stack.** Never call
  `onConnect`/`onMessage` callback bodies directly from
  `Esp32MqttClient::handleEvent()` — **confirmed by a real crash**
  (`stack overflow in task mqtt_task`) when `onConnect` synchronously ran
  `publishAll()` (seven JSON builds) and `onMessage` ran JSON
  deserialization directly on that task. Fixed with a small FreeRTOS queue:
  `handleEvent()` only does cheap fixed-size copies into a `QueuedEvent`;
  `Esp32MqttClient::loop()` (called from the main loop, ample stack) drains
  the queue and invokes the actual callbacks. **Any new MQTT callback work
  must go through this queue, never inline in `handleEvent()`.**

## Favero 10-byte protocol (`lib/FaveroSerialDecoder`)

`0xFF` sync, 8 payload bytes (scores/time BCD, lamp/priority/card bitfields,
chrono-running bit), checksum = sum(bytes 0-8) mod 256, no carry. Full field
mapping and framing-state-machine rationale in the header comments — read
those before touching it, especially the corner case: an all-zero payload's
checksum is legitimately `0xFF` too, so the wire can contain two consecutive
`0xFF` bytes; a naive "resync on every 0xFF" parser breaks on this. The
HUNTING/LOCKED design handles it — see `FaveroSerialDecoder.h`'s class
comment. Native unit tests (`pio test -e native`) cover this exact case.

**Spec discrepancy, not yet independently reverified against real
hardware:** the source spec's own worked example doesn't satisfy its own
stated checksum rule (recomputed by hand three times: rule gives `0xC5`,
spec text prints `0x56`; every other field in that example checks out).
Implemented per the stated rule. First thing to check if real frames ever
fail checksum validation once GPIO13 is wired to a live Favero.

## Protocol boundaries (explicit user decisions, do not relitigate)

- **Next/Prev never guess pool/tableau progression.** They publish
  `control{command:NEXT/PREV}` (OPP2 §19) and wait for the CMS to push back
  `software/match`+`software/fencers`. An earlier local-counter
  implementation was explicitly corrected away from.
- **Never subscribe to a CMS's internal/e-scoresheet data** (e.g. Atlas's
  custom `software/record` bout-list schema) to work around the above.
  Explicit instruction: "Absolutely not... A scoring device should not
  subscribe to that." If a CMS handles `control`, it's also expected to
  publish standard `match`/`fencers` — verified true for Atlas in practice.
- **End is a request, not a fact.** `ApparatusState::ENDING` is spec'd as
  "awaiting ACK from software" (opp2-library `opp2_types.h`) — pressing End
  publishes `control{command:END}` and *stays* in ENDING until the CMS
  answers on `software/.../control` with `ACK` (→ `WAITING`) or `NAK`
  (→ `HALT`, i.e. as if End had never been pressed). Fixed 2026-08-06: the
  original implementation only ever set local state to ENDING and never
  published the `END` request at all, so nothing could ever ACK it — a
  real apparatus got stuck showing "Ending" indefinitely (score 4-4,
  priority right — a legitimately end-able bout). Confirmed against
  `esp32scoringdeviceMqtt/src/Opp2Handler.cpp`'s working END/ACK/NAK
  handling before fixing. If End ever seems to hang again, first check
  whether the CMS is actually subscribed to and answering on
  `software/<piste>/control` at all — this bridge's half is now correct,
  but a CMS that doesn't answer control-END will still hang forever by
  design (that's the point of an ACK protocol).
- **`handleSoftwareMatch`/`handleSoftwareFencers` relay onward** under
  `apparatus/match` + `apparatus/fencers` (not just update local state).
  Reason: other devices on a piste (displays, repeaters) are expected to
  only need `apparatus/*`, and in practice `software/*` may not even be
  readable by them — the real broker this was tested against silently
  drops anything published under `software/*` from unauthorized/anonymous
  clients (accepts the `PUBLISH` at the protocol level, then never forwards
  or retains it — no error surfaced to the publisher).
- **UW2F is timer-only, never auto-assigns `p_card`.** Elapsed wall time
  since the last valid hit, accruing only while Favero reports the clock
  running, resetting on any score change. `p_card` is a human officiating
  call (explicit decision) and this bridge never sets it above 0.
- **Web UI strings are plain ASCII only.** A `‖`/`▶`/`◀` mojibake bug
  (missing `<meta charset="UTF-8">`) was fixed, but the glyphs were then
  replaced with `|`/`>`/`<` anyway rather than just fixing the encoding —
  don't reintroduce non-ASCII glyphs into `data/*.html` without a real
  reason; this is a small control-panel UI, not worth the fragility.

## When stuck: check `esp32scoringdeviceMqtt` first

Nearly every hard bug in this session (task-watchdog starvation, the
AsyncWebServer fork choice, the `app_main`/stack-size pattern, the MQTT
client choice) was resolved by comparing against that sibling project's
*actual working code* on the same toolchain, not by reasoning from ESP-IDF
docs alone. If something on this toolchain misbehaves, check there before
spending long on first-principles debugging.

## Known gaps / not yet done

- GPIO13 has never been tested against a real physical Favero — the decoder
  is verified only via native unit tests with synthetic byte streams.
- No MQTT broker auth/TLS (the real broker this was tested against is
  anonymous-reachable on port 1883 for at least `apparatus/*`).
- No OTA (traded away for the larger app partition — see above).
- Begin/Halt/Pause remain purely local UI state. End now does a proper
  CMS round-trip (see "Protocol boundaries" below) — Next/Prev/End are the
  three lifecycle actions that leave local state and wait on the CMS.
