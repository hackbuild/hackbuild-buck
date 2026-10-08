#include "speech.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <driver/gpio.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include "health.h"
#include "log.h"
#include "settings.h"

extern "C" {
#include "SamData.h"
#include "reciter.h"
#include "sam.h"
extern void (*samFrameHook)(unsigned char f1, unsigned char a1, unsigned char a2, unsigned char flags);
}

// SAM's working memory. sam.c, render.c and reciter.c reach it through this pointer.
SamData *samdata = nullptr;

namespace {

const int CHUNK_MAX = 80;          // characters per SAM call; its phoneme buffer is 256 bytes

struct Utterance {
  Source src;
  char text[TEXT_MAX + 1];
};

struct JawEvent {
  uint32_t at;                     // sample index in the audio stream
  uint8_t open;                    // 0..255
};

struct Voice {
  const char *name;
  uint8_t speed, pitch, throat, mouth;
};
const Voice VOICES[] = {
    {"sam", 72, 64, 128, 128},    {"elf", 72, 64, 110, 160},     {"robot", 92, 60, 190, 190},
    {"stuffy", 82, 72, 110, 105}, {"oldlady", 82, 32, 145, 145}, {"et", 100, 64, 150, 200},
};

I2SClass i2s;
QueueHandle_t textQ;
QueueHandle_t eventQ;
QueueHandle_t jawQ;
QueueHandle_t jawCmdQ;
StreamBufferHandle_t audio;

volatile bool stopReq = false;
volatile bool speaking = false;
volatile int remoteWaiting = 0;
portMUX_TYPE remoteLock = portMUX_INITIALIZER_UNLOCKED;

void remoteDone() {
  taskENTER_CRITICAL(&remoteLock);
  if (remoteWaiting > 0) remoteWaiting = remoteWaiting - 1;
  taskEXIT_CRITICAL(&remoteLock);
}
volatile float jawLevel = 0;
volatile int lastUs = -1;
uint32_t produced = 0;             // synth task only
uint32_t eventSeq = 0;

// ------------------------------------------------------------ servo, player task only

bool servoOn = false;

void servoWriteRaw(int us) {
  us = constrain(us, SERVO_MIN_US, SERVO_MAX_US);
  if (!servoOn) { ledcAttach(PIN_SERVO, 50, 14); servoOn = true; }
  if (us != lastUs) {
    ledcWrite(PIN_SERVO, (uint32_t)us * ((1 << 14) - 1) / 20000);
    lastUs = us;
  }
}

// Inside the calibrated swing only. Uncalibrated: does nothing.
void servoWriteJaw(int us) {
  if (!jawCalibrated()) return;
  int lo = min(cfg.openUs, cfg.closedUs), hi = max(cfg.openUs, cfg.closedUs);
  servoWriteRaw(constrain(us, lo, hi));
}

void servoRelease() {
  if (!servoOn) return;
  ledcWrite(PIN_SERVO, 0);
  ledcDetach(PIN_SERVO);
  servoOn = false;
  gpio_set_level((gpio_num_t)PIN_SERVO, 0);
  gpio_set_direction((gpio_num_t)PIN_SERVO, GPIO_MODE_OUTPUT);
}

// Exactly n pulses, bit banged, then the line goes quiet. One 20 ms frame lets an
// SG90 travel about 12 degrees, so a single pulse is a safe first contact.
void servoPulses(int us, int n) {
  servoRelease();
  us = constrain(us, SERVO_MIN_US, SERVO_MAX_US);
  for (int i = 0; i < n; i++) {
    gpio_set_level((gpio_num_t)PIN_SERVO, 1);
    delayMicroseconds(us);
    gpio_set_level((gpio_num_t)PIN_SERVO, 0);
    vTaskDelay(pdMS_TO_TICKS(20 - us / 1000));
  }
  lastUs = us;
}

int jawUs(float f) {
  f = constrain(f, 0.0f, 1.0f);
  return cfg.closedUs + (int)lroundf((cfg.openUs - cfg.closedUs) * f);
}

// What talking uses for "closed": a hair toward open from the calibrated contact
// point, so the servo is not pushing the jaw into the head between syllables.
int talkUs(float f) {
  int span = cfg.openUs - cfg.closedUs;
  int margin = min(JAW_CLOSED_MARGIN_US, abs(span) / 4);
  int closed = cfg.closedUs + (span > 0 ? margin : -margin);
  f = constrain(f, 0.0f, 1.0f);
  return closed + (int)lroundf((cfg.openUs - closed) * f);
}

// One step of a slow glide toward `target`: about 30 degrees per second.
void glideToward(int target, bool raw) {
  int from = lastUs;
  int next = target;
  if (from >= 0 && abs(target - from) > 7) next = from + (target > from ? 7 : -7);
  if (raw) servoWriteRaw(next);
  else servoWriteJaw(next);
}

// ------------------------------------------------------------ synth task

uint8_t outBuf[256];
int outN = 0;

void flushOut() {
  if (!outN) return;
  if (!stopReq) {
    xStreamBufferSend(audio, outBuf, outN, portMAX_DELAY);
    produced += outN;
  }
  outN = 0;
}

void samByte(void *, unsigned char b) {
  outBuf[outN++] = b;
  if (outN == (int)sizeof(outBuf)) flushOut();
}

// Called by SAM at the start of every frame with the values that make its sound.
void samFrame(unsigned char f1, unsigned char a1, unsigned char, unsigned char flags) {
  if (stopReq) return;
  float o;
  if (flags & 248) {
    o = JAW_UNVOICED;                                   // sampled, unvoiced consonant
  } else {
    float f = constrain((f1 - F1_SHUT) / float(F1_WIDE - F1_SHUT), 0.0f, 1.0f);
    float a = constrain((a1 & 15) / float(AMP_FULL), 0.0f, 1.0f);
    o = f * a;
  }
  JawEvent e = {produced + (uint32_t)outN, (uint8_t)(o * 255)};
  xQueueSend(jawQ, &e, portMAX_DELAY);
}

void sayChunk(const char *txt) {
  static char in[256];
  int n = 0;
  for (const char *p = txt; *p && n < CHUNK_MAX; p++) in[n++] = toupper((unsigned char)*p);
  in[n] = 0;
  strcat(in, "[");
  memset(samdata, 0, sizeof(SamData));
  if (!TextToPhonemes(in)) {
    logLine("say", "could not pronounce: %s", txt);
    return;
  }
  SetSpeed(cfg.speed);
  SetPitch(cfg.pitch);
  SetThroat(cfg.throat);
  SetMouth(cfg.mouth);
  EnableSingmode(0);
  SetInput(in);
  SAMMain(samByte, nullptr);
  flushOut();
}

// Keep what SAM's reciter knows (letters, digits, a little punctuation), then cut
// into chunks at sentence ends, then commas, then spaces.
void sayLine(char *line) {
  for (char *p = line; *p; p++) {
    unsigned char c = *p;
    if (!(isalnum(c) || strchr(" .,?!'-:;", c))) *p = ' ';
  }
  char *s = line;
  while (*s && !stopReq) {
    while (*s == ' ') s++;
    if (!*s) break;
    int len = strlen(s), cut = min(len, CHUNK_MAX);
    if (len > CHUNK_MAX) {
      int best = -1;
      for (int i = 0; i < cut; i++)
        if (strchr(".?!;:,", s[i])) best = i + 1;
      if (best < 0)
        for (int i = cut - 1; i > 0; i--)
          if (s[i] == ' ') { best = i; break; }
      if (best > 0) cut = best;
    }
    char save = s[cut];
    s[cut] = 0;
    sayChunk(s);
    s[cut] = save;
    s += cut;
  }
}

void toneSamples(int hz, int ms) {
  int total = (long)SAMPLE_RATE * ms / 1000;
  float ph = 0, inc = 2 * PI * hz / SAMPLE_RATE;
  for (int i = 0; i < total && !stopReq; i++) {
    samByte(nullptr, (uint8_t)(128 + 38 * sinf(ph)));
    ph += inc;
    if (ph > 2 * PI) ph -= 2 * PI;
  }
  flushOut();
}

void synthTask(void *) {
  static Utterance u;              // static: 400 bytes stay off this task's stack
  static SpeechEvent ev;
  samFrameHook = samFrame;
  for (;;) {
    xQueueReceive(textQ, &u, portMAX_DELAY);
    if (u.src == SRC_CLASP || u.src == SRC_MQTT) remoteDone();
    stopReq = false;
    speaking = true;
    if (u.text[0] == '\x01') {
      int hz = 440, ms = 1000;
      sscanf(u.text + 1, "%d %d", &hz, &ms);
      toneSamples(hz, ms);
      continue;
    }
    ev.seq = ++eventSeq;
    ev.src = u.src;
    strlcpy(ev.text, u.text, sizeof(ev.text));
    xQueueOverwrite(eventQ, &ev);
    logLine("say", "(%s) %s", sourceName(u.src), u.text);
    sayLine(u.text);
  }
}

// ------------------------------------------------------------ player task

enum JawMode : uint8_t { MODE_SPEECH, MODE_HOLD_FRACTION, MODE_HOLD_US, MODE_SWEEP };

void playerTask(void *) {
  static uint8_t blk[BLOCK_SAMPLES];
  static int16_t st[BLOCK_SAMPLES * 2];
  const float att = 1 - expf(-20.0f / JAW_ATTACK_MS), rel = 1 - expf(-20.0f / JAW_RELEASE_MS);
  uint32_t consumed = 0, idleMs = 1000;
  float lvl = 0, tgt = 0;
  JawMode mode = MODE_SPEECH;
  uint32_t modeUntil = 0, sweepStart = 0;
  float holdFraction = 0;
  int holdUs = 0;
  bool warnedUncalibrated = false;

  healthWatchThisTask();
  for (;;) {
    healthFeed();

    // Console jaw commands.
    JawCommand c;
    while (xQueueReceive(jawCmdQ, &c, 0) == pdTRUE) {
      uint32_t now = millis();
      modeUntil = c.holdMs ? now + c.holdMs : 0;
      switch (c.kind) {
        case JawCommand::HOLD_FRACTION: mode = MODE_HOLD_FRACTION; holdFraction = c.fraction; break;
        case JawCommand::HOLD_US: mode = MODE_HOLD_US; holdUs = c.us; break;
        case JawCommand::SWEEP: mode = MODE_SWEEP; sweepStart = now; modeUntil = now + 3000; break;
        case JawCommand::PROBE: mode = MODE_SPEECH; servoPulses(c.us, c.pulses); break;
        case JawCommand::RELEASE: mode = MODE_SPEECH; servoRelease(); idleMs = 1000; lvl = 0; break;
      }
    }

    // Audio.
    size_t n = xStreamBufferReceive(audio, blk, BLOCK_SAMPLES, pdMS_TO_TICKS(20));
    if (n) {
      if (idleMs >= 100) {
        // Start of speech: 60 ms of silence so the jaw gets the same head start as
        // the rest of the line, and speech takes the jaw back from any hold.
        memset(st, 0, sizeof(st));
        for (int i = 0; i < 3; i++) i2s.write((uint8_t *)st, sizeof(st));
        if (mode == MODE_HOLD_US || (mode != MODE_SPEECH && modeUntil == 0)) mode = MODE_SPEECH;
      }
      idleMs = 0;
      consumed += n;
      uint32_t horizon = consumed + (uint32_t)(cfg.leadMs * SAMPLE_RATE / 1000);
      JawEvent e;
      bool got = false;
      float mx = 0;
      while (xQueuePeek(jawQ, &e, 0) == pdTRUE && (int32_t)(e.at - horizon) <= 0) {
        xQueueReceive(jawQ, &e, 0);
        mx = max(mx, e.open / 255.0f);
        got = true;
      }
      if (got) tgt = mx;
      if (stopReq) {
        tgt = 0;                                         // drain without playing
      } else {
        int g = cfg.vol * 256 / 100;
        for (size_t i = 0; i < BLOCK_SAMPLES; i++) {
          int s = i < n ? ((int)blk[i] - 128) * g : 0;
          st[2 * i] = st[2 * i + 1] = (int16_t)constrain(s, -32768, 32767);
        }
        i2s.write((uint8_t *)st, sizeof(st));
      }
    } else {
      idleMs += 20;
      if (idleMs >= 100) {
        tgt = 0;
        if (uxQueueMessagesWaiting(textQ) == 0) speaking = false;
        JawEvent e;
        while (xQueueReceive(jawQ, &e, 0) == pdTRUE) {
        }
      }
    }

    // Jaw.
    uint32_t now = millis();
    if (mode != MODE_SPEECH && modeUntil && (int32_t)(now - modeUntil) >= 0) {
      mode = MODE_SPEECH;
      idleMs = min(idleMs, (uint32_t)100);               // fall back through the release timer
    }
    lvl += (tgt - lvl) * (tgt > lvl ? att : rel);

    switch (mode) {
      case MODE_HOLD_US:
        glideToward(holdUs, true);
        break;
      case MODE_HOLD_FRACTION:
        glideToward(jawUs(holdFraction), false);
        break;
      case MODE_SWEEP: {
        float t = (now - sweepStart) / 3000.0f;
        servoWriteJaw(jawUs(0.5f - 0.5f * cosf(min(t, 1.0f) * 2 * PI)));
        break;
      }
      case MODE_SPEECH:
        if (!jawCalibrated()) {
          if (idleMs == 0 && !warnedUncalibrated) {
            logLine("jaw", "not calibrated, the jaw stays still (see /help)");
            warnedUncalibrated = true;
          }
          servoRelease();
        } else if (idleMs < 600 || lvl > 0.02f) {
          servoWriteJaw(talkUs(lvl));
        } else {
          servoRelease();                                // quiet: no buzz, no heat
        }
        break;
    }
    jawLevel = mode == MODE_SPEECH ? lvl
               : jawCalibrated() && lastUs >= 0
                   ? constrain((lastUs - cfg.closedUs) / float(cfg.openUs - cfg.closedUs), 0.0f, 1.0f)
                   : 0;
  }
}

}  // namespace

// ------------------------------------------------------------ public

const char *sourceName(Source s) {
  switch (s) {
    case SRC_SERIAL: return "serial";
    case SRC_CLASP: return "clasp";
    case SRC_MQTT: return "mqtt";
    default: return "system";
  }
}

void speechBegin() {
  samdata = (SamData *)calloc(1, sizeof(SamData));
  textQ = xQueueCreate(SPEECH_QUEUE_LEN, sizeof(Utterance));
  eventQ = xQueueCreate(1, sizeof(SpeechEvent));
  jawQ = xQueueCreate(128, sizeof(JawEvent));
  jawCmdQ = xQueueCreate(4, sizeof(JawCommand));
  audio = xStreamBufferCreate(AUDIO_BUFFER_BYTES, BLOCK_SAMPLES);
  if (!samdata || !textQ || !eventQ || !jawQ || !jawCmdQ || !audio) {
    logLine("speech", "out of memory at start, restarting");
    delay(1000);
    ESP.restart();
  }

  i2s.setPins(PIN_BCLK, PIN_LRCLK, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    logLine("speech", "I2S failed to start, no audio");

  // Player above the synth, both above the network task, so WiFi and TLS work
  // never starves the speaker.
  xTaskCreate(playerTask, "player", 4096, nullptr, 5, nullptr);
  xTaskCreate(synthTask, "synth", 8192, nullptr, 3, nullptr);
}

// Called from the console (loop task) and the network task. The 400 byte
// Utterance lives on the caller's stack; both stacks are sized for it.
bool speechSay(const char *text, Source src) {
  if (!text || !*text) return false;
  bool remote = src == SRC_CLASP || src == SRC_MQTT;
  taskENTER_CRITICAL(&remoteLock);
  bool full = remote && remoteWaiting >= REMOTE_QUEUE_MAX;
  if (!full && remote) remoteWaiting = remoteWaiting + 1;
  taskEXIT_CRITICAL(&remoteLock);
  if (full) return false;
  Utterance u;
  u.src = src;
  strlcpy(u.text, text, sizeof(u.text));
  if (xQueueSend(textQ, &u, 0) == pdTRUE) return true;
  if (remote) remoteDone();
  return false;
}

bool speechTone(int hz, int ms) {
  char buf[24];
  snprintf(buf, sizeof(buf), "\x01%d %d", constrain(hz, 50, 8000), constrain(ms, 10, 10000));
  return speechSay(buf, SRC_SYSTEM);
}

void speechStop() {
  xQueueReset(textQ);
  taskENTER_CRITICAL(&remoteLock);
  remoteWaiting = 0;
  taskEXIT_CRITICAL(&remoteLock);
  stopReq = true;
}

bool speechSpeaking() { return speaking; }
int speechQueued() { return (int)uxQueueMessagesWaiting(textQ); }
int speechQueuedRemote() { return remoteWaiting; }
float speechJawLevel() { return jawLevel; }
bool speechTakeEvent(SpeechEvent &out) { return xQueueReceive(eventQ, &out, 0) == pdTRUE; }

bool speechSetVoice(const char *name) {
  for (const Voice &v : VOICES) {
    if (strcasecmp(name, v.name)) continue;
    cfg.speed = v.speed;
    cfg.pitch = v.pitch;
    cfg.throat = v.throat;
    cfg.mouth = v.mouth;
    strlcpy(cfg.voice, v.name, sizeof(cfg.voice));
    return true;
  }
  return false;
}

const char *speechVoiceList() { return "sam elf robot stuffy oldlady et"; }

bool jawCommand(const JawCommand &c) { return xQueueSend(jawCmdQ, &c, pdMS_TO_TICKS(50)) == pdTRUE; }
int jawLastUs() { return lastUs; }
