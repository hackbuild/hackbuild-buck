# How the firmware works

BUCK's firmware is Arduino on ESP-IDF, built with PlatformIO for an ESP32-C3 at 160 MHz. The C3 has one core, so everything here is about priorities and bounded work.

## Tasks

```
serial --+
CLASP  --+--> remote gate --> text queue --> synth task --> stream buffer --> player task --> I2S amp
MQTT   --+    (clean, limit)  (8 lines)      (SAM)          (8 KB, 370 ms)    (20 ms blocks)  +-> servo
                                               |                                ^
                                               +--> jaw events, timestamped ----+
                                               +--> "said" event --> net task --> status on both relays
```

| task | priority | stack | job | file |
|---|---|---|---|---|
| player | 5 | 4 KB | stream buffer to I2S, jaw to servo, console jaw commands | `src/speech.cpp` |
| synth | 3 | 8 KB | text to SAM to samples and jaw events | `src/speech.cpp` |
| net | 2 | 8 KB | WiFi, CLASP link, MQTT link, status | `src/net/net.cpp` |
| loop | 1 | 8 KB | serial console | `src/console.cpp` |

The player outranks the synth, and both outrank the network. A TLS handshake or a stalled socket can slow a link down, but it cannot take time from the 20 ms audio blocks. WiFi's own tasks run above all of these, as ESP-IDF sets them.

Only the player task touches the servo. The console asks for jaw moves through a small queue (`jawCommand()`), so nothing ever fights over the LEDC channel.

Only the net task touches sockets. Links, codecs and buffers have one owner and need no locks.

## Speech and the jaw

SAM renders speech in 10 ms frames. Each frame carries the formants that make its sound, and the first formant, F1, tracks how open a real mouth is: high for "ah", low for "ee" and "oo", and lowest for m, b and p. `lib/sam/src/render.c` has one change from upstream, a `samFrameHook` called at the start of every frame with F1 and the voicing amplitude. The synth task turns that into an openness value:

```
voiced frame:    open = clamp((F1 - 6) / (24 - 6)) * clamp(A1 / 13)
unvoiced frame:  open = 0.15     s, sh, f, th, ch
silence:         open = 0        A1 is 0, which also covers the closure of p, t, k
```

Each value is stamped with its sample position in the stream and queued. The player writes 20 ms blocks to I2S, and for each block takes the widest frame due by the end of that block plus `/lead` ms. It smooths that with a 30 ms attack and a 70 ms release and moves the servo.

The I2S DMA holds about 60 ms, so the servo hears about the jaw roughly 60 ms before the speaker plays the sound. That covers the SG90's lag. Every line starts with 60 ms of silence so the first syllable gets the same head start.

While talking, "closed" is 10 us short of the calibrated closed end (`JAW_CLOSED_MARGIN_US`). A servo held exactly at the contact point pushes against the head and draws stall current, which on a USB supply is enough to brown out the board. After 0.6 s of quiet the PWM detaches entirely.

Talking moves are rate limited to 60 us per 20 ms block (`JAW_SLEW_US`), about 5 degrees. That caps the servo's current spikes and turns a jump from a held position into a quick glide.

Everything reaches SAM through `lib/sam/src/sam_say.c`. `samTame` keeps the input to what SAM's reciter knows, caps any repeated character at three ("soooooo" says "sooo") and splits words longer than 14 characters. `samSay` cuts lines into pieces; SAM's reciter quietly stops once its phoneme output passes 120 characters, and every digit is a word, so a phone number fills that fast. It checks where the reciter's end marker landed and, if a piece overflowed, splits it at the space nearest the middle and tries each half.

SAM itself is old C with byte-sized indexes into fixed arrays. A line of the letter o hung the deer on 2026-10-07 (an endless loop in `InsertBreath`), and fuzzing under AddressSanitizer found five more ways to hang or overrun it. All are fixed and listed in `lib/sam/README.md`, and `test/test_sam` runs the same `samTame`/`samSay` path on the host under the sanitizers, with an alarm on every input so a hang fails the test.

The jaw never goes outside the calibrated range during speech, `/jaw` or `/sweep`. Only `/servo` and `/probe`, the calibration tools, can reach the full 500 to 2500 us, and `/servo` glides at about 30 degrees a second.

## Memory

Everything is allocated once, at boot:

| what | size |
|---|---|
| text queue, 8 lines of 400 characters | 3.2 KB |
| audio stream buffer | 8 KB |
| jaw event queue, 128 events | 1 KB |
| SAM working memory | 3.7 KB |
| CLASP link buffers (rx 2 KB, tx 640 B, frame 600 B) | 3.3 KB |
| MQTT link buffers (rx 512 B, tx 512 B) | 1 KB |
| task stacks | 28 KB |

Static RAM use is 49 KB of 320 KB. With WiFi up and a TLS session open, about 95 KB of heap stays free, and the lowest it has been is reported as `minHeap` in `status/info`. No code path allocates per message. The codecs write into caller buffers and point into received frames instead of copying.

The TLS client does allocate when it connects. Before each attempt the CLASP link checks that the largest free block is at least 45 KB and skips the attempt if not, so a fragmented heap delays a reconnect instead of crashing it.

## When things go wrong

| failure | what BUCK does |
|---|---|
| WiFi drops | both links stop; WiFi auto reconnect runs, with a fresh `WiFi.begin` every 30 s; after 20 minutes offline it reboots, once it is quiet |
| relay unreachable or refuses | that link backs off 2 s, 4 s, 8 s and so on up to 2 minutes, plus up to 25 percent jitter; the other link carries on |
| link goes silent | WebSocket ping or MQTT PINGREQ every 20 s when idle; nothing heard for 65 s means reconnect |
| relay drops the subscription but keeps the socket | heartbeat: every 45 s each link sends itself a message through its own subscription (CLASP by SET echo, MQTT from a short lived second connection); no beat back for 140 s means reconnect. Pings cannot catch this, the relay keeps answering them |
| malformed CLASP frame | ignored, link stays up; the decoder checks every length and nesting depth (fuzzed with 200,000 random frames in the tests) |
| malformed MQTT stream | link is dropped and reconnected |
| oversize MQTT packet | read to its end so the stream stays in sync, and delivered cut to the 512 byte buffer; the text is then cut to 200 characters as usual |
| oversize WebSocket message (over 2 KB) | read off the wire and dropped, counted |
| flood of messages | token bucket (4, then 1 per 3 s) and a cap of 4 waiting network lines |
| old messages replayed by a relay | ignored: CLASP snapshots are skipped, MQTT messages before SUBACK are skipped |
| a task hangs | task watchdog, 60 s, on the player, synth, net and loop tasks; the chip resets with a backtrace on serial. Every network connect is bounded at 8 s plus the TLS handshake, and every later socket write at 3 s (plain) or 8 s (TLS), with the watchdog fed between steps |
| crashes in a loop | a counter in RTC memory; three crash resets in a row boot into safe mode with networking off, so serial still works. Five crash-free minutes turn networking back on by themselves; `/reboot` clears the count and boots normally straight away |
| brownout | the chip resets; `status/info` reports `"reset":"brownout"` so it shows up remotely |
| nobody reading USB serial | the CDC TX timeout is zero, so logging never blocks |

## Serial console

115200 baud on the USB port. Lines starting with `/` are commands; anything else is spoken.

| command | does |
|---|---|
| `/vol <0-100>` | volume, saved |
| `/voice <name>` | `sam` `elf` `robot` `stuffy` `oldlady` `et`, saved |
| `/speed` `/pitch` `/throat` `/mouth <1-255>` | SAM's four voice controls, saved |
| `/stop` | cut the current line and clear the queue |
| `/tone [hz] [ms]` | test tone |
| `/jaw <0-100>` | hold the jaw at a percent open for 3 s |
| `/sweep` | closed, open, closed over 3 s |
| `/open <us or deg>` `/closed <us or deg>` | calibration ends; over 180 means microseconds; saved |
| `/lead <ms>` | jaw lead over the sound, default 10, saved |
| `/servo <us>` | glide to a raw pulse and hold it until `/release` or speech |
| `/probe <us> [n]` | n single pulses, then let go |
| `/release` | stop driving the servo |
| `/wifi <ssid> <password>` | save and reboot; the SSID cannot contain spaces, the password can |
| `/id <name>` | 1 to 31 letters, digits, `-` or `_`; save and reboot |
| `/clasp <wss://host or off>` | CLASP relay; save and reboot |
| `/mqtt <host[:port] or off>` | MQTT broker; save and reboot |
| `/tls on or off` | check the relay's certificate (on by default); save and reboot |
| `/remote on or off` | accept network messages; saved |
| `/net` | WiFi and link status, counters |
| `/status` | firmware, calibration, voice, heap |
| `/reboot` `/factory` | restart; wipe saved settings and restart |

Network settings reboot on change because the network task reads them once at start. That keeps a half written string from ever reaching a socket.

## Settings

NVS namespace `buck`. Missing keys fall back to the build defaults in `platformio.ini`.

| key | type | default |
|---|---|---|
| `open` `closed` | int, us | `BUCK_DEFAULT_OPEN_US`, `BUCK_DEFAULT_CLOSED_US`, 0 means uncalibrated |
| `lead` | int, ms | 10 |
| `vol` | int | 50 |
| `voice` | string | `sam` |
| `speed` `pitch` `throat` `mouth` | int | 72, 64, 128, 128 |
| `id` | string | `BUCK_DEFAULT_ID`, else `buck-` and the MAC |
| `ssid` `pass` | string | `BUCK_DEFAULT_SSID`, `BUCK_DEFAULT_PASS` |
| `clasp` | string | `wss://relay.clasp.to` |
| `mqtt` `mqttport` | string, int | `relay.clasp.chat`, 1883 |
| `tls` `remote` | bool | true, true |

## TLS

The CLASP link checks the relay's certificate against the roots in `src/net/ca_roots.h`: Google Trust Services R1, R3 and R4, and ISRG X1 and X2. relay.clasp.to currently chains to GTS Root R4 through Cloudflare. The earliest of these roots expires in 2035. If the relay moves to a CA outside that list, the CLASP link logs a connect failure and MQTT keeps working; add the new root, or `/tls off` as a stopgap.

## Build environments

| env | for |
|---|---|
| `buck` | your own BUCK; nothing preset |
| `heatsync` | the HeatSync Labs head: id `heatsync`, its WiFi, and its jaw calibration (open 500 us, closed 730 us) |
| `native` | host unit tests: `pio test -e native` |

The platform is pinned to pioarduino 55.03.312-1, which is Arduino core 3.3.12 on ESP-IDF 5.5.5, and needs PlatformIO Core 6.2 or newer.

## Power

Peak draw is the amp, the servo and the WiFi radio together, and the amp is the largest part. Measured on the HeatSync head running from a bus powered USB hub: talking at `/vol 50` browned out at the first syllable, the jaw alone at `/vol 0` did not, and `/vol 30` got through a 14 second sentence clean.

The firmware already trims what it can: WiFi transmit power is 8.5 dBm (radio bursts drop from about 300 mA, and the SuperMini's antenna likes it), talking never presses the jaw into its stop, and jaw moves are rate limited. The rest is hardware: a USB charger of 1 A or more and a 470 to 1000 uF capacitor at the servo. On weak power, keep `/vol` around 30.
