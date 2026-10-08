// BUCK: the safe way into SAM. Everything that turns a line of text into SAM
// audio goes through here, on the device and in the host tests, so the guards
// below are exercised by the same code that runs on the wall.
#ifndef SAM_SAY_H
#define SAM_SAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  unsigned char speed, pitch, throat, mouth;
} SamVoice;

// Rewrites text into something SAM's reciter handles: letters, digits and
// " .,?!'-:;" only (anything else becomes a space), no character repeated more
// than three times in a row, and no word longer than 14 characters (longer ones
// are split with a space). Writes at most cap - 1 characters plus a NUL; returns
// the length. Long runs and long words are what drove SAM into its worst bugs.
size_t samTame(const char *in, char *out, size_t cap);

// Speaks text that samTame has already cleaned. Cuts it into pieces SAM can
// take, sends every sample to out(ctx, byte), and checks stop(ctx) between
// pieces (stop may be NULL). Returns the number of pieces spoken.
int samSay(const char *text, const SamVoice *voice, void (*out)(void *, unsigned char), void *ctx,
           int (*stop)(void *));

#ifdef __cplusplus
}
#endif

#endif
