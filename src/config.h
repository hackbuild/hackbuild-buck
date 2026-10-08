// Fixed facts about the hardware, and the defaults a build starts from.
// Anything a person changes at runtime lives in settings.h instead.
#pragma once

#define FW_VERSION "1.0.0"

// Pins on the ESP32-C3 SuperMini. GPIO2, 8 and 9 are strapping pins (8 is the
// onboard LED, 9 is BOOT) and 20/21 are the UART, so none of those are used.
#define PIN_SERVO 4
#define PIN_BCLK 5
#define PIN_LRCLK 6
#define PIN_DIN 7

// Audio. SAM renders 8 bit mono at 22050 Hz; the player works in 20 ms blocks.
#define SAMPLE_RATE 22050
#define BLOCK_SAMPLES 441
#define AUDIO_BUFFER_BYTES 8192   // about 370 ms between the synth and the player

// Text limits. Serial gets the long limit, anything from the network the short one.
#define TEXT_MAX 400
#define REMOTE_TEXT_MAX 200
#define SPEECH_QUEUE_LEN 8
#define REMOTE_QUEUE_MAX 4        // remote lines allowed to wait at once
#define REMOTE_BURST 4            // token bucket shared by every remote sender
#define REMOTE_REFILL_MS 3000

// Jaw shaping from SAM's frame data (README, "how the jaw follows speech").
#define F1_SHUT 6
#define F1_WIDE 24
#define AMP_FULL 13
#define JAW_UNVOICED 0.15f
#define JAW_ATTACK_MS 30
#define JAW_RELEASE_MS 70
#define JAW_CLOSED_MARGIN_US 10  // talking stops this short of the closed end so the jaw never presses (and stalls) against the head
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500

// Build defaults. platformio.ini sets these per environment; empty means "ask".
#ifndef BUCK_DEFAULT_ID
#define BUCK_DEFAULT_ID ""        // empty: buck-<last 3 bytes of the MAC>
#endif
#ifndef BUCK_DEFAULT_SSID
#define BUCK_DEFAULT_SSID ""
#endif
#ifndef BUCK_DEFAULT_PASS
#define BUCK_DEFAULT_PASS ""
#endif
#ifndef BUCK_DEFAULT_OPEN_US
#define BUCK_DEFAULT_OPEN_US 0    // 0: jaw not calibrated, the servo stays still
#endif
#ifndef BUCK_DEFAULT_CLOSED_US
#define BUCK_DEFAULT_CLOSED_US 0
#endif
#ifndef BUCK_DEFAULT_CLASP_URL
#define BUCK_DEFAULT_CLASP_URL "wss://relay.clasp.to"
#endif
#ifndef BUCK_DEFAULT_MQTT_HOST
#define BUCK_DEFAULT_MQTT_HOST "relay.clasp.chat"
#endif
#ifndef BUCK_DEFAULT_MQTT_PORT
#define BUCK_DEFAULT_MQTT_PORT 1883
#endif

// Every address BUCK uses sits under /hackbuild/buck/<id>/ in CLASP and
// hackbuild/buck/<id>/ in MQTT.
#define ADDR_ROOT "hackbuild/buck"

// Network timing.
#define WIFI_RETRY_MS 30000           // re-issue WiFi.begin after this long without a link
#define WIFI_REBOOT_MS (20UL * 60000) // reboot after this long offline, if quiet
#define LINK_CONNECT_TIMEOUT_MS 8000
#define LINK_HANDSHAKE_TIMEOUT_MS 10000
#define LINK_PING_MS 20000
#define LINK_DEAD_MS 65000            // nothing heard for this long: reconnect
#define LINK_BACKOFF_MIN_MS 2000
#define LINK_BACKOFF_MAX_MS 120000
#define INFO_PERIOD_MS 60000
#define JAW_STREAM_MS 66              // jaw position stream rate while talking, ~15 Hz
#define TLS_MIN_HEAP 45000            // skip a TLS connect when the largest free block is smaller

// Crash handling.
#define SAFE_MODE_CRASHES 3           // this many crash resets in a row: boot without networking
#define CRASH_CLEAR_MS (5UL * 60000)  // uptime that clears the crash count
#define WDT_TIMEOUT_MS 30000
