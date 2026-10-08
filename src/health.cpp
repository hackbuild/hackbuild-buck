#include "health.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "log.h"

namespace {

const uint32_t MAGIC = 0xB0C4DEE5;
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcCrashes;

bool safeMode = false;
bool cleared = false;
esp_reset_reason_t reason = ESP_RST_UNKNOWN;

bool isCrash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

}  // namespace

void healthBegin() {
  reason = esp_reset_reason();
  if (rtcMagic != MAGIC) { rtcMagic = MAGIC; rtcCrashes = 0; }
  if (isCrash(reason)) rtcCrashes++;
  else if (reason != ESP_RST_SW) rtcCrashes = 0;   // power on, brownout, external reset
  safeMode = rtcCrashes >= SAFE_MODE_CRASHES;

  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = WDT_TIMEOUT_MS;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
}

bool healthSafeMode() { return safeMode; }
uint32_t healthCrashCount() { return rtcCrashes; }

const char *healthResetReason() {
  switch (reason) {
    case ESP_RST_POWERON: return "power";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int-wdt";
    case ESP_RST_TASK_WDT: return "task-wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT: return "pin";
    case ESP_RST_USB: return "usb";
    default: return "unknown";
  }
}

void healthTick() {
  if (!cleared && millis() > CRASH_CLEAR_MS) {
    cleared = true;
    if (rtcCrashes) logLine("health", "up %lu min, crash count cleared", CRASH_CLEAR_MS / 60000);
    rtcCrashes = 0;
  }
}

void healthWatchThisTask() { esp_task_wdt_add(nullptr); }
void healthFeed() { esp_task_wdt_reset(); }
