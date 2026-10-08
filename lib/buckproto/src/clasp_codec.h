// CLASP v3 binary codec, the subset BUCK speaks.
//
// Matches crates/clasp-core/src/codec.rs in lumencanvas/clasp. Every frame is
//   'S' (0x53) | flags | payload length (u16 BE) | [timestamp u64 if flags bit 5] | payload
// flags: [7:6] QoS, [5] timestamp, [4] encrypted, [3] compressed, [2:0] encoding (1 = binary).
// Over WebSocket each binary message carries exactly one frame.
//
// No heap. Encoders write into a caller buffer and return the frame length, or 0 when
// the buffer is too small. The decoder points into the frame it was given.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace clasp {

enum MsgType : uint8_t {
  MSG_HELLO = 0x01,
  MSG_WELCOME = 0x02,
  MSG_SUBSCRIBE = 0x10,
  MSG_UNSUBSCRIBE = 0x11,
  MSG_PUBLISH = 0x20,
  MSG_SET = 0x21,
  MSG_GET = 0x22,
  MSG_SNAPSHOT = 0x23,
  MSG_BUNDLE = 0x30,
  MSG_SYNC = 0x40,
  MSG_PING = 0x41,
  MSG_PONG = 0x42,
  MSG_ACK = 0x50,
  MSG_ERROR = 0x51,
};

enum ValType : uint8_t {
  VAL_NULL = 0x00,
  VAL_BOOL = 0x01,
  VAL_I8 = 0x02,
  VAL_I16 = 0x03,
  VAL_I32 = 0x04,
  VAL_I64 = 0x05,
  VAL_F32 = 0x06,
  VAL_F64 = 0x07,
  VAL_STRING = 0x08,
  VAL_BYTES = 0x09,
  VAL_ARRAY = 0x0A,
  VAL_MAP = 0x0B,
};

enum Signal : uint8_t { SIG_PARAM = 0, SIG_EVENT = 1, SIG_STREAM = 2, SIG_GESTURE = 3, SIG_TIMELINE = 4 };
enum Qos : uint8_t { QOS_FIRE = 0, QOS_CONFIRM = 1, QOS_COMMIT = 2 };

static const uint8_t MAGIC = 0x53;
static const size_t HEADER = 4;

// A decoded value. Strings and bytes point into the frame. Arrays and maps are
// validated and skipped; only their type is reported.
struct Value {
  uint8_t type = VAL_NULL;
  bool b = false;
  int64_t i = 0;
  double f = 0;
  const char *str = nullptr;
  uint16_t len = 0;
};

struct Message {
  uint8_t type = 0;
  uint8_t qos = 0;
  // PUBLISH / SET
  const char *addr = nullptr;
  uint16_t addrLen = 0;
  uint8_t signal = SIG_PARAM;   // PUBLISH signal type; SET reports SIG_PARAM
  bool hasValue = false;
  Value value;
  // ERROR
  uint16_t errCode = 0;
  const char *errMsg = nullptr;
  uint16_t errLen = 0;
  // SNAPSHOT
  uint16_t count = 0;
};

// Encoders. `name`, `token`, addresses and text are NUL-terminated.
size_t encodeHello(uint8_t *out, size_t cap, const char *name, const char *token);
size_t encodeSubscribe(uint8_t *out, size_t cap, uint32_t id, const char *pattern);
size_t encodePublishString(uint8_t *out, size_t cap, Signal sig, const char *addr, const char *text);
size_t encodeStreamFloat(uint8_t *out, size_t cap, const char *addr, double v);
size_t encodeSetString(uint8_t *out, size_t cap, const char *addr, const char *text);
size_t encodeSetBool(uint8_t *out, size_t cap, const char *addr, bool v);
size_t encodeSetInt(uint8_t *out, size_t cap, const char *addr, int64_t v);
size_t encodeSetFloat(uint8_t *out, size_t cap, const char *addr, double v);
size_t encodePing(uint8_t *out, size_t cap);
size_t encodePong(uint8_t *out, size_t cap);

// Decode one frame. Returns false for anything malformed or truncated. Unknown but
// well-formed message types decode with only `type` and `qos` set.
bool decode(const uint8_t *frame, size_t len, Message &m);

// True when `addr` (length n) equals the NUL-terminated `want`.
bool addrIs(const char *addr, uint16_t n, const char *want);

}  // namespace clasp
