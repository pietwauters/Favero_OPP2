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

## Web UI is one page (`data/index.html`), not four

Remote/Repeater/OPP2/Settings used to be four separate SPIFFS files reached
by real `<a href>` navigation; merged into one document (2026-08-06) with
JS-only view switching (`showView()`, driven by `location.hash` —
`#remote`/`#repeater`/`#opp2`/`#settings`) plus a Fullscreen API toggle.
Reason: the Fullscreen API spec forces the browser out of fullscreen on
*any* top-level navigation, so a genuine multi-page site can never stay
fullscreen across nav clicks — merging into one document (view-switch, no
navigation) was the only way to make the fullscreen button meaningful.
`repeater.html`/`opp2.html`/`settings.html` are now tiny stubs that
`location.replace()` to the equivalent `/#...` route, kept only so
pre-existing bookmarks/links to those paths (e.g. a spectator display
already pointed at `.../repeater.html`) don't silently 404.

Gotcha hit while building this, worth remembering: `element.style.display
= ''` does **not** show an element that a stylesheet rule hides (here,
`.view { display: none; }`) — clearing the inline style just removes the
override and falls back to the CSS rule, so the element stays hidden. Must
set an explicit value (`'block'`) for the shown view, not `''`.

Because all four views now share one DOM, element IDs that used to be
independent per-page (`statusLine`, `apparatusState`, `matchNum`,
`weapon`, `leftCurrent`/`rightCurrent`) had to be deduplicated: `apparatus
State`/`matchNum`/`weapon`/`leftCurrent`/`rightCurrent` became **classes**
(shared between the Repeater and OPP2 views, updated via one shared
`poll()` using `querySelectorAll`), while `statusLine` became three
distinct IDs (`statusLineRepeater`/`statusLineOpp2`/`statusLineSettings`)
since their semantics differ per view. Adding a fifth view later must
follow the same pattern — check for ID collisions against the whole file,
not just within the section being added, since there's no per-page
scoping anymore.

The Remote page's Favero buttons used to be `<a href="/FaveroXxx">`
wrapping a `<button>` (real navigation, server-side redirected back to
`/`). Converted to `onclick="faveroAction(...)"` (`fetch()`, no
navigation) for the same reason as the merge itself — any real navigation
here would drop fullscreen too. `addFaveroIrRoute()` (`main.cpp`) no
longer redirects, just returns 200, since nothing navigates to these
routes anymore.

Fullscreen support is feature-detected and the button hidden if absent,
same pattern as the vibration feedback below. Checks vendor-prefixed
methods too (`webkitRequestFullscreen`/`mozRequestFullScreen`/
`msRequestFullscreen`), not just the unprefixed `requestFullscreen` —
older Chromium/WebKit engines only expose the prefixed ones, and checking
only the modern name wrongly reported them as unsupported (caught
2026-08-07 when a user reported it working "only on recent Chrome, not
iOS at all"). iOS Safari is a separate, real platform gap, not a
detection bug: non-video fullscreen there is gated behind an
experimental, off-by-default Safari feature flag (Settings > Safari >
Advanced > Feature Flags > "Fullscreen API") on the iOS versions that
have it at all — nothing client-side can force this on. The button label
is plain ASCII ("Fullscreen"/"Exit fullscreen"), not an icon glyph — see
"Web UI strings are plain ASCII only" in Protocol boundaries below; that
decision predates this feature but applies just as much here.

## Piste identity matches esp32scoringdeviceMqtt exactly, on purpose

`Settings` has two piste-identity fields, mirroring
`esp32scoringdeviceMqtt/src/Opp2Handler.cpp` field-for-field: a numeric
`pisteNr` that always exists, and an optional free-text `pisteName`
(e.g. "Red", "Podium"). Three different things derive from these, each
matching the sibling's own convention exactly — **do not let any of them
drift back toward "Favero_OPP2" branding**, since the explicit
instruction (2026-08-07) was that to the CMS this bridge must look like
any other piste, not something Favero-specific:

- **OPP2 `piste_id`** (`buildPisteId()`, `main.cpp`) — `pisteName` if
  set, else `pisteNr` as a string. This is the one place the friendly
  name matters; it's what the CMS/broker actually sees.
- **MQTT client ID** — always `Piste_%03u` from `pisteNr` alone, *never*
  the friendly name, matching `CyranoHandler.cpp`'s
  `sprintf(mqttClientId, "Piste_%.3d", PisteNr)` exactly. Used to be
  `"Favero_OPP2-" + WiFi.macAddress()` — wrong on two counts (brand name
  leaking into a CMS-visible identifier, and not matching the sibling at
  all).
- **mDNS hostname** (`buildMdnsHostname()`, `main.cpp`) — always
  `piste_%03u` from `pisteNr` alone too. This one is *not* CMS-facing
  (purely a LAN convenience for finding this device's own web UI), but
  explicit instruction was to drop the `favero-opp2-` prefix here too and
  match the piste-number convention regardless — so all three identity
  strings now agree on "just the number," matching the sibling, rather
  than a fourth bespoke scheme. Being purely numeric, this needs no
  free-text sanitizing at all (a zero-padded number is always a legal DNS
  label), unlike the free-text-based scheme this replaced.

One explicitly-approved exception: the WiFi captive-portal AP SSID stays
`"Favero_OPP2-setup"` (`WiFiSetup.cpp`) — it's only ever seen during
initial WiFi setup, never by a CMS, and changing it wasn't asked for.

Like the sibling, none of these three take effect live — settings-save
already forces a reboot, so a hostname/client-ID/piste_id change only
applies on next boot. The resolved values are echoed back on the
settings page (`/api/settings-info`) since there'd otherwise be no way to
find a given board's identity/address on a multi-piste LAN without a
serial monitor. Note: changing the NVS key names (`pisteId` →
`pisteNr`/`pisteName`) means a board upgrading from the old scheme loses
its configured piste number on first boot after the flash — it resets to
the default (`pisteNr=1`, no name) and must be re-entered once via the
settings page. No migration path was built for this (one-off, low-stakes
during active development); worth revisiting if this ever needs to
survive an OTA update to boards already deployed at an event.

## Always-on remote-control AP: permanent `WIFI_AP_STA`, not a fallback

The `/FaveroXxx` IR routes (`addFaveroIrRoute()`, `main.cpp`) have zero
dependency on MQTT/OPP2/broker connectivity — they only ever touch
`FaveroIR` (GPIO4). That made a "pure remote control" mode viable: the
device now runs `WIFI_AP_STA` permanently, broadcasting a second,
password-protected AP (`Piste_%03u-remote`, `buildApSsid()`) the entire
time it's powered, independent of whether the normal STA/MQTT link is up
at all. This is concurrent with normal STA operation, not a
fallback-only mode — both run at once.

- `WiFi.mode(WIFI_AP_STA)` + `WiFi.softAP(...)` (`main.cpp::setup()`) run
  **after** `WiFiSetup::begin()` returns, never before or during.
  WiFiManager manages WiFi mode internally throughout its own
  `autoConnect()`/captive-portal flow (temporary AP+STA for the portal,
  dropped back to plain `WIFI_STA` on a successful connect) — any mode
  call issued earlier would just get overwritten by WiFiManager's own.
  `WiFi.softAP()` never touches TCP port 80, so it's unrelated to (and
  placed after) the existing `delay(500)` that compensates for
  WiFiManager's blocking-portal webserver not releasing that port
  instantly.
- `g_server` needed no changes at all — it already binds all interfaces
  (`0.0.0.0:80`), so it became reachable via the AP automatically the
  moment `WiFi.softAP()` came up.
- SSID convention: `Piste_%03u-remote` (`buildApSsid()`, mirrors
  `buildMdnsHostname()`/the MQTT client ID's numeric-first style), kept
  distinct from the one-time-setup `"Favero_OPP2-setup"` SSID
  (`WiFiSetup.cpp`) — that one keeps its already-documented branding
  exception above, untouched by this feature, since it's a different
  network serving a different (temporary, config-only) purpose.
- Security: WPA2-required, not open — `Settings::apPassword` (new NVS
  field, `favero_opp2` namespace, default `"FaveroRemote1"`, always a
  valid 8+ char password out of the box), editable on the settings page
  like every other identity field. `/api/settings-save` validates
  length 8–63 *before* touching any other field and 400s otherwise
  (guard-then-reject, same idiom `addGuardedOpp2Route` already uses for
  the running-clock check) — a bad AP password isn't a soft
  misconfiguration like a bad `mqttBroker`, it makes `WiFi.softAP()` fail
  outright at next boot, so it's the one field here worth rejecting
  up front rather than saving blindly.
- **Boot no longer blocks/reboot-loops just because STA is temporarily
  unreachable** — this was a real gap in the first pass of this feature,
  caught during testing rather than designed in up front: `main.cpp`'s
  `WiFi.softAP()` call only ran *after* `WiFiSetup::begin()` returned,
  and that function used to block in `wm.autoConnect()`'s captive portal
  and `ESP.restart()` on any failed connect — so if the venue router was
  down at boot, the device would loop in WiFiManager's own temporary
  portal forever and the "always-on" remote AP would never actually
  start, directly undercutting the point of the feature. Fixed in
  `WiFiSetup::begin()` (now returns `bool`, see `WiFiSetup.h`): if
  `wm.getWiFiIsSaved()` is true (credentials exist from a prior
  successful setup), `wm.setConnectTimeout(15)` +
  `wm.setEnableConfigPortal(false)` bound the attempt and skip the
  blocking portal on failure — a failed connect at that point almost
  always means the router is just currently down, not that the device
  needs reconfiguring. Only a genuinely credential-less device (first
  boot, or after `resetAndReboot()`) still blocks in the portal, since
  that's the only way to enter STA credentials at all. `main.cpp::loop()`
  nudges `WiFi.reconnect()` every 30s while `WiFi.status() !=
  WL_CONNECTED`, since the core's own auto-reconnect is trusted to cover
  a live connection *drop* but not confidently a connect that never
  succeeded in the first place.
- Restricting an AP-joined client is **client-side only** (`index.html`):
  `IS_AP_CLIENT` checks `location.hostname === '192.168.4.1'` (ESP32's
  fixed default softAP gateway — no `WiFi.softAPConfig()` call changes
  it). Only Repeater/OPP2 are hidden and force-redirected to `'remote'`
  (`AP_ALLOWED_VIEWS`) — **not** because `/api/state` needs MQTT to reach
  an AP client (it doesn't — see below), but because those two views'
  *content* (fencer names, match info, lifecycle buttons) only has real
  meaning once a CMS has actually pushed data over MQTT; showing them
  against a broker that was never reached would just be stale/misleading
  clutter, and their lifecycle buttons (Next/Prev/Begin/End) are no-ops
  without one. **Settings stays reachable from the AP too**,
  deliberately: it has no MQTT dependency, and it's the only place
  `pisteNr` (which drives this device's own AP SSID,
  `Piste_%03u-remote`) and `apPassword` can be changed — hiding it here
  would mean a device could only ever be reconfigured over STA, defeating
  the point for a piste with no STA network available at all. Deliberate
  choice to keep this in the one shared SPA rather than a second HTML
  file/server-side interface check, consistent with "Web UI is one page"
  above — and deliberately spoofable (editing `location.hostname` in
  devtools defeats it trivially). That's accepted: this check is a UX
  convenience only, not a security boundary — the AP's WPA2 password is
  the actual boundary, and every `/FaveroXxx`/`/api/*` route is equally
  reachable from *either* interface regardless of what the SPA shows.
- **`poll()` (`/api/state`) runs unconditionally, regardless of
  `IS_AP_CLIENT`.** Originally gated off for AP clients on the claim that
  it was "MQTT-backed data with no route to reach it" — **that claim was
  wrong**, caught 2026-08-08 when the compact remote layout's Main screen
  (the first Remote-view content to actually display live score/clock)
  showed permanently blank placeholders over the AP. `/api/state` is
  populated straight from the Favero's UART telemetry via
  `Opp2StateOwner`, with zero MQTT/STA dependency — an AP-connected
  client reaches it exactly as well as an STA one, since it's just
  another HTTP endpoint on the same device already serving the page.
  Only the *other* views' content (fencer names/match info from a CMS)
  is actually MQTT-gated, per the bullet above.

**Not yet verified on real hardware:** whether calling
`WiFi.mode(WIFI_AP_STA)` right after WiFiManager has already established
a `WIFI_STA` connection preserves that association cleanly, or forces a
brief reconnect blip. No library/ESP-IDF doc reading resolved this either
way with confidence — flagged rather than assumed, per this project's
usual practice; see "Known gaps" below. Worst case if it does blip is a
brief MQTT reconnect, already handled by `esp_mqtt_client`'s own retry
logic — not expected to be user-visible even if it happens.

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

## Red cards: the Favero's bit is transient, this bridge's count isn't

Confirmed by observation (2026-08-07): the Favero's red-card telemetry
bit flashes true for a frame or two when a card is given, then reverts to
false on its own — even though the card, and the point it awards the
opponent, remains valid for the rest of the match. The Favero has no
memory of an already-given card and no way to un-give one at all (no such
IR command exists). `Opp2StateOwner::updateFromFavero()` used to mirror
the live bit directly into `OPP2::ScoreState.red_cards` (lossy anyway —
that field is a real 0–9 count, the old code only ever wrote 0 or 1) —
this dropped every card the instant the Favero's own bit cleared.

Now: a false→true edge on the raw bit (tracked via
`m_prevRedCardLeft`/`m_prevRedCardRight`, independent of the accumulated
count) increments a persistent per-side counter that survives the bit
clearing. Two things clear it back to 0, matching fencing rules (a card
is valid for the whole match, not one period):
- `FaveroReset` ("Mise a zero") — `Opp2StateOwner::resetRedCards()`,
  called from that route in `main.cpp`.
- A genuinely new match from the CMS — `handleSoftwareMatch()` compares
  incoming `match_num` against the current one before resetting, so a
  retained-message redelivery or a same-match correction (e.g. fencer
  name fix) doesn't wrongly wipe a still-valid card.

**Undo (long-press) exists only in this bridge, not on the real
apparatus.** Since the Favero can't un-give a card, `/FaveroUndoRedLeft`/
`/FaveroUndoRedRight` (`main.cpp`) decrement the local count for that
side via `Opp2StateOwner::undoRedCard()` (never below 0) and, *only if
there was actually a card to undo*, send the existing
`scoreMinus{Left,Right}()` IR command for the **opposite** side — a red
card awards the opponent a point, so undoing it must also undo that
point. The guard (don't touch the score if the count was already 0)
was a deliberate choice: a stray long-press after the count's already at
0 does nothing at all, rather than silently knocking a point off a real
fencer's score with no way to detect it happened by mistake. This
inherits the project's general IR limitation (no feedback path) — if the
score-minus IR command is dropped, the local count still decremented but
the physical scoreboard didn't; nothing new here, every IR-based action
in this project has the same gap.

Long-press is implemented client-side only (`data/index.html`,
`bindRedCardButton()`, pointerdown/pointerup timing, 500ms threshold) —
short tap gives a card as before, holding past the threshold undoes one
instead, with a longer/distinct vibration (200ms vs the usual 100ms) so
it's obvious something different just happened.

**First version didn't work on iOS at all** (confirmed 2026-08-07): iOS
Safari's own long-press gesture (text-selection callout — Copy/Look Up/
Translate) races against and wins over the JS timer, firing
`pointercancel` and showing its own menu before our 500ms elapses.
`style.css`'s `.button`/`.actionbar button` rules now set
`-webkit-touch-callout: none`, `user-select: none` (plus vendor prefixes),
and `touch-action: none` (the last one only on `.button`, since only the
Red Card buttons need custom press-and-hold handling) — the standard,
necessary fix for implementing a custom long-press gesture in Safari.
Verified the underlying pointer-event logic still fires correctly after
this change (Chrome, dispatched PointerEvents), but the actual Safari
callout race can only be confirmed on a real iPhone, not through browser
automation.

**Red cards also have a yellow-LED side effect, confirmed on real
hardware (2026-08-07): the Favero has no separate red LED at all — giving
a red card lights the *yellow* one instead.** So while a side has ≥1
active red card, its yellow telemetry bit is genuinely ambiguous (shared
with the red card's side effect) and must not be trusted. Handled in
`updateFromFavero()`:
- The instant before a side's first red card of a "series" lands
  (`red_cards` 0→1), whatever the *current* yellow value is gets captured
  into `m_hadYellowBeforeFirstRedLeft`/`Right` — this is the only way to
  later tell a real yellow card from the red card's side effect.
- While that side's `red_cards > 0`, the yellow bit is ignored entirely —
  `m_state.score.*.yellow_card` simply stops changing, frozen at whatever
  it was right before the first red card.
- `undoRedCard()` resolves it when a side's count reaches back to 0: if
  there was no genuine yellow before the red card, whatever the LED is
  *physically* showing right now needs clearing — checked against
  `m_lastRawYellowLeft`/`Right` (the actual last-observed telemetry bit,
  tracked unconditionally every frame), **not** the frozen reported
  value, since that value is frozen precisely because it can't be trusted
  to reflect physical LED state. Got this wrong on the first pass of this
  session — initially checked the frozen value instead, which could never
  detect a leftover physical LED that had been correctly ignored the
  whole time. If there's something to clear, fires the
  `setYellowClearCallback` callback for that side.

**`FaveroReset` ("Mise a zero") doesn't clear a lit yellow LED or an
active priority either — confirmed on real hardware.** `armPostResetCleanup()`
arms a pending flag; `updateFromFavero()` waits for telemetry to actually
confirm the reset (both scores read 0 — not a fixed delay after the IR
command) before checking `f.yellowCardRight`/`Left` and
`derivePriority(f)` directly (the *raw* frame, not any frozen value) and
firing `setYellowClearCallback`/`setPriorityClearCallback` if either is
still asserted. Priority is cleared by sending `prioMan()` — confirmed
this is the correct way to clear an active priority on this hardware, not
guessed. Known minor gap, not engineered around: if a new red card lands
before the reset is confirmed, score may never return to exactly 0-0
again this match, leaving the pending flag stuck (harmless — any later
reset re-arms it) rather than firing spuriously.

Neither of these two corrective actions (`setYellowClearCallback`/
`setPriorityClearCallback`) can be issued by `Opp2StateOwner` itself — it
has no `FaveroIR` dependency by design (see class comment). `main.cpp`
wires both to the actual `FaveroIR` calls right after `g_faveroIr.begin()`.
Called synchronously from `updateFromFavero()`, itself called synchronously
from `loop()` (`g_decoder.feed()` → `onFaveroFrame()`), so — unlike the
MQTT callbacks — no queue is needed; everything here is already on the
same task.

None of this (yellow/red LED sharing, reset not clearing yellow/priority,
which IR code clears what) could be verified end-to-end when first
written — no physical Favero was wired to the bench board's GPIO13 yet
(see "Known gaps"). It has since been wired to a real unit and tested;
see the retry mechanism below, added in response to real-hardware
behavior the first pass didn't account for.

**Corrective IR sends aren't guaranteed to land on the first try —
confirmed on real hardware (2026-08-08): "the reset doesn't always clear
the yellow card."** Each `FaveroIR` call already sends 3 repeats
internally (see `FaveroIR.cpp`), but that's no guarantee either if
conditions are bad for that whole ~50ms window (line-of-sight, ambient IR
interference — the same no-feedback-path limitation as every other IR
action in this project). `armYellowClear()`/`armPriorityClear()` +
`driveYellowClearRetry()`/`drivePriorityClearRetry()` (`Opp2StateOwner`)
add a second, higher-level retry layer: once armed, every subsequent
telemetry frame is checked against the *live* bit (never a frozen or
previously-observed value) — resolved the moment it's confirmed cleared,
otherwise re-sent at `kIrRetryIntervalMs` (700ms) up to `kIrMaxAttempts`
(5) before giving up and leaving it for the referee to clear manually.

The 700ms interval is deliberately longer than a single logical press's
own realistic round-trip (send → Favero processes → next telemetry frame
reflects it) for a specific reason: yellow-clearing works by sending the
same *toggle* command that originally lit it. If a retry fired before an
earlier attempt's effect had been reflected in telemetry yet, it would
toggle the LED back **on**, undoing its own fix. The interval is a
judgment call, not measured against real latency data — worth shortening
only with actual evidence it's overly conservative, not by assumption,
given what double-toggling would do.

## Compact remote layout (Atlas-style), selectable in Settings

`data/index.html`'s Remote view (the "classic" 18-button FA-05 grid) got
dense enough ("large buttons... don't fit on a single line") to warrant
an alternative. Modeled on the user's own Atlas-device remote app
(github.com/pietwauters/remotecontrolapp) — mapped against its actual
Kotlin source, not just its layout XML/labels, since several buttons
weren't what their labels suggested (e.g. "Next/Pause" does nothing on
tap and only sends "next period" on long-press; the UW2F button
auto-escalates a single card rather than being three separate Y/R/B
buttons). Selected per-device via `Settings::remoteLayout` (0=classic,
1=compact, `/api/settings-info`+`-save`, takes effect on next boot like
every other setting here) — classic is unchanged, byte-for-byte, when
selected.

- **Three `.remote-layout` divs, one `<section>`.** `#remoteClassic`
  (the existing grid), `#remoteCompactMain` (score/clock/round,
  start/stop, +/-, reset), `#remoteCompactPenalties` (cards, priority,
  UW2F) all live inside the same `<section id="view-remote">` — no new
  files/routes for markup, same "one page" reasoning as above.
  `renderRemoteLayout()` (`index.html`) shows exactly one, based on
  `activeRemoteLayout` (fetched once from `/api/settings-info` at page
  load — distinct from the Settings view's own `<select
  id="remoteLayout">`, which only changes what gets *saved*, not what's
  live in this page load) and, when compact, `compactSubView`
  ('main'/'penalties').
- **`leftScore`/`rightScore`/`clock` converted from ids to classes** so
  the compact Main screen can show the same live values the Repeater
  view already does — the same id→class dedup this file already
  documents doing once for `apparatusState`/`matchNum`/etc. `poll()`
  updated to `setText()`/`querySelectorAll('.clock')` accordingly.
- **Main<->Penalties sub-navigation**: swipe (touchstart/touchend deltaX
  on `#view-remote`, ~60px horizontal threshold, ignored if vertical
  delta dominates) *and* explicit "Penalties ->"/"<- Main" buttons —
  swipe mirrors the Atlas app's own gesture nav, buttons are the
  reliable fallback (desktop, or if swipe tuning ever needs revisiting).
  Coexists fine with the Red/Yellow/UW2F buttons' own pointer-event
  long-press handling below (scoped to individual elements, doesn't stop
  propagation).
- **`bindRedCardButton()` generalized to `bindLongPress()`** (tap fires
  one URL, long-press fires another, or — new — tap can instead just
  call a plain JS callback with no network request, needed for Reset's
  tap-warns/long-press-confirms behavior, mirrored from the Atlas app).
  Reused for: Red cards (unchanged behavior, tap gives/long-press
  undoes), Yellow cards (long-press just re-sends the same tap command —
  Favero yellow is a raw IR *toggle*, not a counted value like red, so
  there's no separate "undo" IR command to long-press into), the new
  UW2F control (tap `/Opp2UW2FCardLeft|Right`, long-press
  `/Opp2UndoUW2FCardLeft|Right`), and Reset (tap only shows "Long-press
  to reset", long-press fires `/FaveroReset`).
- **Explicitly omitted, no Favero equivalent**: Black card buttons
  (Favero hardware has no black-card IR command at all — same limitation
  as everywhere else in this project) and the "Next" half of "Next/Pause"
  (period-advance already happens automatically on the Favero itself,
  confirmed under "Round" above — mapped to the existing `/FaveroPause`
  instead, per explicit user decision).

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
- **Halt/Pause are reported facts, not user commands — no manual buttons
  for them.** `/Opp2Halt` and `/Opp2Pause` (and their opp2.html buttons)
  were removed entirely (2026-08-06), not just left as local-only actions.
  Reason: `updateFromFavero()` already derives Fencing/Halt from the
  Favero's real chrono-running bit whenever current state is Fencing or
  Halt, so a manual Halt press was always transient — the next telemetry
  frame silently flips it back the moment the real clock disagrees.
  Pause had no telemetry counterpart at all, but the explicit decision
  here is that this bridge should reflect what the real apparatus is
  doing, not let an operator inject a lifecycle state that isn't backed
  by reality. Don't re-add either as a manual control.
- **Lifecycle transitions are rejected outright while the clock is
  running.** `addGuardedOpp2Route()` (`main.cpp`) 409s Prev/Begin/Next/End
  whenever `g_opp2.state().clock.running` is true — a bout is live, so no
  lifecycle transition makes sense until the referee/apparatus actually
  stops the clock. opp2.html mirrors this client-side too (disables the
  four lifecycle buttons and shows a warning while running), but the
  server-side 409 is the actual guarantee; the client-side disabling is
  just so a stray click doesn't even need a round-trip to be told no.
  Weapon selection and fencer entry are deliberately NOT gated by this —
  they're metadata edits, not lifecycle transitions.
- **`handleSoftwareMatch`/`handleSoftwareFencers` relay onward** under
  `apparatus/match` + `apparatus/fencers` (not just update local state).
  Reason: other devices on a piste (displays, repeaters) are expected to
  only need `apparatus/*`, and in practice `software/*` may not even be
  readable by them — the real broker this was tested against silently
  drops anything published under `software/*` from unauthorized/anonymous
  clients (accepts the `PUBLISH` at the protocol level, then never forwards
  or retains it — no error surfaced to the publisher).
- **UW2F timer is derived, `p_card` is given locally via the compact
  remote layout, never via IR.** Elapsed wall time since the last valid
  hit, accruing only while Favero reports the clock running, resetting on
  any score change. This was originally "never sets `p_card` above 0 --
  a human officiating call" (explicit decision); revised (2026-08-08,
  explicit decision) once the compact layout's UW2F control gave a
  concrete reason to actually set it: `Opp2StateOwner::incrementPCard()`/
  `undoPCard()` mutate `uw2f.<side>.p_card` directly (capped at 5, per
  `opp2_types.h`'s "1-5 ordinal position per rulebook"), and giving a
  card resets the passivity timer same as a hit does (also explicit
  decision — a card is itself an enforcement event). Still never touches
  the Favero at all: it has no way to display a P-card or this timer, so
  unlike every IR-backed action in this class there's nothing to also
  send over IR here — purely `Opp2StateOwner`/OPP2-over-MQTT state for
  the web UI, repeaters, and CMS. See "Compact remote layout" below.
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
- Whether `WiFi.mode(WIFI_AP_STA)` after an already-established STA
  connection preserves that connection or forces a reconnect blip has
  not been confirmed on real hardware — see "Always-on remote-control AP"
  above.
- Begin remains purely local UI state. End now does a proper CMS
  round-trip (see "Protocol boundaries" below) — Next/Prev/End are the
  three lifecycle actions that leave local state and wait on the CMS.
  Halt/Pause were removed entirely as manual buttons — see "Protocol
  boundaries" below.
