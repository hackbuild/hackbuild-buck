# Build your own BUCK

Print a deer, give it a voice, and let the internet talk through it.

## Introduction

BUCK is a white-tail trophy head that talks. You print it in four colours of PETG, drop in a microcontroller, a servo, and a tiny amp, and hang it on pegboard. Text you send it comes out of its mouth in SAM's 1982 robot voice, and the jaw moves with the vowels.

In this guide we'll print the parts, wire the electronics, flash the firmware, connect it to WiFi, and calibrate the jaw so it opens and closes in the right places. Plan on a weekend: most of it is print time.

## Prerequisites

Tools:

- A 3D printer with a bed of at least 256 x 256 mm (or print the plates in more pieces)
- A soldering iron and a little solder
- A small Phillips screwdriver
- A computer with [PlatformIO](https://platformio.org), either the VS Code extension or the `pio` command line, Core 6.2 or newer
- About 6 GB of free disk space for the first PlatformIO build

Parts:

| qty | part | notes |
|---|---|---|
| 1 | ESP32-C3 SuperMini | the USB-C one with 8 pins down each side |
| 1 | SG90 or MG90S 9 g servo | with its single arm horn and two mounting screws |
| 1 | NULLLAB NS4168 I2S amp, 3 W kit | comes with the speaker and PH2.0 leads |
| 1 | the kit speaker | 30 mm square frame, ears 39 mm apart, 7.5 mm deep |
| 1 | 470 to 1000 uF electrolytic capacitor, 10 V or more | across the servo power |
| 1 | 10 k resistor | GPIO4 to ground, keeps the jaw still while the board boots |
| 2 | M3 x 6 screws and washers | hold the amp |
| 2 | M2 x 6 self tapping screws (or #2 x 1/4 in) | hold the speaker |
| 3 | M3 x 16 flat head screws (or #4 x 5/8 in wood screws) | hold the head to the plaque |
| 1 | USB-C cable that carries data | |
| 1 | USB charger, 5 V at 1 A or more | a laptop port browns out when the servo and amp peak together |
| | hookup wire | |

Filament, all PETG at 40 percent infill: about 80 g brown, 6 g bone, 5 g black, and 148 g walnut. Any four colours work, or print it all in one colour and paint it.

## Print the parts

Everything is in `hardware/`:

| file | what it is |
|---|---|
| `buck-plates-3mf.zip` | four plates, one per filament colour, ready for OrcaSlicer and its relatives |
| `buck-one-colour.3mf` | every part on one bed, for a single filament |
| `buck-stl.zip` | each part as its own STL, if your slicer drops the 3MF colours |
| `print-card.txt` | orientations, fits, and hardware in one page |
| `buck-bench.html` | the BUCK bench design tool, opens offline in a browser |

1. Open the plates in your slicer. They were set up for a Flashforge Creator 5 Pro with a 0.4 mm nozzle at 0.24 mm layers, but nothing about them is printer specific.
2. Print them. No part needs supports: the head prints on its neck rim, the jaw on its mouth face, and the plaque on its flat back.
3. Check the fits once they cool. The eyes press in with 0.1 mm of clearance, the nose cap onto its peg, and the ears and antlers into keyed sockets. A drop of CA glue fixes anything loose.

If you want a bigger head, a different jaw swing, or a speaker you measured yourself, open `buck-bench.html`, change the sliders, and export new plates. The fit audit re-runs on every change.

## Wire it up on the bench first

Wire and test everything on the table before it goes inside the head. It is a lot easier to fix a swapped wire now.

Hold the SuperMini with the USB-C port at the top and read the pins off the silk:

```
                 USB-C
          5    [       ]  5V
          6    [       ]  G
          7    [       ]  3V3
          8    [       ]  4
          9    [       ]  3
          10   [       ]  2
          20   [       ]  1
          21   [       ]  0
```

| from | to |
|---|---|
| servo orange (signal) | GPIO4 |
| servo red | 5V |
| servo brown | G |
| amp V | 5V (the pad takes two wires, the amp's and the servo's) |
| amp G | G (two wires here too) |
| amp BCLK | GPIO5 |
| amp LRCLK | GPIO6 |
| amp DIN | GPIO7 |
| speaker | its PH2.0 plug into the amp's output |
| 10 k resistor | GPIO4 to G |
| capacitor | across servo red and brown, at the servo end, stripe to brown |

A few rules for this board:

- Leave GPIO2, 8 and 9 alone. They are strapping pins, 8 drives the onboard LED, and 9 is the BOOT button. GPIO20 and 21 are the serial port.
- The amp output is bridge tied. Never connect either speaker wire to ground.
- The BUCK print card lists a pinout for an ESP32-S3 SuperMini. The C3 uses the table above.

The resistor matters more than it looks. GPIO4 to 7 come out of reset pulled up, because they double as the C3's JTAG pins. Until the firmware starts, a high servo line drives the jaw toward closed, and if the board ever sits in its bootloader the servo keeps pushing. The resistor holds the line low.

## Flash the firmware

1. Clone this repo and open it in PlatformIO.

   ```
   git clone https://github.com/hackbuild/hackbuild-buck
   cd hackbuild-buck
   ```

2. Plug the SuperMini in and flash the `buck` environment.

   ```
   pio run -e buck -t upload
   ```

   The first build downloads the ESP32 toolchain and Arduino core 3.3.12 through the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform, which takes a while. If the upload cannot find or sync the port, hold BOOT, tap RESET, release BOOT, and run it again.

3. Open the serial console.

   ```
   pio device monitor
   ```

   You should see the banner:

   ```
   [buck] BUCK 1.0.0 on ESP32-C3, id buck-4fb4c3, reset: usb
   [buck] jaw not calibrated: it stays still until /open and /closed are set
   [buck] type a line to hear it, /help for commands
   [net] no WiFi set, send: /wifi <ssid> <password>
   ```

4. Type `hello` and press enter. The speaker should say it. The jaw stays still until we calibrate it.

`python3 tools/buck.py console` is a serial console too, if you would rather not use PlatformIO's.

## Connect it to WiFi

Send your network and a name. Each one saves and reboots.

```
/wifi MyNetwork my-password
/id mydeer
```

The C3 only sees 2.4 GHz networks. After the reboot, `/net` shows both links:

```
[net] wifi MyNetwork 192.168.1.40 rssi -55
[net] clasp ready wss://relay.clasp.to  connects 1 rx 6 tx 8
[net] mqtt  ready relay.clasp.chat:1883  connects 1 rx 3 tx 6
```

From any computer, it now answers to:

```
curl -d "hello deer" mqtt://relay.clasp.chat/hackbuild/buck/mydeer/say
```

The ids share one public namespace, so pick something nobody else would. `heatsync` is taken.

## Put it together

1. Press the eyes into their sockets, pointed corner toward the nose, then press the nose cap onto the muzzle peg.
2. Press the ears and antlers into their keyed sockets.
3. Screw the servo to the fin with its own two screws, gear tower toward the right cheek. Leave the horn off.
4. Slide the speaker in under the amp shelf from the right side, face down. Lift it over the seat ring, push it back against the stop, drop it, and screw both ears down.
5. Slide the amp in from the same side, components up, input connector first. Drop it onto the two spigots, screw it down with the M3 x 6 screws and washers, and plug in the speaker.
6. Stick the SuperMini onto the plinth on the left face of the fin, USB toward the plaque, with foam tape or hot glue. Route the wires through the slot in the fin.
7. Feed the USB-C cable through the plaque slot and lay it in the groove on the back.
8. Slide the head over the fin onto the locating lip and drive the three M3 x 16 screws in from the back until they sit flush.

The horn and jaw go on during calibration, next.

## Calibrate the jaw

The firmware needs two pulse widths: where the jaw is closed and where it is fully open. Until both are set, the servo never moves on its own.

1. Plug BUCK into its charger and open the console. With the horn still off, center the servo:

   ```
   /servo 1500
   ```

   It glides to 1500 us and holds.

2. Reach in through the mouth and press the horn onto the spline pointing forward and down. That is the closed position.

3. Slide the jaw up and back through the mouth so its right arm takes the horn, then push the pivot pin in through the left cheek until it clicks.

4. Save closed, and save open about 22 degrees away:

   ```
   /closed 1500
   /open 1255
   ```

   The jaw opens clockwise when you look at the end of the servo shaft, and on an SG90 that is the shorter pulse. 245 us is about 22 degrees, the swing the head was designed for.

5. Try it.

   ```
   /jaw 50
   /sweep
   ```

   The jaw should drop halfway, then close, open fully, and close again. If it pushes up into the head instead, your servo turns the other way: `/open 1745`.

6. Fine tune. Move `/closed` 10 us at a time until the lips meet, and `/open` until the mouth is as wide as you like. Every value is saved. While talking, the firmware stops 10 us short of closed so the servo never presses against the head.

7. Type a sentence and watch the mouth. If the jaw seems to lag the voice, raise `/lead` (default 10 ms); if it runs ahead, lower it.

If you already put the jaw on at an unknown angle, there is a careful way back. A single servo pulse moves an SG90 at most about 12 degrees, less than the jaw's 22 degree swing, so `/probe <us>` can never jam it. Probe toward the end you think is open and watch which way it twitches, then approach closed from the open side in small `/servo` steps. Opening the jaw only runs it to a stop; going past closed pushes it into the head.

## Hang it

Roll the two printed hooks into 1/4 in pegboard, 4 in apart: hold each one tilted up, put the tip in a hole, and roll it down. Slide the plaque's back channel onto both hooks from the side. The head's weight winds the hooks tighter.

## Testing

From another computer:

```
python3 tools/buck.py say "testing, one, two, three"
python3 tools/buck.py watch
```

`watch` prints `said`, `speaking`, and `info` as they change. If BUCK reboots in the middle of a sentence and `info` shows `"reset":"brownout"`, it needs a stronger power supply or the servo capacitor.

Common problems:

| symptom | likely cause |
|---|---|
| no sound | BCLK, LRCLK and DIN swapped, or amp V not on 5V |
| reboots mid sentence, `reset: brownout` | laptop USB port or hub; use a 1 A charger and fit the capacitor |
| jaw buzzes when quiet | it should not: the servo is released 0.6 s after speech. Check `/status` for a held `/servo` |
| jaw creeps when you plug it in | the 10 k pull-down on GPIO4 is missing |
| `/net` shows wifi down | 2.4 GHz only; check `/wifi`; the firmware already uses low TX power for the SuperMini antenna |
| upload cannot find the port | a charge-only USB cable, or hold BOOT, tap RESET, release BOOT |

## What next

You now have a deer that anyone on the internet can make talk. Some ideas:

- Point a CLASP rule at it, so a door sensor or a 3D printer finishing a job makes BUCK announce it.
- Swap SAM for recorded clips and drive the jaw from the audio's loudness.
- Give it a button or a motion sensor and let it greet people as they walk in.

If you build one, we would love to hear it talk. Share it with the [hack.build](https://hack.build) community or open an issue on this repo with a video.
