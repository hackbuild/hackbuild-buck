// SAM on the host, through the same samTame/samSay path the firmware uses.
// A line of the letter o hung the deer on 2026-10-07: SAM's InsertBreath looped
// forever on a long word, and fuzzing found five more ways in. Every input here
// runs under an alarm, so a hang fails the test instead of stalling it.
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>

extern "C" {
#include "SamData.h"
#include "sam_say.h"
}

SamData *samdata;
static long samples;
static const char *current;

static void onAlarm(int) {
  const char msg[] = "\nSAM hung on input: ";
  write(2, msg, sizeof(msg) - 1);
  write(2, current, strlen(current));
  _exit(3);
}

static void count(void *, unsigned char) { samples++; }

static long speak(const char *text) {
  static char tamed[700];
  current = text;
  samples = 0;
  samTame(text, tamed, sizeof(tamed));
  SamVoice v = {80, 72, 128, 128};
  alarm(10);
  samSay(tamed, &v, count, nullptr, nullptr);
  alarm(0);
  return samples;
}

static void repeat(char *buf, char c, int n) {
  memset(buf, c, n);
  buf[n] = 0;
}

void test_tame() {
  char out[128];
  samTame("soooooo cool!!!!!", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("sooo cool!!!", out);
  samTame("supercalifragilisticexpialidocious", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("supercalifragi listicexpialid ocious", out);
  samTame("h\x01i <there> #1 & $5", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("h i  there   1   5", out);
  samTame("abcdef", out, 4);
  TEST_ASSERT_EQUAL_STRING("ab", out);   // stops early, leaving room for a split space and the NUL
}

void test_ordinary_lines_speak() {
  TEST_ASSERT_GREATER_THAN(10000, speak("Hello there. I am Buck, the talking deer."));
  TEST_ASSERT_GREATER_THAN(10000, speak("Call me at 4805551234 or 6025559876 tomorrow, please."));
}

void test_the_letter_o() {
  char buf[512];
  for (int n = 1; n <= 400; n += 23) {
    repeat(buf, 'o', n);
    TEST_ASSERT_GREATER_THAN(0, speak(buf));
  }
  repeat(buf, 'o', 200);
  memcpy(buf, "b", 1);
  TEST_ASSERT_GREATER_THAN(0, speak(buf));
}

void test_long_words_and_runs() {
  char buf[512];
  const char letters[] = "aeiouymnrlsz";
  for (const char *c = letters; *c; c++) {
    repeat(buf, *c, 300);
    speak(buf);
  }
  speak("supercalifragilisticexpialidocious antidisestablishmentarianism pneumonoultramicroscopicsilicovolcanoconiosis");
  speak("1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890");
  speak("o?'oo!.,ooo?o-oooo  o..'?ooooo??o!'?oo!ooo o'o!''ooooooo o-oo'?ooo'-'o,oo!o',-oo!oo o?'-  oo,!,o!oo?ooo,  "
        "oo?o- oooo.oooooooo-,!-.!!.,..!o'-o.o?o!o!ooooo oo?oo.o- .!oo'-??o-!!o.o,o.'?");
  TEST_PASS();
}

void test_random_lines() {
  const char *alphabets[] = {"o", "oO", "aeiou", "abcdefghijklmnopqrstuvwxyz     ", "oooooooo .,!?'-",
                             "0123456789 ", "mmmnnnrrrlll", " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"};
  unsigned long rng = 20261007;
  char buf[256];
  for (int it = 0; it < 600; it++) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    const char *a = alphabets[(rng >> 33) % 8];
    int al = strlen(a), len = 1 + (int)((rng >> 40) % 200);
    for (int k = 0; k < len; k++) {
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      buf[k] = a[(rng >> 33) % al];
    }
    buf[len] = 0;
    speak(buf);
  }
  TEST_PASS();
}

int main() {
  samdata = (SamData *)calloc(1, sizeof(SamData));
  signal(SIGALRM, onAlarm);
  UNITY_BEGIN();
  RUN_TEST(test_tame);
  RUN_TEST(test_ordinary_lines_speak);
  RUN_TEST(test_the_letter_o);
  RUN_TEST(test_long_words_and_runs);
  RUN_TEST(test_random_lines);
  return UNITY_END();
}
