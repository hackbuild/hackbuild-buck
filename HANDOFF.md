# buck, handoff

## what it is

A 3D printed talking deer head. ESP32-C3 SuperMini, SG90 jaw servo, NULLLAB NS4168 I2S amp. Text arrives over USB serial, CLASP over WebSocket, or MQTT; SAM speaks it on the chip and the jaw follows each frame's first formant. One hangs at HeatSync Labs and anyone can make it talk with curl.

Read `RULES.md` first. It is absolute and wins over this file.

## deployments

| what | where |
|---|---|
| repo | https://github.com/hackbuild/hackbuild-buck |
| talk page | https://hackbuild.github.io/hackbuild-buck/ (GitHub Pages, `web/`, `.github/workflows/pages.yml`) |
| the HeatSync head | id `heatsync`, WiFi `heatsynclabs`, flashed from the `heatsync` env on 2026-10-07 |
| CLASP door | `wss://relay.clasp.to`, open, no MQTT (DigitalOcean App Platform behind Cloudflare) |
| MQTT door | `relay.clasp.chat:1883`, open; its CLASP WebSocket side needs a token |
| curl | `curl -d "hello" mqtt://relay.clasp.chat/hackbuild/buck/heatsync/say` |

## where it stands, 2026-10-07

Firmware 1.0.0 is on the HeatSync head and verified end to end, on the HeatSync WiFi:

- curl over MQTT, `tools/buck.py` over MQTT and CLASP, and the `@clasp-to/core` 4.3.2 SDK all made it speak
- status (`online`, `speaking`, `said`, `info`) reached subscribers on both relays, late joiners included; the jaw stream ran at about 15 Hz
- the rate limiter passed 4 of a burst of 6
- 20 host tests pass, including 200,000 random frames through the CLASP decoder
- after the last flash: 6 minutes up, no reconnects, heap flat at 94.7 KB free, 74.5 KB lowest

The talk page was checked against the same SDK calls from Node, not yet in a browser once it went live.

The head's saved voice is speed 80, pitch 72 (default is 72, 64). That came from an earlier session and was left as the person running it wanted; `/voice sam` over serial puts it back.

## decisions and why

- Two relays. relay.clasp.to is the only open CLASP WebSocket relay; relay.clasp.chat is the only one with MQTT open, and curl speaks MQTT. Guest tokens on relay.clasp.chat are scoped to `/chat/**`, so one relay could not serve both. curl over `wss://` cannot work: the relay drops PUBLISH before HELLO and reads one frame per WebSocket message.
- Own WebSocket client and codecs. The CLASP Arduino library (`bindings/arduino`, 1.0.0, on every branch, unpublished) speaks TCP only, and the relays only take WebSocket. The codec follows `crates/clasp-core/src/codec.rs`, with test vectors captured from the live relay.
- Ignore replays. relay.clasp.chat replays every stored MQTT topic before SUBACK with retain cleared, so BUCK skips anything on a topic until its SUBACK. CLASP snapshots are skipped too.
- ESP32-C3, not S3. The print card and bench say S3 SuperMini; the board on the wall is a C3 (esptool says so), wired servo 4, BCLK 5, LRCLK 6, DIN 7.
- Jaw calibration on the HeatSync head: it was assembled at the servo's end stop with the jaw fully open. The BUCK geometry (shaft out the right cheek, jaw opens clockwise looking at the shaft) put open at the short-pulse end. Probing with single pulses (at most about 12 degrees each, under the 22 degree swing) confirmed it, and closed was found from the open side in small steps. Result: open 500 us, closed 730 us.
- Talking stops 10 us short of closed. Driving the servo into the contact point stalls it and browned out the board on USB power.
- WiFi TX power 8.5 dBm. Cuts radio bursts from about 300 mA, and is the known fix for the SuperMini's antenna. Full power plus servo plus amp browned out the board.
- Network settings save and reboot instead of applying live, so no task ever reads a half written string.
- GPL-3.0 for the whole repo, because SAM is GPL.

## open questions

- Anyone on the internet can make the head say anything. The rate limit stops floods, not words. A word filter, an allow list, or `/remote off` during events are options; nothing is filtered now. This is a call for HeatSync.
- The head runs off a bus powered USB hub on the bench. On the wall it needs a 1 A charger. The 10 k pull-down on GPIO4 and the servo capacitor may not be fitted yet; both are in docs/BUILD.md.
- An HTTP bridge on relay.clasp.to (`clasp http`, `POST /v1/emit/...`) would allow a plain `curl -X POST https://...` that works without MQTT. Today the relay answers every HTTP path with `ok`.

## things found in CLASP along the way

Worth fixing upstream in lumencanvas/clasp:

- relay.clasp.chat's MQTT bridge stores non-retained publishes and replays them before SUBACK with the retain flag cleared.
- docs.clasp.to's LLM reference shows `subscribe` callbacks as `(address, value)`; `client.ts` has `(value, address)`.
- The protocol doc's SET example says payload length 0x1B; its own bytes are 26 (0x1A).
- `clasp pub` 4.5.0 prints "OK Published" and then logs "Server connection not yet implemented".
- The Arduino library's value codes differ from the protocol doc's (bool as 0x01 plus a byte), though they match the Rust codec.

## this machine

- PlatformIO Core was upgraded to 6.2.0; pioarduino 55.03.312-1 needs it.
- The disk was full during the first build. `~/.platformio/packages/framework-arduinoespressif32-libs` holds only the `esp32c3` and `hosted` folders of core 3.3.12's libs, marked installed by hand. Building for any other ESP32 with this platform will fail until it is reinstalled with space free: `pio pkg uninstall -g -t framework-arduinoespressif32-libs`, then build again.
- The CLASP source checked out at `~/Projects/lumencanvas/clasp` (branch relay-scaling) is where the relay, codec and Arduino library were read from.

## running locally

```
pio test -e native                         # host tests, must pass before a commit
pio run -e buck -e heatsync                # both firmware builds, must pass before a commit
pio run -e heatsync -t upload              # flash the HeatSync head
pio device monitor                         # serial console
python3 tools/buck.py say "hi"             # MQTT, stdlib only
python3 tools/buck.py watch --via clasp    # needs websockets
```

Opening the C3's USB serial with RTS high and DTR low resets it. `tools/buck.py console` holds RTS low.

Before flashing an assembled head, park the jaw part open with `/jaw 50`. The reset during upload drives the servo line high briefly, which pushes toward closed.

## commits

Author is Moheeb Zara <hackbuildvideo@gmail.com>. No trailers, no AI attribution, imperative lowercase subjects under 60 characters. See `RULES.md`.
