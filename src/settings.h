// Settings that survive a reboot, kept in NVS under the "buck" namespace.
//
// Threading: numbers are read live by the audio tasks (32 bit loads are atomic on
// the C3). The network strings (id, ssid, pass, relay hosts) are only read at boot;
// changing one saves it and reboots, so no socket ever sees a string half written.
// `voice` is the exception: a name for display, rewritten by /voice and by remote
// voice changes, never used to make a decision.
#pragma once

#include <stdint.h>

struct Settings {
  // jaw
  int openUs;
  int closedUs;
  int leadMs;
  // voice
  int vol;
  int speed;
  int pitch;
  int throat;
  int mouth;
  char voice[12];
  // network
  char id[32];
  char ssid[33];
  char pass[65];
  char claspUrl[96];
  char mqttHost[64];
  int mqttPort;
  bool tlsVerify;
  bool remote;          // accept text from CLASP and MQTT
};

extern Settings cfg;

void settingsLoad();
void settingsPutInt(const char *key, int value);
void settingsPutStr(const char *key, const char *value);
void settingsPutBool(const char *key, bool value);
void settingsFactoryReset();

bool jawCalibrated();
bool validId(const char *id);
