SAM (Software Automatic Mouth) as ported to the ESP8266 by Earle F. Philhower III
(github.com/earlephilhower/ESP8266SAM, GPL-3.0), itself from the reverse-engineered
C version by Sebastian Macke (github.com/s-macke/SAM).

One change for BUCK in render.c: `samFrameHook` is called at the start of every
10 ms frame with that frame's F1 frequency, F1/F2 amplitudes and consonant flags,
so the firmware can drive the jaw from the same data that makes the sound.
