#include "remote.h"

#include <Arduino.h>

#include "log.h"
#include "settings.h"
#include "text_filter.h"

namespace {

buck::RateLimiter bucket(REMOTE_BURST, REMOTE_REFILL_MS);
RemoteStats stats = {};

}  // namespace

const char *remoteResultName(RemoteResult r) {
  switch (r) {
    case REMOTE_OK: return "ok";
    case REMOTE_EMPTY: return "empty";
    case REMOTE_LIMITED: return "rate limited";
    case REMOTE_BUSY: return "queue full";
    default: return "remote off";
  }
}

RemoteResult remoteSay(Source src, const uint8_t *text, size_t len) {
  if (!cfg.remote) return REMOTE_OFF;
  char clean[REMOTE_TEXT_MAX + 1];
  if (!buck::cleanText(text, len, clean, sizeof(clean))) {
    stats.empty++;
    return REMOTE_EMPTY;
  }
  if (speechQueuedRemote() >= REMOTE_QUEUE_MAX) {
    stats.busy++;
    logLine(sourceName(src), "dropped, queue full: %s", clean);
    return REMOTE_BUSY;
  }
  if (!bucket.take(millis())) {
    stats.limited++;
    logLine(sourceName(src), "dropped, rate limited: %s", clean);
    return REMOTE_LIMITED;
  }
  if (!speechSay(clean, src)) {
    stats.busy++;
    return REMOTE_BUSY;
  }
  stats.accepted++;
  return REMOTE_OK;
}

RemoteResult remoteVoice(Source src, const uint8_t *name, size_t len) {
  if (!cfg.remote) return REMOTE_OFF;
  char clean[16];
  if (!buck::cleanText(name, len, clean, sizeof(clean))) return REMOTE_EMPTY;
  if (!speechSetVoice(clean)) return REMOTE_EMPTY;
  logLine(sourceName(src), "voice %s", clean);
  return REMOTE_OK;   // not saved: a remote voice change lasts until reboot
}

RemoteStats remoteStats() { return stats; }
