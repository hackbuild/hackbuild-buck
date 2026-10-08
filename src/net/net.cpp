#include "net.h"

#include <Arduino.h>
#include <WiFi.h>

#include "../config.h"
#include "../health.h"
#include "../log.h"
#include "../remote.h"
#include "../settings.h"
#include "../speech.h"
#include "links.h"

namespace {

ClaspLink claspLink;
MqttLink mqttLink;
bool started = false;
uint32_t offlineSince = 0, lastWifiKick = 0;

bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

void buildInfo(char *out, size_t cap) {
  RemoteStats rs = remoteStats();
  snprintf(out, cap,
           "{\"fw\":\"%s\",\"id\":\"%s\",\"up\":%lu,\"heap\":%lu,\"minHeap\":%lu,\"rssi\":%d,"
           "\"reset\":\"%s\",\"crashes\":%lu,\"queue\":%d,\"heard\":%lu,\"limited\":%lu,\"voice\":\"%s\"}",
           FW_VERSION, cfg.id, (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getMinFreeHeap(), (int)WiFi.RSSI(), healthResetReason(),
           (unsigned long)healthCrashCount(), speechQueued(), (unsigned long)rs.accepted, (unsigned long)rs.limited,
           cfg.voice);
}

void publishInfo() {
  char info[256];
  buildInfo(info, sizeof(info));
  claspLink.setParam("info", info);
  mqttLink.publish("info", info);
}

void netTask(void *) {
  healthWatchThisTask();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  char host[40];
  snprintf(host, sizeof(host), "buck-%s", cfg.id);
  WiFi.setHostname(host);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);
  // Full power WiFi bursts draw about 300 mA. With the amp and servo peaking at the
  // same moment that browns out a USB port. 8.5 dBm is also the known fix for the
  // C3 SuperMini's antenna layout, which connects better at lower power.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  logLine("net", "joining %s as %s", cfg.ssid, host);

  claspLink.begin(cfg.claspUrl, cfg.id, cfg.tlsVerify);
  mqttLink.begin(cfg.mqttHost, (uint16_t)cfg.mqttPort, cfg.id);

  bool wasUp = false, wasSpeaking = false, jawIdleSent = true;
  LinkState claspWas = LINK_OFF, mqttWas = LINK_OFF;
  uint32_t lastInfo = 0, lastJaw = 0;
  static SpeechEvent ev;                 // 400 bytes, kept off the stack
  offlineSince = millis();

  for (;;) {
    healthFeed();
    uint32_t now = millis();
    bool up = WiFi.status() == WL_CONNECTED;

    if (!up) {
      if (wasUp) {
        logLine("net", "WiFi lost");
        claspLink.stop();
        mqttLink.stop();
        offlineSince = now;
      }
      wasUp = false;
      // Auto reconnect usually recovers on its own; this kick covers the times it stalls.
      if (due(now, lastWifiKick + WIFI_RETRY_MS) && due(now, offlineSince + WIFI_RETRY_MS)) {
        lastWifiKick = now;
        WiFi.disconnect();
        WiFi.begin(cfg.ssid, cfg.pass);
      }
      if (due(now, offlineSince + WIFI_REBOOT_MS) && !speechSpeaking()) {
        logLine("net", "offline for %lu min, restarting", WIFI_REBOOT_MS / 60000);
        delay(200);
        ESP.restart();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!wasUp) {
      logLine("net", "WiFi up, %s, rssi %d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      wasUp = true;
    }

    claspLink.service(now);
    mqttLink.service(millis());
    now = millis();

    // A link that came up on this pass gets the full picture.
    bool claspFresh = claspLink.state() == LINK_READY && claspWas != LINK_READY;
    bool mqttFresh = mqttLink.state() == LINK_READY && mqttWas != LINK_READY;
    claspWas = claspLink.state();
    mqttWas = mqttLink.state();
    if (claspFresh || mqttFresh) {
      claspLink.setParam("speaking", speechSpeaking());
      mqttLink.publish("speaking", speechSpeaking() ? "true" : "false");
      lastInfo = now - INFO_PERIOD_MS;   // publish info right away
    }

    // What BUCK is saying, and whether it is talking.
    if (speechTakeEvent(ev)) {
      claspLink.setParam("said", ev.text);
      mqttLink.publish("said", ev.text);
    }
    bool speaking = speechSpeaking();
    if (speaking != wasSpeaking) {
      wasSpeaking = speaking;
      claspLink.setParam("speaking", speaking);
      mqttLink.publish("speaking", speaking ? "true" : "false");
    }

    // Jaw position stream for anything animating along, CLASP only.
    if (speaking && due(now, lastJaw + JAW_STREAM_MS)) {
      lastJaw = now;
      claspLink.streamJaw(speechJawLevel());
      jawIdleSent = false;
    } else if (!speaking && !jawIdleSent) {
      claspLink.streamJaw(0);
      jawIdleSent = true;
    }

    if (due(now, lastInfo + INFO_PERIOD_MS)) {
      lastInfo = now;
      publishInfo();
    }

    vTaskDelay(pdMS_TO_TICKS(speaking ? 5 : 15));
  }
}

}  // namespace

void netBegin() {
  if (healthSafeMode()) {
    logLine("net", "safe mode: networking off after %lu crashes in a row, /reboot to try again",
            (unsigned long)healthCrashCount());
    return;
  }
  if (!cfg.ssid[0]) {
    logLine("net", "no WiFi set, send: /wifi <ssid> <password>");
    return;
  }
  started = true;
  xTaskCreate(netTask, "net", 8192, nullptr, 2, nullptr);
}

void netReport(char *out, size_t cap) {
  if (!started) {
    snprintf(out, cap, "[net] off (%s)", healthSafeMode() ? "safe mode" : cfg.ssid[0] ? "not started" : "no WiFi set");
    return;
  }
  bool up = WiFi.status() == WL_CONNECTED;
  RemoteStats rs = remoteStats();
  snprintf(out, cap,
           "[net] wifi %s %s rssi %d\n"
           "[net] clasp %s %s  connects %lu rx %lu tx %lu  %s\n"
           "[net] mqtt  %s %s:%d  connects %lu rx %lu tx %lu  %s\n"
           "[net] remote heard %lu, limited %lu, queue full %lu, empty %lu",
           cfg.ssid, up ? WiFi.localIP().toString().c_str() : "(down)", up ? (int)WiFi.RSSI() : 0,
           linkStateName(claspLink.state()), cfg.claspUrl, (unsigned long)claspLink.connects,
           (unsigned long)claspLink.rx, (unsigned long)claspLink.tx, claspLink.lastError(),
           linkStateName(mqttLink.state()), cfg.mqttHost, cfg.mqttPort, (unsigned long)mqttLink.connects,
           (unsigned long)mqttLink.rx, (unsigned long)mqttLink.tx, mqttLink.lastError(), (unsigned long)rs.accepted,
           (unsigned long)rs.limited, (unsigned long)rs.busy, (unsigned long)rs.empty);
}
