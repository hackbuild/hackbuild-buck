#include "text_filter.h"

namespace buck {

size_t cleanText(const uint8_t *in, size_t len, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  bool space = true;                     // swallow leading whitespace
  auto put = [&](char c) {
    if (c == ' ') {
      if (space) return;
      space = true;
    } else {
      space = false;
    }
    if (n + 1 < cap) out[n++] = c;
  };
  for (size_t i = 0; in && i < len; i++) {
    uint8_t c = in[i];
    if (c >= 0x20 && c < 0x7F) { put((char)c); continue; }
    if (c < 0x80) { put(' '); continue; }  // control bytes, tabs, newlines
    // UTF-8: E2 80 98/99 are single quotes, 9C/9D double quotes, A6 an ellipsis.
    if (c == 0xE2 && i + 2 < len && in[i + 1] == 0x80) {
      uint8_t d = in[i + 2];
      i += 2;
      if (d == 0x98 || d == 0x99) put('\'');
      else if (d == 0x9C || d == 0x9D) put('"');
      else if (d == 0xA6) { put('.'); put('.'); put('.'); }
      else put(' ');
      continue;
    }
    // Any other multibyte sequence: skip its continuation bytes, leave a space.
    while (i + 1 < len && (in[i + 1] & 0xC0) == 0x80) i++;
    put(' ');
  }
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = 0;
  return n;
}

RateLimiter::RateLimiter(uint8_t capacity, uint32_t refillMs)
    : capacity_(capacity), refillMs_(refillMs ? refillMs : 1), tokens_(capacity), last_(0) {}

bool RateLimiter::take(uint32_t nowMs) {
  if (!started_) { started_ = true; last_ = nowMs; }
  uint32_t earned = (nowMs - last_) / refillMs_;
  if (earned) {
    uint32_t t = tokens_ + earned;
    tokens_ = t > capacity_ ? capacity_ : (uint8_t)t;
    last_ += earned * refillMs_;
    if (tokens_ == capacity_) last_ = nowMs;
  }
  if (!tokens_) return false;
  tokens_--;
  return true;
}

}  // namespace buck
