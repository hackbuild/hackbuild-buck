// BUCK: see sam_say.h.

#include "sam_say.h"

#include <ctype.h>
#include <string.h>

#include "SamData.h"
#include "reciter.h"
#include "sam.h"

#define CHUNK_MAX 80   // characters per first cut; the reciter's input buffer is 256
#define WORD_MAX 14
#define REPEAT_MAX 3

size_t samTame(const char *in, char *out, size_t cap) {
  size_t n = 0;
  int run = 0, word = 0;
  char last = 0;
  if (!out || cap == 0) return 0;
  for (const char *p = in; p && *p && n + 2 < cap; p++) {
    unsigned char c = (unsigned char)*p;
    char ch = (isalnum(c) || strchr(" .,?!'-:;", c)) ? (char)c : ' ';
    if (tolower((unsigned char)ch) == tolower((unsigned char)last)) {
      if (++run > REPEAT_MAX) continue;   // "soooooo" says "sooo"
    } else {
      run = 1;
    }
    last = ch;
    if (isalnum((unsigned char)ch)) {
      if (++word > WORD_MAX) {            // break up a very long word
        out[n++] = ' ';
        word = 1;
      }
    } else {
      word = 0;
    }
    out[n++] = ch;
  }
  out[n] = 0;
  return n;
}

// Reciter on txt[0..n) into in[]. Returns the phoneme length before the 155 end
// marker, or -1 when the reciter cannot handle the text.
static int recite(const char *txt, int n, char *in) {
  int k = 0, i;
  memset(in, 0, 256);
  for (i = 0; i < n && k < CHUNK_MAX; i++) in[k++] = (char)toupper((unsigned char)txt[i]);
  in[k] = '[';
  memset(samdata, 0, sizeof(SamData));
  if (!TextToPhonemes(in)) return -1;
  for (i = 0; i < 255; i++) {
    if ((unsigned char)in[i] == 155) {
      in[i + 1] = 0;   // SetInput reads to a NUL; the phonemes can run past the original one
      return i;
    }
  }
  in[255] = 0;
  return 255;
}

typedef struct {
  const SamVoice *voice;
  void (*out)(void *, unsigned char);
  void *ctx;
  int (*stop)(void *);
  int pieces;
} Job;

// The reciter quietly stops once its phoneme output passes 120 characters
// (reciter.c, pos36654). When that happens the piece is split at the space
// nearest its middle and each half is tried again.
static void piece(Job *job, const char *txt, int n, int depth) {
  static char in[256];
  int plen;
  while (n > 0 && *txt == ' ') { txt++; n--; }
  while (n > 0 && txt[n - 1] == ' ') n--;
  if (n <= 0 || (job->stop && job->stop(job->ctx))) return;
  plen = recite(txt, n, in);
  if (plen < 0) return;
  if (plen > 118 && n > 1 && depth < 8) {
    int mid = n / 2, cut = -1, d;
    for (d = 0; d <= n / 2 && cut < 0; d++) {
      if (mid - d > 0 && txt[mid - d] == ' ') cut = mid - d;
      else if (mid + d < n - 1 && txt[mid + d] == ' ') cut = mid + d;
    }
    if (cut < 0) cut = mid;
    piece(job, txt, cut, depth + 1);
    piece(job, txt + cut, n - cut, depth + 1);
    return;
  }
  SetSpeed(job->voice->speed);
  SetPitch(job->voice->pitch);
  SetThroat(job->voice->throat);
  SetMouth(job->voice->mouth);
  EnableSingmode(0);
  SetInput(in);
  SAMMain(job->out, job->ctx);
  job->pieces++;
}

int samSay(const char *text, const SamVoice *voice, void (*out)(void *, unsigned char), void *ctx,
           int (*stop)(void *)) {
  Job job = {voice, out, ctx, stop, 0};
  const char *s = text;
  while (*s && !(stop && stop(ctx))) {
    int len, cut, best = -1, i;
    while (*s == ' ') s++;
    if (!*s) break;
    len = (int)strlen(s);
    cut = len < CHUNK_MAX ? len : CHUNK_MAX;
    if (len > CHUNK_MAX) {
      // Prefer a sentence or clause end, then a space.
      for (i = 0; i < cut; i++)
        if (strchr(".?!;:,", s[i])) best = i + 1;
      if (best < 0)
        for (i = cut - 1; i > 0; i--)
          if (s[i] == ' ') { best = i; break; }
      if (best > 0) cut = best;
    }
    piece(&job, s, cut, 0);
    s += cut;
  }
  return job.pieces;
}
