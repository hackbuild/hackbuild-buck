#include "mqtt_codec.h"

#include <string.h>

namespace mqtt {
namespace {

size_t strField(const char *s) { return 2 + (s ? strlen(s) : 0); }

struct Writer {
  uint8_t *buf;
  size_t cap;
  size_t len = 0;
  bool ok = true;
  Writer(uint8_t *b, size_t c) : buf(b), cap(c) {}
  void u8(uint8_t v) { if (len + 1 > cap) { ok = false; return; } buf[len++] = v; }
  void u16(uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
  void bytes(const void *p, size_t n) {
    if (len + n > cap) { ok = false; return; }
    if (n) memcpy(buf + len, p, n);
    len += n;
  }
  void str(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF) { ok = false; return; }
    u16((uint16_t)n);
    bytes(s, n);
  }
  void header(uint8_t first, size_t remaining) {
    if (remaining > 268435455) { ok = false; return; }
    u8(first);
    do {
      uint8_t b = remaining % 128;
      remaining /= 128;
      if (remaining) b |= 0x80;
      u8(b);
    } while (remaining);
  }
  size_t done() const { return ok ? len : 0; }
};

}  // namespace

size_t encodeConnect(uint8_t *out, size_t cap, const char *clientId, uint16_t keepaliveSec, const Will &will) {
  bool hasWill = will.topic && will.topic[0];
  size_t body = 10 + strField(clientId);
  if (hasWill) body += strField(will.topic) + strField(will.message);
  Writer w(out, cap);
  w.header(CONNECT << 4, body);
  w.str("MQTT");
  w.u8(4);                                      // protocol level 3.1.1
  uint8_t flags = 0x02;                         // clean session
  if (hasWill) flags |= 0x04 | (will.retain ? 0x20 : 0);
  w.u8(flags);
  w.u16(keepaliveSec);
  w.str(clientId);
  if (hasWill) { w.str(will.topic); w.str(will.message); }
  return w.done();
}

size_t encodeSubscribe(uint8_t *out, size_t cap, uint16_t packetId, const char *topic) {
  Writer w(out, cap);
  w.header(SUBSCRIBE << 4 | 0x02, 2 + strField(topic) + 1);
  w.u16(packetId);
  w.str(topic);
  w.u8(0);                                      // QoS 0
  return w.done();
}

size_t encodePublish(uint8_t *out, size_t cap, const char *topic, const uint8_t *payload, size_t len, bool retain) {
  Writer w(out, cap);
  w.header(PUBLISH << 4 | (retain ? 1 : 0), strField(topic) + len);
  w.str(topic);
  w.bytes(payload, len);
  return w.done();
}

size_t encodePingreq(uint8_t *out, size_t cap) {
  Writer w(out, cap);
  w.header(PINGREQ << 4, 0);
  return w.done();
}

size_t encodeDisconnect(uint8_t *out, size_t cap) {
  Writer w(out, cap);
  w.header(DISCONNECT << 4, 0);
  return w.done();
}

void Reader::reset() {
  state_ = HEADER;
  have_ = 0;
  remaining_ = 0;
  malformed = false;
}

bool Reader::finish() {
  pkt_.type = header_ >> 4;
  pkt_.flags = header_ & 0x0F;
  pkt_.body = buf_;
  pkt_.len = have_;
  state_ = HEADER;
  return true;
}

bool Reader::push(uint8_t byte) {
  switch (state_) {
    case HEADER:
      header_ = byte;
      remaining_ = 0;
      multiplier_ = 1;
      lengthBytes_ = 0;
      have_ = 0;
      state_ = LENGTH;
      return false;
    case LENGTH:
      remaining_ += (uint32_t)(byte & 0x7F) * multiplier_;
      multiplier_ *= 128;
      if (++lengthBytes_ > 4) { malformed = true; state_ = HEADER; return false; }
      if (byte & 0x80) return false;
      if (remaining_ == 0) return finish();
      if (remaining_ > cap_) { dropped++; state_ = SKIP; return false; }
      state_ = BODY;
      return false;
    case BODY:
      buf_[have_++] = byte;
      return have_ == remaining_ ? finish() : false;
    case SKIP:
      if (--remaining_ == 0) state_ = HEADER;
      return false;
  }
  return false;
}

bool parsePublish(const Packet &p, Publish &out) {
  out = Publish();
  if (p.type != PUBLISH || p.len < 2) return false;
  out.qos = (p.flags >> 1) & 0x03;
  out.retain = p.flags & 0x01;
  uint16_t tl = (uint16_t)(p.body[0] << 8 | p.body[1]);
  size_t off = 2 + (size_t)tl;
  if (off > p.len) return false;
  out.topic = (const char *)p.body + 2;
  out.topicLen = tl;
  if (out.qos > 0) {
    if (off + 2 > p.len) return false;
    out.packetId = (uint16_t)(p.body[off] << 8 | p.body[off + 1]);
    off += 2;
  }
  out.payload = p.body + off;
  out.payloadLen = p.len - off;
  return true;
}

int parseConnack(const Packet &p) {
  if (p.type != CONNACK || p.len != 2) return -1;
  return p.body[1];
}

bool parseSuback(const Packet &p, uint16_t &packetId, uint8_t &granted) {
  if (p.type != SUBACK || p.len < 3) return false;
  packetId = (uint16_t)(p.body[0] << 8 | p.body[1]);
  granted = p.body[2];
  return true;
}

}  // namespace mqtt
