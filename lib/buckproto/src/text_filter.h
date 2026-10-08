// Input hygiene for anything BUCK is asked to say, plus a token bucket for
// rate limiting remote senders. No heap, no Arduino dependencies.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace buck {

// Turns arbitrary bytes (usually UTF-8) into plain printable ASCII: curly quotes
// become straight ones, other non-ASCII and control bytes become spaces, runs of
// whitespace collapse to one space, and the ends are trimmed. Writes at most
// cap - 1 characters plus a NUL. Returns the length written.
size_t cleanText(const uint8_t *in, size_t len, char *out, size_t cap);

// Token bucket: holds up to `capacity` tokens and earns one every `refillMs`.
class RateLimiter {
 public:
  RateLimiter(uint8_t capacity, uint32_t refillMs);
  bool take(uint32_t nowMs);
  uint8_t tokens() const { return tokens_; }

 private:
  uint8_t capacity_;
  uint32_t refillMs_;
  uint8_t tokens_;
  uint32_t last_;
  bool started_ = false;
};

}  // namespace buck
