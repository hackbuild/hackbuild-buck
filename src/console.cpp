#include "console.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "health.h"
#include "log.h"
#include "net/net.h"
#include "settings.h"
#include "speech.h"

namespace {

const size_t CONSOLE_LINE = TEXT_MAX + 64;

void out(const char *s) { Serial.println(s); }

// A value over 180 is a pulse width in microseconds; otherwise degrees.
int parseUs(float v) { return v > 180 ? (int)v : SERVO_MIN_US + (int)lroundf(v * (SERVO_MAX_US - SERVO_MIN_US) / 180); }

void showStatus() {
  logLine("status", "BUCK %s, id %s, up %lus, reset %s%s", FW_VERSION, cfg.id, (unsigned long)(millis() / 1000),
          healthResetReason(), healthSafeMode() ? ", SAFE MODE" : "");
  if (jawCalibrated())
    logLine("status", "jaw open %d us, closed %d us, lead %d ms", cfg.openUs, cfg.closedUs, cfg.leadMs);
  else
    logLine("status", "jaw not calibrated (open %d, closed %d), see /help", cfg.openUs, cfg.closedUs);
  logLine("status", "voice %s (speed %d pitch %d throat %d mouth %d), vol %d, %s, %d queued", cfg.voice, cfg.speed,
          cfg.pitch, cfg.throat, cfg.mouth, cfg.vol, speechSpeaking() ? "speaking" : "quiet", speechQueued());
  logLine("status", "heap %lu free, %lu lowest, %lu largest block", (unsigned long)ESP.getFreeHeap(),
          (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
}

void help() {
  out("Type a line and BUCK says it. Commands:");
  out("  talking   /vol <0-100>  /voice <sam|elf|robot|stuffy|oldlady|et>  /stop  /tone [hz] [ms]");
  out("            /speed /pitch /throat /mouth <1-255>");
  out("  jaw       /jaw <0-100>        hold the jaw at a percent open for 3 s");
  out("            /sweep              closed, open, closed");
  out("            /open <us|deg>      save the open end (over 180 means us)");
  out("            /closed <us|deg>    save the closed end");
  out("            /lead <ms>          jaw lead over the sound, default 10");
  out("  servo     /servo <us>         glide to a raw pulse and hold it, for fitting the horn");
  out("            /probe <us> [n]     n single pulses (default 1), then let go");
  out("            /release            stop driving the servo");
  out("  network   /wifi <ssid> <password>   /id <name>   /remote on|off");
  out("            /clasp <wss://host|off>   /mqtt <host[:port]|off>   /tls on|off");
  out("            /net                link status");
  out("  system    /status  /reboot  /factory");
  out("Network changes save and reboot.");
}

void saveAndReboot(const char *what) {
  logLine("console", "%s saved, rebooting", what);
  Serial.flush();
  delay(300);
  ESP.restart();
}

void command(char *line) {
  char *cmd = strtok(line + 1, " ");
  if (!cmd) return help();
  char *a = strtok(nullptr, " ");
  char *b = strtok(nullptr, "");          // rest of the line
  for (char *p = cmd; *p; p++) *p = tolower((unsigned char)*p);
  float v = a ? atof(a) : 0;

  if (!strcmp(cmd, "help")) return help();
  if (!strcmp(cmd, "status")) return showStatus();
  if (!strcmp(cmd, "net")) {
    char rep[640];
    netReport(rep, sizeof(rep));
    Serial.println(rep);
    return;
  }

  // talking
  if (!strcmp(cmd, "vol") && a) {
    cfg.vol = constrain((int)v, 0, 100);
    settingsPutInt("vol", cfg.vol);
    return logLine("console", "vol %d", cfg.vol);
  }
  if (!strcmp(cmd, "voice") && a) {
    if (!speechSetVoice(a)) return logLine("console", "voices: %s", speechVoiceList());
    settingsPutStr("voice", cfg.voice);
    settingsPutInt("speed", cfg.speed);
    settingsPutInt("pitch", cfg.pitch);
    settingsPutInt("throat", cfg.throat);
    settingsPutInt("mouth", cfg.mouth);
    return logLine("console", "voice %s", cfg.voice);
  }
  if ((!strcmp(cmd, "speed") || !strcmp(cmd, "pitch") || !strcmp(cmd, "throat") || !strcmp(cmd, "mouth")) && a) {
    int x = constrain((int)v, 1, 255);
    if (!strcmp(cmd, "speed")) cfg.speed = x;
    else if (!strcmp(cmd, "pitch")) cfg.pitch = x;
    else if (!strcmp(cmd, "throat")) cfg.throat = x;
    else cfg.mouth = x;
    settingsPutInt(cmd, x);
    return logLine("console", "%s %d", cmd, x);
  }
  if (!strcmp(cmd, "stop")) { speechStop(); return logLine("console", "stopped"); }
  if (!strcmp(cmd, "tone")) {
    speechTone(a ? (int)v : 440, b ? atoi(b) : 1000);
    return;
  }

  // jaw
  if (!strcmp(cmd, "jaw") && a) {
    if (!jawCalibrated()) return logLine("jaw", "calibrate first: /open and /closed");
    JawCommand c = {JawCommand::HOLD_FRACTION, constrain(v, 0.0f, 100.0f) / 100.0f, 0, 0, 3000};
    jawCommand(c);
    return logLine("jaw", "%d%% open", (int)v);
  }
  if (!strcmp(cmd, "sweep")) {
    if (!jawCalibrated()) return logLine("jaw", "calibrate first: /open and /closed");
    JawCommand c = {JawCommand::SWEEP, 0, 0, 0, 3000};
    jawCommand(c);
    return;
  }
  if ((!strcmp(cmd, "open") || !strcmp(cmd, "closed")) && a) {
    int us = constrain(parseUs(v), SERVO_MIN_US, SERVO_MAX_US);
    bool open = !strcmp(cmd, "open");
    (open ? cfg.openUs : cfg.closedUs) = us;
    settingsPutInt(open ? "open" : "closed", us);
    logLine("jaw", "%s end %d us", cmd, us);
    if (jawCalibrated()) {
      JawCommand c = {JawCommand::HOLD_FRACTION, open ? 1.0f : 0.0f, 0, 0, 3000};
      jawCommand(c);
    }
    return;
  }
  if (!strcmp(cmd, "lead") && a) {
    cfg.leadMs = constrain((int)v, -60, 200);
    settingsPutInt("lead", cfg.leadMs);
    return logLine("jaw", "lead %d ms", cfg.leadMs);
  }

  // servo, for calibration
  if (!strcmp(cmd, "servo") && a) {
    int us = constrain(parseUs(v), SERVO_MIN_US, SERVO_MAX_US);
    if (jawLastUs() < 0) logLine("servo", "position unknown, this first move is a jump");
    JawCommand c = {JawCommand::HOLD_US, 0, us, 0, 0};
    jawCommand(c);
    return logLine("servo", "holding %d us until /release or speech", us);
  }
  if (!strcmp(cmd, "probe") && a) {
    int n = b ? constrain(atoi(b), 1, 50) : 1;
    JawCommand c = {JawCommand::PROBE, 0, constrain(parseUs(v), SERVO_MIN_US, SERVO_MAX_US), n, 0};
    jawCommand(c);
    return logLine("servo", "%d pulse(s) at %d us, then released", n, c.us);
  }
  if (!strcmp(cmd, "release") || !strcmp(cmd, "off")) {
    JawCommand c = {JawCommand::RELEASE, 0, 0, 0, 0};
    jawCommand(c);
    return logLine("servo", "released");
  }

  // network
  if (!strcmp(cmd, "wifi") && a) {
    settingsPutStr("ssid", a);
    settingsPutStr("pass", b ? b : "");
    return saveAndReboot("WiFi");
  }
  if (!strcmp(cmd, "id") && a) {
    if (!validId(a)) return logLine("console", "id: 1 to 31 letters, digits, - or _");
    settingsPutStr("id", a);
    return saveAndReboot("id");
  }
  if (!strcmp(cmd, "clasp") && a) {
    bool off = !strcmp(a, "off");
    if (!off && strncmp(a, "ws://", 5) && strncmp(a, "wss://", 6)) return logLine("console", "clasp: wss://host or off");
    settingsPutStr("clasp", off ? "" : a);
    return saveAndReboot("CLASP relay");
  }
  if (!strcmp(cmd, "mqtt") && a) {
    bool off = !strcmp(a, "off");
    char host[64];
    strlcpy(host, off ? "" : a, sizeof(host));
    char *colon = strchr(host, ':');
    int port = BUCK_DEFAULT_MQTT_PORT;
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    settingsPutStr("mqtt", host);
    settingsPutInt("mqttport", constrain(port, 1, 65535));
    return saveAndReboot("MQTT broker");
  }
  if (!strcmp(cmd, "tls") && a) {
    settingsPutBool("tls", !strcmp(a, "on"));
    return saveAndReboot("TLS check");
  }
  if (!strcmp(cmd, "remote") && a) {
    cfg.remote = !strcmp(a, "on");
    settingsPutBool("remote", cfg.remote);
    return logLine("console", "remote messages %s", cfg.remote ? "on" : "off");
  }

  // system
  if (!strcmp(cmd, "reboot")) return saveAndReboot("nothing");
  if (!strcmp(cmd, "factory")) {
    settingsFactoryReset();
    return saveAndReboot("factory settings");
  }

  logLine("console", "unknown or incomplete: /%s (try /help)", cmd);
}

}  // namespace

void consoleBanner() {
  logLine("buck", "BUCK %s on ESP32-C3, id %s, reset: %s", FW_VERSION, cfg.id, healthResetReason());
  if (!jawCalibrated()) logLine("buck", "jaw not calibrated: it stays still until /open and /closed are set");
  logLine("buck", "type a line to hear it, /help for commands");
}

void consolePoll() {
  static char line[CONSOLE_LINE];
  static size_t len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\r' || ch == '\n') {
      line[len] = 0;
      if (len && !overflow) {
        if (line[0] == '/') command(line);
        else if (!speechSay(line, SRC_SERIAL)) logLine("console", "queue full, try /stop");
      } else if (overflow) {
        logLine("console", "line longer than %u characters, ignored", (unsigned)(CONSOLE_LINE - 1));
      }
      len = 0;
      overflow = false;
    } else if (len < CONSOLE_LINE - 1) {
      line[len++] = (char)ch;
    } else {
      overflow = true;
    }
  }
}
