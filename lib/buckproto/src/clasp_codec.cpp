#include "clasp_codec.h"

#include <string.h>

namespace clasp {
namespace {

// Bounded big-endian writer. Once anything overflows, `ok` stays false.
struct Writer {
  uint8_t *buf;
  size_t cap;
  size_t len = 0;
  bool ok = true;

  Writer(uint8_t *b, size_t c) : buf(b), cap(c) {}
  void u8(uint8_t v) {
    if (len + 1 > cap) { ok = false; return; }
    buf[len++] = v;
  }
  void u16(uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
  void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
  void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
  void f64(double v) { uint64_t bits; memcpy(&bits, &v, 8); u64(bits); }
  void bytes(const void *p, size_t n) {
    if (len + n > cap) { ok = false; return; }
    memcpy(buf + len, p, n);
    len += n;
  }
  void str(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF) { ok = false; return; }
    u16((uint16_t)n);
    if (n) bytes(s, n);
  }
};

// Starts a frame: leaves room for the 4 byte header, finish() fills it in.
struct Frame {
  Writer w;
  uint8_t qos;
  Frame(uint8_t *out, size_t cap, uint8_t q) : w(out, cap), qos(q) {
    w.u8(MAGIC); w.u8(0); w.u16(0);
  }
  size_t finish() {
    if (!w.ok || w.len < HEADER) return 0;
    size_t payload = w.len - HEADER;
    if (payload > 0xFFFF) return 0;
    w.buf[1] = (uint8_t)((qos & 0x03) << 6) | 0x01;   // binary encoding
    w.buf[2] = (uint8_t)(payload >> 8);
    w.buf[3] = (uint8_t)(payload & 0xFF);
    return w.len;
  }
};

// Bounded big-endian reader. Any short read sets ok = false and returns zeros.
struct Reader {
  const uint8_t *p;
  const uint8_t *end;
  bool ok = true;

  Reader(const uint8_t *b, size_t n) : p(b), end(b + n) {}
  size_t left() const { return (size_t)(end - p); }
  bool need(size_t n) {
    if (!ok || left() < n) { ok = false; return false; }
    return true;
  }
  uint8_t u8() { if (!need(1)) return 0; return *p++; }
  uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)(p[0] << 8 | p[1]); p += 2; return v; }
  uint32_t u32() { uint32_t hi = u16(); return hi << 16 | u16(); }
  uint64_t u64() { uint64_t hi = u32(); return hi << 32 | u32(); }
  void skip(size_t n) { if (need(n)) p += n; }
  const char *str(uint16_t &n) {
    n = u16();
    if (!need(n)) { n = 0; return nullptr; }
    const char *s = (const char *)p;
    p += n;
    return s;
  }
};

const int MAX_DEPTH = 8;

bool readValue(Reader &r, uint8_t type, Value &v, int depth) {
  if (depth > MAX_DEPTH) return false;
  v.type = type;
  switch (type) {
    case VAL_NULL: return true;
    case VAL_BOOL: v.b = r.u8() != 0; break;
    case VAL_I8: v.i = (int8_t)r.u8(); break;
    case VAL_I16: v.i = (int16_t)r.u16(); break;
    case VAL_I32: v.i = (int32_t)r.u32(); break;
    case VAL_I64: v.i = (int64_t)r.u64(); break;
    case VAL_F32: { uint32_t bits = r.u32(); float f; memcpy(&f, &bits, 4); v.f = f; break; }
    case VAL_F64: { uint64_t bits = r.u64(); memcpy(&v.f, &bits, 8); break; }
    case VAL_STRING: v.str = r.str(v.len); break;
    case VAL_BYTES: v.str = r.str(v.len); break;
    case VAL_ARRAY: {
      uint16_t n = r.u16();
      for (uint16_t k = 0; k < n && r.ok; k++) {
        Value item;
        if (!readValue(r, r.u8(), item, depth + 1)) return false;
      }
      break;
    }
    case VAL_MAP: {
      uint16_t n = r.u16();
      for (uint16_t k = 0; k < n && r.ok; k++) {
        uint16_t kl;
        r.str(kl);
        Value item;
        if (!readValue(r, r.u8(), item, depth + 1)) return false;
      }
      break;
    }
    default: return false;
  }
  return r.ok;
}

}  // namespace

size_t encodeHello(uint8_t *out, size_t cap, const char *name, const char *token) {
  Frame f(out, cap, QOS_CONFIRM);
  f.w.u8(MSG_HELLO);
  f.w.u8(1);       // protocol version
  f.w.u8(0xE0);    // features: param, event, stream
  f.w.str(name);
  f.w.str(token);  // empty string when there is no token
  return f.finish();
}

size_t encodeSubscribe(uint8_t *out, size_t cap, uint32_t id, const char *pattern) {
  Frame f(out, cap, QOS_CONFIRM);
  f.w.u8(MSG_SUBSCRIBE);
  f.w.u32(id);
  f.w.str(pattern);
  f.w.u8(0xFF);    // every signal type
  f.w.u8(0);       // no options
  return f.finish();
}

size_t encodePublishString(uint8_t *out, size_t cap, Signal sig, const char *addr, const char *text) {
  Frame f(out, cap, sig == SIG_STREAM ? QOS_FIRE : QOS_CONFIRM);
  f.w.u8(MSG_PUBLISH);
  f.w.u8((uint8_t)((sig & 0x07) << 5));
  f.w.str(addr);
  f.w.u8(1);       // has value
  f.w.u8(VAL_STRING);
  f.w.str(text);
  return f.finish();
}

size_t encodeStreamFloat(uint8_t *out, size_t cap, const char *addr, double v) {
  Frame f(out, cap, QOS_FIRE);
  f.w.u8(MSG_PUBLISH);
  f.w.u8((uint8_t)(SIG_STREAM << 5));
  f.w.str(addr);
  f.w.u8(1);
  f.w.u8(VAL_F64);
  f.w.f64(v);
  return f.finish();
}

size_t encodeSetString(uint8_t *out, size_t cap, const char *addr, const char *text) {
  Frame f(out, cap, QOS_CONFIRM);
  f.w.u8(MSG_SET); f.w.u8(VAL_STRING); f.w.str(addr); f.w.str(text);
  return f.finish();
}

size_t encodeSetBool(uint8_t *out, size_t cap, const char *addr, bool v) {
  Frame f(out, cap, QOS_CONFIRM);
  f.w.u8(MSG_SET); f.w.u8(VAL_BOOL); f.w.str(addr); f.w.u8(v ? 1 : 0);
  return f.finish();
}

size_t encodeSetInt(uint8_t *out, size_t cap, const char *addr, int64_t v) {
  Frame f(out, cap, QOS_CONFIRM);
  f.w.u8(MSG_SET); f.w.u8(VAL_I64); f.w.str(addr); f.w.u64((uint64_t)v);
  return f.finish();
}

size_t encodeSetFloat(uint8_t *out, size_t cap, const char *addr, double v) {
  Frame f(out, cap, QOS_CONFIRM);
  f.w.u8(MSG_SET); f.w.u8(VAL_F64); f.w.str(addr); f.w.f64(v);
  return f.finish();
}

size_t encodePing(uint8_t *out, size_t cap) {
  Frame f(out, cap, QOS_FIRE);
  f.w.u8(MSG_PING);
  return f.finish();
}

size_t encodePong(uint8_t *out, size_t cap) {
  Frame f(out, cap, QOS_FIRE);
  f.w.u8(MSG_PONG);
  return f.finish();
}

bool decode(const uint8_t *frame, size_t len, Message &m) {
  m = Message();
  if (!frame || len < HEADER || frame[0] != MAGIC) return false;
  uint8_t flags = frame[1];
  if ((flags & 0x07) == 0) return false;      // legacy MessagePack, not spoken here
  if (flags & 0x18) return false;             // encrypted or compressed
  size_t off = HEADER;
  if (flags & 0x20) off += 8;                 // timestamp
  size_t payload = (size_t)frame[2] << 8 | frame[3];
  if (len < off + payload || payload < 1) return false;

  m.qos = flags >> 6;
  Reader r(frame + off, payload);
  m.type = r.u8();
  switch (m.type) {
    case MSG_PUBLISH: {
      uint8_t pf = r.u8();
      m.signal = (pf >> 5) & 0x07;
      m.addr = r.str(m.addrLen);
      uint8_t indicator = r.u8();
      if (indicator == 1) {
        m.hasValue = readValue(r, r.u8(), m.value, 0);
        if (!m.hasValue) return false;
      } else if (indicator == 2) {
        r.skip((size_t)r.u16() * 8);          // f64 samples
      }
      break;
    }
    case MSG_SET: {
      uint8_t sf = r.u8();
      m.signal = SIG_PARAM;
      m.addr = r.str(m.addrLen);
      m.hasValue = readValue(r, sf & 0x0F, m.value, 0);
      if (!m.hasValue) return false;
      break;
    }
    case MSG_SNAPSHOT:
      m.count = r.u16();
      break;
    case MSG_ERROR:
      m.errCode = r.u16();
      m.errMsg = r.str(m.errLen);
      break;
    default:
      break;                                  // WELCOME, ACK, PING, PONG and the rest
  }
  return r.ok;
}

bool addrIs(const char *addr, uint16_t n, const char *want) {
  size_t wl = strlen(want);
  return addr && wl == n && memcmp(addr, want, n) == 0;
}

}  // namespace clasp
