// Speech and jaw.
//
// Two tasks. The synth task takes lines from a queue, renders them with SAM, and
// writes 8 bit samples into a stream buffer, plus a timestamped jaw openness for
// every 10 ms SAM frame. The player task drains the stream to I2S in 20 ms
// blocks and moves the jaw to the openness due at that point. The player task is
// the only code that touches the servo; everything else asks through jawCommand().
//
// No heap after speechBegin(): queues, buffers and SAM's working memory are
// allocated once there.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

enum Source : uint8_t { SRC_SERIAL = 0, SRC_CLASP = 1, SRC_MQTT = 2, SRC_SYSTEM = 3 };
const char *sourceName(Source s);

void speechBegin();

// Queue a line. Returns false when the queue is full. `text` is copied.
bool speechSay(const char *text, Source src);
bool speechTone(int hz, int ms);
void speechStop();                 // drop the queue and cut the current line
bool speechSpeaking();
int speechQueued();
int speechQueuedRemote();
float speechJawLevel();            // smoothed jaw openness 0..1, for the status stream

// Raised by the synth task when it starts a line; the network task reads them to
// publish what BUCK is saying. Holds the latest line only.
struct SpeechEvent {
  uint32_t seq;
  Source src;
  char text[TEXT_MAX + 1];
};
bool speechTakeEvent(SpeechEvent &out);

// Voices. Names are lowercase; applyVoice() also saves the choice.
bool speechSetVoice(const char *name);
const char *speechVoiceList();

// Jaw control from the console.
struct JawCommand {
  enum Kind : uint8_t { HOLD_FRACTION, HOLD_US, PROBE, SWEEP, RELEASE } kind;
  float fraction;      // HOLD_FRACTION: 0 closed .. 1 open
  int us;              // HOLD_US, PROBE
  int pulses;          // PROBE
  uint32_t holdMs;     // HOLD_*: 0 holds until the next command or speech
};
bool jawCommand(const JawCommand &c);
int jawLastUs();       // last pulse sent, -1 when unknown
