// BUCK: a talking deer head. ESP32-C3 SuperMini, SG90 jaw servo, NS4168 I2S amp.
//
// Text arrives over USB serial, CLASP (WebSocket) or MQTT. SAM speaks it, and the
// jaw follows the first formant of every 10 ms frame. See README.md and docs/.
//
// Tasks, highest priority first:
//   player (5)  stream buffer -> I2S, moves the jaw          speech.cpp
//   synth  (3)  text queue -> SAM -> stream buffer           speech.cpp
//   net    (2)  WiFi, CLASP and MQTT links, status           net/net.cpp
//   loop   (1)  serial console                               console.cpp

#include <Arduino.h>
#include <driver/gpio.h>

#include "config.h"
#include "console.h"
#include "health.h"
#include "log.h"
#include "net/net.h"
#include "settings.h"
#include "speech.h"

// GPIO4 to 7 leave reset with weak pull-ups (they are the C3's JTAG pins). A high
// servo line drives the jaw toward closed, so pull every output low as early as
// the app can run code. A 10k pull-down on GPIO4 covers the bootloader before this.
__attribute__((constructor)) static void earlyPinsLow() {
  const gpio_num_t pins[] = {(gpio_num_t)PIN_SERVO, (gpio_num_t)PIN_BCLK, (gpio_num_t)PIN_LRCLK, (gpio_num_t)PIN_DIN};
  for (gpio_num_t p : pins) {
    gpio_set_level(p, 0);
    gpio_set_direction(p, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never stall when no computer is reading the USB port
  healthBegin();
  settingsLoad();
  speechBegin();

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  consoleBanner();

  netBegin();
  enableLoopWDT();
}

void loop() {
  consolePoll();
  if (healthTick()) netBegin();   // safe mode ended on this pass
  delay(5);
}
