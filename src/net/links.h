// The two ways the internet reaches BUCK. Both run inside the network task.
//
// ClaspLink speaks CLASP v3 over WebSocket (wss://relay.clasp.to by default):
//   listens  /hackbuild/buck/<id>/say    event or param, string: text to speak
//            /hackbuild/buck/<id>/voice  event or param, string: voice name
//   writes   /hackbuild/buck/<id>/status/...  params, plus a jaw stream
//
// MqttLink speaks MQTT 3.1.1 over TCP (relay.clasp.chat:1883 by default), which is
// the same CLASP relay's MQTT door, so `curl -d text mqtt://...` works:
//   listens  hackbuild/buck/<id>/say, hackbuild/buck/<id>/voice
//   writes   hackbuild/buck/<id>/status/...
//
// Each link reconnects on its own with exponential backoff and jitter, pings
// when idle, and drops a connection that goes quiet for LINK_DEAD_MS. Each also
// publishes a heartbeat to its own status/beat and listens for the echo; no echo
// for BEAT_DEAD_MS means the relay dropped the subscription, so it reconnects.
#pragma once

#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <stdint.h>

#include "mqtt_codec.h"
#include "ws_client.h"

enum LinkState : uint8_t { LINK_OFF, LINK_WAIT, LINK_HANDSHAKE, LINK_READY };
const char *linkStateName(LinkState s);

class Backoff {
 public:
  void reset() { delay_ = 0; }
  uint32_t next();             // milliseconds to wait before the next attempt
  uint32_t current() const { return delay_; }

 private:
  uint32_t delay_ = 0;
};

class ClaspLink {
 public:
  void begin(const char *url, const char *id, bool tlsVerify);
  void service(uint32_t now);
  void stop();
  LinkState state() const { return state_; }
  const char *lastError() const { return err_; }
  uint32_t connects = 0, rx = 0, tx = 0, beats = 0;

  void setParam(const char *leaf, const char *text);
  void setParam(const char *leaf, bool v);
  void streamJaw(float v);

 private:
  bool connectNow();
  void onFrame(const uint8_t *f, size_t n);
  void send(size_t n);
  void fail(const char *why);

  NetworkClientSecure tls_;
  NetworkClient plain_;
  uint8_t rxBuf_[2048];        // largest CLASP frame accepted; bigger ones are dropped
  uint8_t txBuf_[640];
  uint8_t frame_[600];
  WsClient ws_{rxBuf_, sizeof(rxBuf_), txBuf_, sizeof(txBuf_)};

  bool enabled_ = false, secure_ = true, verify_ = true;
  char host_[64] = "";
  char path_[32] = "/";
  uint16_t port_ = 443;
  char root_[64] = "";         // /hackbuild/buck/<id>
  char sayAddr_[80] = "";
  char voiceAddr_[80] = "";
  char beatAddr_[80] = "";

  LinkState state_ = LINK_OFF;
  uint32_t nextTry_ = 0, since_ = 0, lastTx_ = 0, beatSent_ = 0, beatSeen_ = 0;
  Backoff backoff_;
  const char *err_ = "";
};

class MqttLink {
 public:
  void begin(const char *host, uint16_t port, const char *id);
  void service(uint32_t now);
  void stop();
  LinkState state() const { return state_; }
  const char *lastError() const { return err_; }
  uint32_t connects = 0, rx = 0, tx = 0, beats = 0;

  void publish(const char *leaf, const char *text);

 private:
  bool connectNow();
  void onPacket(const mqtt::Packet &p);
  void fail(const char *why);
  bool sendRaw(size_t n);
  void beatFromProbe(uint32_t now);

  NetworkClient tcp_;
  NetworkClient probe_;        // short lived second connection that delivers the heartbeat
  uint8_t rxBuf_[512];
  uint8_t txBuf_[512];
  mqtt::Reader reader_{rxBuf_, sizeof(rxBuf_)};

  bool enabled_ = false;
  char host_[64] = "";
  uint16_t port_ = 1883;
  char clientId_[48] = "";
  char root_[64] = "";         // hackbuild/buck/<id>
  char sayTopic_[80] = "";
  char voiceTopic_[80] = "";
  char willTopic_[96] = "";
  char beatTopic_[80] = "";
  bool sayLive_ = false, voiceLive_ = false, beatLive_ = false;   // SUBACK seen: replays are over

  LinkState state_ = LINK_OFF;
  uint32_t nextTry_ = 0, since_ = 0, lastTx_ = 0, lastRx_ = 0, beatSent_ = 0, beatSeen_ = 0;
  Backoff backoff_;
  const char *err_ = "";
};
