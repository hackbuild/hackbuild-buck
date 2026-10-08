// The one door remote messages come through. Anyone on the internet can reach
// BUCK, so every line from CLASP or MQTT is cleaned to plain ASCII, cut to
// REMOTE_TEXT_MAX, rate limited by a token bucket shared across both links, and
// refused when REMOTE_QUEUE_MAX lines are already waiting. Network task only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "speech.h"

enum RemoteResult : uint8_t { REMOTE_OK, REMOTE_EMPTY, REMOTE_LIMITED, REMOTE_BUSY, REMOTE_OFF };
const char *remoteResultName(RemoteResult r);

RemoteResult remoteSay(Source src, const uint8_t *text, size_t len);
RemoteResult remoteVoice(Source src, const uint8_t *name, size_t len);

// Counters for the status report.
struct RemoteStats {
  uint32_t accepted, limited, busy, empty;
};
RemoteStats remoteStats();
