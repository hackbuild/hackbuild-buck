SAM (Software Automatic Mouth) as ported to the ESP8266 by Earle F. Philhower III
(github.com/earlephilhower/ESP8266SAM, GPL-3.0), itself from the reverse-engineered
C version by Sebastian Macke (github.com/s-macke/SAM).

Changes for BUCK, each marked `BUCK:` in the source:

- `render.c` calls `samFrameHook` at the start of every 10 ms frame with that
  frame's F1 frequency, F1/F2 amplitudes and consonant flags, so the firmware
  drives the jaw from the same data that makes the sound.
- `sam_say.c` and `sam_say.h` are new: the only way the firmware reaches SAM.
  `samTame` limits the input (no character repeated more than three times, no
  word over 14 characters, reciter charset only) and `samSay` cuts it into
  pieces, splitting again whenever the reciter's 120 character phoneme limit
  would silently drop the end of a piece.

Bugs fixed, found when a line of the letter o hung the deer on 2026-10-07 and
then by fuzzing (test/test_sam runs the same path under AddressSanitizer):

- `InsertBreath` looped forever on a long word with no pause: it placed the
  breath at index 255, the byte index wrapped to 0, and the loop restarted while
  overwriting the phoneme list. It now makes a pause where it is, forgets used
  pauses, and is bounded.
- `Insert` lost the list's end marker when the list filled, because the working
  memory starts zeroed; `PrepareOutput` then walked the list forever. The marker
  is kept, and the walk is bounded.
- `PrepareOutput` wrote past its 60 entry output arrays on long runs of short
  phonemes, and could queue more than `Render`'s 256 frames. It now renders early.
- `Render` counted frames down by two for sampled consonants and stopped only at
  exactly zero, so an odd count wrapped to 255 and ran on.
- `bufferpos` grew without bound (signed overflow); only its remainder mod 50
  matters, so that is all it keeps.
- Several rules read `[pos-1]` and `[X-1]` at index 0: the phoneme, length and
  stress arrays now have a zeroed guard byte in front.
- Phoneme indexed tables (78 to 82 entries) were read with the 254 and 255
  markers; they are zero padded to 256.
