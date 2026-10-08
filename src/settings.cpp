#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "config.h"

Settings cfg;

namespace {

const char *NS = "buck";

int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void loadStr(Preferences &p, const char *key, char *dst, size_t cap, const char *fallback) {
  String v = p.getString(key, fallback);
  strlcpy(dst, v.c_str(), cap);
}

}  // namespace

bool validId(const char *id) {
  size_t n = strlen(id);
  if (n < 1 || n > 31) return false;
  for (size_t i = 0; i < n; i++) {
    char c = id[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
  }
  return true;
}

bool jawCalibrated() {
  return cfg.openUs >= SERVO_MIN_US && cfg.openUs <= SERVO_MAX_US && cfg.closedUs >= SERVO_MIN_US &&
         cfg.closedUs <= SERVO_MAX_US && cfg.openUs != cfg.closedUs;
}

void settingsLoad() {
  Preferences p;
  p.begin(NS, true);   // read only; fails quietly on a fresh chip and every get returns its default

  cfg.openUs = p.getInt("open", BUCK_DEFAULT_OPEN_US);
  cfg.closedUs = p.getInt("closed", BUCK_DEFAULT_CLOSED_US);
  cfg.leadMs = clampInt(p.getInt("lead", 10), -60, 200);
  cfg.vol = clampInt(p.getInt("vol", 50), 0, 100);
  cfg.speed = clampInt(p.getInt("speed", 72), 1, 255);
  cfg.pitch = clampInt(p.getInt("pitch", 64), 1, 255);
  cfg.throat = clampInt(p.getInt("throat", 128), 1, 255);
  cfg.mouth = clampInt(p.getInt("mouth", 128), 1, 255);
  loadStr(p, "voice", cfg.voice, sizeof(cfg.voice), "sam");

  loadStr(p, "id", cfg.id, sizeof(cfg.id), BUCK_DEFAULT_ID);
  loadStr(p, "ssid", cfg.ssid, sizeof(cfg.ssid), BUCK_DEFAULT_SSID);
  loadStr(p, "pass", cfg.pass, sizeof(cfg.pass), BUCK_DEFAULT_PASS);
  loadStr(p, "clasp", cfg.claspUrl, sizeof(cfg.claspUrl), BUCK_DEFAULT_CLASP_URL);
  loadStr(p, "mqtt", cfg.mqttHost, sizeof(cfg.mqttHost), BUCK_DEFAULT_MQTT_HOST);
  cfg.mqttPort = clampInt(p.getInt("mqttport", BUCK_DEFAULT_MQTT_PORT), 1, 65535);
  cfg.tlsVerify = p.getBool("tls", true);
  cfg.remote = p.getBool("remote", true);
  p.end();

  if (!validId(cfg.id)) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(cfg.id, sizeof(cfg.id), "buck-%02x%02x%02x", mac[3], mac[4], mac[5]);
  }
}

void settingsPutInt(const char *key, int value) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putInt(key, value);
  p.end();
}

void settingsPutStr(const char *key, const char *value) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putString(key, value);
  p.end();
}

void settingsPutBool(const char *key, bool value) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putBool(key, value);
  p.end();
}

void settingsFactoryReset() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.clear();
  p.end();
}
