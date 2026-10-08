#include "links.h"

#include <Arduino.h>
#include <esp_random.h>

#include "../config.h"
#include "../health.h"
#include "../log.h"
#include "../remote.h"
#include "ca_roots.h"
#include "clasp_codec.h"

const char *linkStateName(LinkState s) {
  switch (s) {
    case LINK_OFF: return "off";
    case LINK_WAIT: return "waiting";
    case LINK_HANDSHAKE: return "handshake";
    default: return "ready";
  }
}

uint32_t Backoff::next() {
  delay_ = delay_ ? min(delay_ * 2, (uint32_t)LINK_BACKOFF_MAX_MS) : LINK_BACKOFF_MIN_MS;
  uint32_t jitter = esp_random() % (delay_ / 4 + 1);   // up to +25%, so many BUCKs do not retry in step
  return delay_ + jitter;
}

namespace {

bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

// A remote value becomes text: strings as they are, numbers printed.
size_t valueText(const clasp::Value &v, char *out, size_t cap) {
  switch (v.type) {
    case clasp::VAL_STRING: {
      size_t n = v.len < cap - 1 ? v.len : cap - 1;
      memcpy(out, v.str, n);
      out[n] = 0;
      return n;
    }
    case clasp::VAL_I8: case clasp::VAL_I16: case clasp::VAL_I32: case clasp::VAL_I64:
      return snprintf(out, cap, "%lld", (long long)v.i);
    case clasp::VAL_F32: case clasp::VAL_F64:
      return snprintf(out, cap, "%g", v.f);
    default:
      return 0;
  }
}

}  // namespace

// ================================================================ CLASP

void ClaspLink::begin(const char *url, const char *id, bool tlsVerify) {
  stop();
  enabled_ = false;
  const char *rest;
  if (!strncmp(url, "wss://", 6)) { secure_ = true; port_ = 443; rest = url + 6; }
  else if (!strncmp(url, "ws://", 5)) { secure_ = false; port_ = 80; rest = url + 5; }
  else { state_ = LINK_OFF; err_ = "no url"; return; }

  const char *slash = strchr(rest, '/');
  size_t hostLen = slash ? (size_t)(slash - rest) : strlen(rest);
  if (hostLen == 0 || hostLen >= sizeof(host_)) { err_ = "bad url"; return; }
  memcpy(host_, rest, hostLen);
  host_[hostLen] = 0;
  char *colon = strchr(host_, ':');
  if (colon) { *colon = 0; port_ = (uint16_t)atoi(colon + 1); }
  strlcpy(path_, slash ? slash : "/", sizeof(path_));

  snprintf(root_, sizeof(root_), "/%s/%s", ADDR_ROOT, id);
  snprintf(sayAddr_, sizeof(sayAddr_), "%s/say", root_);
  snprintf(voiceAddr_, sizeof(voiceAddr_), "%s/voice", root_);
  snprintf(beatAddr_, sizeof(beatAddr_), "%s/status/beat", root_);
  verify_ = tlsVerify;
  enabled_ = true;
  state_ = LINK_WAIT;
  nextTry_ = millis();
  backoff_.reset();
}

void ClaspLink::stop() {
  ws_.close();
  state_ = enabled_ ? LINK_WAIT : LINK_OFF;
}

void ClaspLink::fail(const char *why) {
  err_ = why;
  ws_.close();
  state_ = LINK_WAIT;
  uint32_t wait = backoff_.next();
  nextTry_ = millis() + wait;
  logLine("clasp", "%s, retry in %lus", why, (unsigned long)(wait / 1000));
}

void ClaspLink::send(size_t n) {
  if (!n) return;
  if (!ws_.sendBinary(frame_, n)) { fail(ws_.error()); return; }
  tx++;
  lastTx_ = millis();
}

bool ClaspLink::connectNow() {
  if (secure_) {
    if (ESP.getMaxAllocHeap() < TLS_MIN_HEAP) { fail("low memory, TLS skipped"); return false; }
    tls_.setHandshakeTimeout(LINK_HANDSHAKE_TIMEOUT_MS / 1000);
    if (verify_) tls_.setCACert(CA_ROOTS_PEM);
    else tls_.setInsecure();
  }
  NetworkClient &net = secure_ ? (NetworkClient &)tls_ : plain_;
  healthFeed();
  bool ok = ws_.open(net, host_, port_, path_, "clasp", LINK_CONNECT_TIMEOUT_MS, LINK_HANDSHAKE_TIMEOUT_MS);
  healthFeed();
  if (!ok) {
    fail(ws_.error());
    return false;
  }
  char name[48];
  snprintf(name, sizeof(name), "BUCK %s", root_ + strlen(ADDR_ROOT) + 2);
  state_ = LINK_HANDSHAKE;
  since_ = millis();
  send(clasp::encodeHello(frame_, sizeof(frame_), name, ""));
  return state_ == LINK_HANDSHAKE;
}

void ClaspLink::service(uint32_t now) {
  if (state_ == LINK_OFF) return;
  if (state_ == LINK_WAIT) {
    if (due(now, nextTry_)) connectNow();
    return;
  }
  for (int i = 0; i < 8; i++) {                 // a few messages per pass, then yield
    int n = ws_.poll();
    if (n < 0) { fail(ws_.error()); return; }
    if (n == 0) break;
    rx++;
    onFrame(ws_.message(), (size_t)n);
    if (state_ == LINK_WAIT) return;
  }
  now = millis();
  if (state_ == LINK_HANDSHAKE && !due(now, since_ + LINK_HANDSHAKE_TIMEOUT_MS)) return;
  if (state_ == LINK_HANDSHAKE) { fail("no WELCOME from relay"); return; }
  if (due(now, ws_.lastRxMs() + LINK_DEAD_MS)) { fail("relay went quiet"); return; }
  if (due(now, beatSeen_ + BEAT_DEAD_MS)) { fail("heartbeat echo lost, relay dropped the subscription"); return; }
  if (due(now, beatSent_ + BEAT_MS)) {
    beatSent_ = now;
    send(clasp::encodeSetInt(frame_, sizeof(frame_), beatAddr_, (int64_t)(now / 1000)));
    if (state_ != LINK_READY) return;
  }
  if (due(now, lastTx_ + LINK_PING_MS)) {
    if (!ws_.sendPing()) { fail("ping failed"); return; }
    lastTx_ = now;
  }
}

void ClaspLink::onFrame(const uint8_t *f, size_t n) {
  clasp::Message m;
  if (!clasp::decode(f, n, m)) return;           // malformed: ignore it, keep the link
  switch (m.type) {
    case clasp::MSG_WELCOME:
      if (state_ != LINK_HANDSHAKE) return;
      state_ = LINK_READY;
      connects++;
      backoff_.reset();
      err_ = "";
      // send() calls fail() and drops to LINK_WAIT on error; stop at the first one
      // so a dead socket costs one backoff step, not three.
      send(clasp::encodeSubscribe(frame_, sizeof(frame_), 1, sayAddr_));
      if (state_ != LINK_READY) return;
      send(clasp::encodeSubscribe(frame_, sizeof(frame_), 2, voiceAddr_));
      if (state_ != LINK_READY) return;
      send(clasp::encodeSubscribe(frame_, sizeof(frame_), 3, beatAddr_));
      if (state_ != LINK_READY) return;
      beatSeen_ = millis();
      beatSent_ = beatSeen_ - BEAT_MS + 3000;      // first beat a few seconds in
      setParam("online", true);
      if (state_ != LINK_READY) return;
      logLine("clasp", "ready on %s, listening at %s", host_, sayAddr_);
      return;
    case clasp::MSG_PUBLISH:
    case clasp::MSG_SET: {
      if (state_ != LINK_READY || !m.hasValue) return;
      if (clasp::addrIs(m.addr, m.addrLen, beatAddr_)) {
        beatSeen_ = millis();
        beats++;
        return;
      }
      char text[REMOTE_TEXT_MAX + 1];
      size_t len = valueText(m.value, text, sizeof(text));
      if (clasp::addrIs(m.addr, m.addrLen, sayAddr_)) {
        RemoteResult r = remoteSay(SRC_CLASP, (const uint8_t *)text, len);
        if (r != REMOTE_OK) logLine("clasp", "say refused: %s", remoteResultName(r));
      } else if (clasp::addrIs(m.addr, m.addrLen, voiceAddr_)) {
        remoteVoice(SRC_CLASP, (const uint8_t *)text, len);
      }
      return;
    }
    case clasp::MSG_PING:
      send(clasp::encodePong(frame_, sizeof(frame_)));
      return;
    case clasp::MSG_ERROR:
      logLine("clasp", "relay error %u: %.*s", m.errCode, (int)m.errLen, m.errMsg ? m.errMsg : "");
      if (state_ == LINK_HANDSHAKE) fail("refused by relay");   // later errors refuse one message only
      return;
    default:
      return;                                     // SNAPSHOT replays, ACKs: nothing to do
  }
}

void ClaspLink::setParam(const char *leaf, const char *text) {
  if (state_ != LINK_READY) return;
  char addr[96];
  snprintf(addr, sizeof(addr), "%s/status/%s", root_, leaf);
  send(clasp::encodeSetString(frame_, sizeof(frame_), addr, text));
}

void ClaspLink::setParam(const char *leaf, bool v) {
  if (state_ != LINK_READY) return;
  char addr[96];
  snprintf(addr, sizeof(addr), "%s/status/%s", root_, leaf);
  send(clasp::encodeSetBool(frame_, sizeof(frame_), addr, v));
}

void ClaspLink::streamJaw(float v) {
  if (state_ != LINK_READY) return;
  char addr[96];
  snprintf(addr, sizeof(addr), "%s/status/jaw", root_);
  send(clasp::encodeStreamFloat(frame_, sizeof(frame_), addr, v));
}

// ================================================================ MQTT

void MqttLink::begin(const char *host, uint16_t port, const char *id) {
  stop();
  enabled_ = host && *host;
  if (!enabled_) { state_ = LINK_OFF; err_ = "no host"; return; }
  strlcpy(host_, host, sizeof(host_));
  port_ = port;
  snprintf(root_, sizeof(root_), "%s/%s", ADDR_ROOT, id);
  snprintf(clientId_, sizeof(clientId_), "hackbuild-buck-%s", id);
  snprintf(sayTopic_, sizeof(sayTopic_), "%s/say", root_);
  snprintf(voiceTopic_, sizeof(voiceTopic_), "%s/voice", root_);
  snprintf(willTopic_, sizeof(willTopic_), "%s/status/online", root_);
  snprintf(beatTopic_, sizeof(beatTopic_), "%s/status/beat", root_);
  state_ = LINK_WAIT;
  nextTry_ = millis();
  backoff_.reset();
}

void MqttLink::stop() {
  if (tcp_.connected()) {
    uint8_t b[2];
    size_t n = mqtt::encodeDisconnect(b, sizeof(b));
    tcp_.write(b, n);
  }
  tcp_.stop();
  state_ = enabled_ ? LINK_WAIT : LINK_OFF;
}

void MqttLink::fail(const char *why) {
  err_ = why;
  tcp_.stop();
  state_ = LINK_WAIT;
  uint32_t wait = backoff_.next();
  nextTry_ = millis() + wait;
  logLine("mqtt", "%s, retry in %lus", why, (unsigned long)(wait / 1000));
}

bool MqttLink::sendRaw(size_t n) {
  if (!n) return false;
  size_t off = 0;
  uint32_t deadline = millis() + 5000;
  while (off < n) {
    if (!tcp_.connected()) { fail("socket closed"); return false; }
    size_t w = tcp_.write(txBuf_ + off, n - off);
    if (!w) {
      if (due(millis(), deadline)) { fail("write timed out"); return false; }
      vTaskDelay(1);
      continue;
    }
    off += w;
  }
  tx++;
  lastTx_ = millis();
  return true;
}

bool MqttLink::connectNow() {
  healthFeed();
  bool ok = tcp_.connect(host_, port_, LINK_CONNECT_TIMEOUT_MS);
  healthFeed();
  if (!ok) { fail("connect failed"); return false; }
  tcp_.setConnectionTimeout(3000);                 // bounds every later write to 3 s
  tcp_.setNoDelay(true);
  reader_.reset();
  sayLive_ = voiceLive_ = beatLive_ = false;
  mqtt::Will will;
  will.topic = willTopic_;
  will.message = "false";
  will.retain = true;
  state_ = LINK_HANDSHAKE;
  since_ = lastRx_ = millis();
  return sendRaw(mqtt::encodeConnect(txBuf_, sizeof(txBuf_), clientId_, 60, will));
}

void MqttLink::service(uint32_t now) {
  if (state_ == LINK_OFF) return;
  if (state_ == LINK_WAIT) {
    if (due(now, nextTry_)) connectNow();
    return;
  }
  if (!tcp_.connected() && !tcp_.available()) { fail("connection lost"); return; }
  for (int budget = 1024; budget > 0 && tcp_.available() > 0; budget--) {
    int c = tcp_.read();
    if (c < 0) break;
    lastRx_ = millis();
    if (reader_.push((uint8_t)c)) {
      onPacket(reader_.packet());
      if (state_ == LINK_WAIT) return;
    }
    if (reader_.malformed) { fail("malformed packet"); return; }
  }
  now = millis();
  if (state_ == LINK_HANDSHAKE) {
    if (due(now, since_ + LINK_HANDSHAKE_TIMEOUT_MS)) fail("no CONNACK from broker");
    return;
  }
  if (due(now, lastRx_ + LINK_DEAD_MS)) { fail("broker went quiet"); return; }
  if (due(now, beatSeen_ + BEAT_DEAD_MS)) { fail("heartbeat echo lost, broker dropped the subscription"); return; }
  if (due(now, beatSent_ + BEAT_MS)) {
    beatSent_ = now;
    beatFromProbe(now);
  }
  if (due(now, lastTx_ + LINK_PING_MS)) sendRaw(mqtt::encodePingreq(txBuf_, sizeof(txBuf_)));
}

void MqttLink::onPacket(const mqtt::Packet &p) {
  rx++;
  switch (p.type) {
    case mqtt::CONNACK: {
      int rc = mqtt::parseConnack(p);
      if (rc != 0) { fail(rc < 0 ? "bad CONNACK" : "connection refused by broker"); return; }
      state_ = LINK_READY;
      connects++;
      backoff_.reset();
      err_ = "";
      if (!sendRaw(mqtt::encodeSubscribe(txBuf_, sizeof(txBuf_), 1, sayTopic_))) return;
      if (!sendRaw(mqtt::encodeSubscribe(txBuf_, sizeof(txBuf_), 2, voiceTopic_))) return;
      if (!sendRaw(mqtt::encodeSubscribe(txBuf_, sizeof(txBuf_), 3, beatTopic_))) return;
      beatSeen_ = millis();
      beatSent_ = beatSeen_ - BEAT_MS + 3000;
      publish("online", "true");
      logLine("mqtt", "ready on %s:%u, listening at %s", host_, port_, sayTopic_);
      return;
    }
    case mqtt::SUBACK: {
      uint16_t id;
      uint8_t granted;
      if (!mqtt::parseSuback(p, id, granted)) return;
      if (granted == 0x80) logLine("mqtt", "subscription %u refused", id);
      if (id == 1) sayLive_ = true;
      if (id == 2) voiceLive_ = true;
      if (id == 3) beatLive_ = true;
      return;
    }
    case mqtt::PUBLISH: {
      mqtt::Publish pub;
      if (!mqtt::parsePublish(p, pub)) return;
      bool say = pub.topicLen == strlen(sayTopic_) && !memcmp(pub.topic, sayTopic_, pub.topicLen);
      bool voice = pub.topicLen == strlen(voiceTopic_) && !memcmp(pub.topic, voiceTopic_, pub.topicLen);
      bool beat = pub.topicLen == strlen(beatTopic_) && !memcmp(pub.topic, beatTopic_, pub.topicLen);
      if (beat) {
        if (beatLive_) { beatSeen_ = millis(); beats++; }
        return;
      }
      // The relay replays stored values before it sends SUBACK, flagged as live
      // messages. Anything that arrives before our SUBACK is history: skip it.
      if (say && sayLive_ && !pub.retain) {
        RemoteResult r = remoteSay(SRC_MQTT, pub.payload, pub.payloadLen);
        if (r != REMOTE_OK) logLine("mqtt", "say refused: %s", remoteResultName(r));
      } else if (voice && voiceLive_ && !pub.retain) {
        remoteVoice(SRC_MQTT, pub.payload, pub.payloadLen);
      }
      return;
    }
    default:
      return;                                     // PINGRESP and the rest
  }
}

// The relay never delivers a client's own publishes back to it, so the heartbeat
// comes from a second, short lived connection: connect, publish, disconnect. The
// main connection receiving it proves the subscription is alive and counts as
// activity on the relay. Plain TCP, a few dozen bytes, bounded at about 3 s.
void MqttLink::beatFromProbe(uint32_t now) {
  healthFeed();
  bool ok = probe_.connect(host_, port_, 3000);    // also bounds the probe's writes and reads to 3 s
  healthFeed();
  if (!ok) return;                                 // a missed beat; three in a row reconnects
  probe_.setNoDelay(true);
  uint8_t buf[160];
  char id[64], n[16];
  snprintf(id, sizeof(id), "%s-beat", clientId_);
  snprintf(n, sizeof(n), "%lu", (unsigned long)(now / 1000));
  mqtt::Will none;
  size_t len = mqtt::encodeConnect(buf, sizeof(buf), id, 10, none);
  if (len && probe_.write(buf, len) == len) {
    // Read the CONNACK so the socket closes clean: closing with unread data makes
    // lwIP send a reset, which can throw away the PUBLISH still in flight.
    uint8_t ack[4];
    size_t got = 0;
    uint32_t deadline = millis() + 2000;
    while (got < sizeof(ack) && probe_.connected() && !due(millis(), deadline)) {
      int c = probe_.read();
      if (c < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      ack[got++] = (uint8_t)c;
    }
    if (got == 4 && ack[0] == 0x20 && ack[3] == 0) {
      len = mqtt::encodePublish(buf, sizeof(buf), beatTopic_, (const uint8_t *)n, strlen(n), false);
      if (len) probe_.write(buf, len);
      len = mqtt::encodeDisconnect(buf, sizeof(buf));
      probe_.write(buf, len);
      // The broker closes after DISCONNECT. Wait briefly for that so our side
      // closes second and nothing queued is lost.
      deadline = millis() + 500;
      while (probe_.connected() && !due(millis(), deadline)) {
        if (probe_.read() < 0) vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
  }
  probe_.stop();
}

void MqttLink::publish(const char *leaf, const char *text) {
  if (state_ != LINK_READY) return;
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/status/%s", root_, leaf);
  sendRaw(mqtt::encodePublish(txBuf_, sizeof(txBuf_), topic, (const uint8_t *)text, strlen(text), true));
}
