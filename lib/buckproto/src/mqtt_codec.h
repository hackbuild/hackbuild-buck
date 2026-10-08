// MQTT 3.1.1, the subset BUCK needs: CONNECT with a will, SUBSCRIBE at QoS 0,
// PUBLISH at QoS 0, PINGREQ and DISCONNECT out; CONNACK, SUBACK, PUBLISH and
// PINGRESP in. No heap: encoders fill a caller buffer, the reader parses a byte
// stream into a fixed buffer.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mqtt {

enum PacketType : uint8_t {
  CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4, SUBSCRIBE = 8, SUBACK = 9,
  UNSUBSCRIBE = 10, UNSUBACK = 11, PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14,
};

struct Will {
  const char *topic = nullptr;   // nullptr for no will
  const char *message = nullptr;
  bool retain = false;
};

size_t encodeConnect(uint8_t *out, size_t cap, const char *clientId, uint16_t keepaliveSec, const Will &will);
size_t encodeSubscribe(uint8_t *out, size_t cap, uint16_t packetId, const char *topic);
size_t encodePublish(uint8_t *out, size_t cap, const char *topic, const uint8_t *payload, size_t len, bool retain);
size_t encodePingreq(uint8_t *out, size_t cap);
size_t encodeDisconnect(uint8_t *out, size_t cap);

struct Packet {
  uint8_t type = 0;
  uint8_t flags = 0;               // low nibble of the fixed header
  const uint8_t *body = nullptr;   // variable header + payload
  size_t len = 0;
  bool truncated = false;          // the packet was longer than the buffer; body holds its start
};

// Incremental parser. Feed it bytes as they arrive; push() returns true each time
// a whole packet is ready in `packet()`. A packet longer than the buffer is still
// read to its end, so the stream stays in sync, and is delivered with its first
// `cap` bytes and `truncated` set (counted in `truncatedCount`).
class Reader {
 public:
  Reader(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}
  bool push(uint8_t byte);
  const Packet &packet() const { return pkt_; }
  void reset();
  uint32_t truncatedCount = 0;
  bool malformed = false;          // remaining-length encoding was invalid

 private:
  enum State : uint8_t { HEADER, LENGTH, BODY };
  uint8_t *buf_;
  size_t cap_;
  State state_ = HEADER;
  uint8_t header_ = 0;
  uint32_t remaining_ = 0;
  uint32_t multiplier_ = 1;
  uint8_t lengthBytes_ = 0;
  size_t have_ = 0;                // bytes stored in buf_
  uint32_t read_ = 0;              // body bytes consumed, stored or not
  Packet pkt_;
  bool finish();
};

struct Publish {
  const char *topic = nullptr;
  uint16_t topicLen = 0;
  const uint8_t *payload = nullptr;
  size_t payloadLen = 0;
  uint8_t qos = 0;
  bool retain = false;
  uint16_t packetId = 0;
};

bool parsePublish(const Packet &p, Publish &out);
// CONNACK return code, or -1 if the packet is malformed.
int parseConnack(const Packet &p);
// SUBACK: packet id and granted QoS (0x80 = refused). False if malformed.
bool parseSuback(const Packet &p, uint16_t &packetId, uint8_t &granted);

}  // namespace mqtt
