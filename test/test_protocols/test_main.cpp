// Host-side tests for the protocol code. The CLASP vectors were captured from
// relay.clasp.to on 2026-10-07; the MQTT ones from relay.clasp.chat:1883.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "clasp_codec.h"
#include "mqtt_codec.h"
#include "text_filter.h"

static size_t hex(const char *s, uint8_t *out) {
  size_t n = 0;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    unsigned v;
    sscanf(s, "%2x", &v);
    out[n++] = (uint8_t)v;
    s += 2;
  }
  return n;
}

static void appendStr(uint8_t *buf, size_t &n, const char *s) {
  size_t l = strlen(s);
  buf[n++] = (uint8_t)(l >> 8);
  buf[n++] = (uint8_t)l;
  memcpy(buf + n, s, l);
  n += l;
}

// ------------------------------------------------------------------ CLASP

void test_clasp_hello_matches_relay_handshake() {
  uint8_t buf[64];
  size_t n = clasp::encodeHello(buf, sizeof(buf), "proto", "");
  uint8_t want[64];
  size_t wn = hex("53 41 00 0c 01 01 e0 00 05 70 72 6f 74 6f 00 00", want);
  TEST_ASSERT_EQUAL(wn, n);
  TEST_ASSERT_EQUAL_MEMORY(want, buf, n);
}

void test_clasp_subscribe_layout() {
  uint8_t buf[64];
  size_t n = clasp::encodeSubscribe(buf, sizeof(buf), 7, "/a/**");
  uint8_t want[64];
  size_t wn = hex("53 41 00 0e 10 00 00 00 07 00 05 2f 61 2f 2a 2a ff 00", want);
  TEST_ASSERT_EQUAL(wn, n);
  TEST_ASSERT_EQUAL_MEMORY(want, buf, n);
}

void test_clasp_set_float_matches_spec_example() {
  // The protocol doc's SET /lumen/opacity 0.75 example, with QoS Confirm. The doc
  // header says 0x1B, but its own payload bytes add up to 26 (0x1A).
  uint8_t buf[64];
  size_t n = clasp::encodeSetFloat(buf, sizeof(buf), "/lumen/opacity", 0.75);
  uint8_t want[64];
  size_t wn = hex("53 41 00 1a 21 07 00 0e 2f 6c 75 6d 65 6e 2f 6f 70 61 63 69 74 79 3f e8 00 00 00 00 00 00", want);
  TEST_ASSERT_EQUAL(wn, n);
  TEST_ASSERT_EQUAL_MEMORY(want, buf, n);
}

void test_clasp_decode_relay_publish_event() {
  uint8_t f[128];
  size_t n = hex("53 41 00 26 20 20 00 14 2f 68 61 63 6b 62 75 69 6c 64 2f 70 72 6f 74 6f 2f 73 61 79 "
                 "01 08 00 0a 68 65 6c 6c 6f 20 62 75 63 6b", f);
  clasp::Message m;
  TEST_ASSERT_TRUE(clasp::decode(f, n, m));
  TEST_ASSERT_EQUAL(clasp::MSG_PUBLISH, m.type);
  TEST_ASSERT_EQUAL(clasp::SIG_EVENT, m.signal);
  TEST_ASSERT_TRUE(clasp::addrIs(m.addr, m.addrLen, "/hackbuild/proto/say"));
  TEST_ASSERT_TRUE(m.hasValue);
  TEST_ASSERT_EQUAL(clasp::VAL_STRING, m.value.type);
  TEST_ASSERT_EQUAL(10, m.value.len);
  TEST_ASSERT_EQUAL_MEMORY("hello buck", m.value.str, 10);
}

void test_clasp_decode_relay_set_with_revision() {
  uint8_t f[128];
  size_t n = hex("53 41 00 31 21 88 00 19 2f 68 61 63 6b 62 75 69 6c 64 2f 70 72 6f 74 6f 2f 73 61 79 2f 6c 61 73 74 "
                 "00 0a 68 65 6c 6c 6f 20 62 75 63 6b 00 00 00 00 00 00 00 01", f);
  clasp::Message m;
  TEST_ASSERT_TRUE(clasp::decode(f, n, m));
  TEST_ASSERT_EQUAL(clasp::MSG_SET, m.type);
  TEST_ASSERT_EQUAL(clasp::VAL_STRING, m.value.type);
  TEST_ASSERT_EQUAL_MEMORY("hello buck", m.value.str, 10);
}

void test_clasp_decode_relay_snapshot() {
  uint8_t f[128];
  size_t n = hex("53 01 00 62 23 00 01 00 19 2f 68 61 63 6b 62 75 69 6c 64 2f 70 72 6f 74 6f 2f 73 61 79 2f 6c 61 73 74 "
                 "08 00 0a 68 65 6c 6c 6f 20 62 75 63 6b 00 00 00 00 00 00 00 01 03 00 24 "
                 "36 32 34 61 30 30 34 33 2d 62 36 66 64 2d 34 31 37 38 2d 62 63 61 31 2d 61 32 62 35 35 32 65 66 66 38 63 36 "
                 "00 06 5d 4a 10 99 15 3c", f);
  clasp::Message m;
  TEST_ASSERT_TRUE(clasp::decode(f, n, m));
  TEST_ASSERT_EQUAL(clasp::MSG_SNAPSHOT, m.type);
  TEST_ASSERT_EQUAL(1, m.count);
}

void test_clasp_decode_relay_error() {
  uint8_t f[64];
  size_t n = hex("53 01 00 1d 51 01 2c 00 17 41 75 74 68 65 6e 74 69 63 61 74 69 6f 6e 20 72 65 71 75 69 72 65 64 00", f);
  clasp::Message m;
  TEST_ASSERT_TRUE(clasp::decode(f, n, m));
  TEST_ASSERT_EQUAL(clasp::MSG_ERROR, m.type);
  TEST_ASSERT_EQUAL(300, m.errCode);
  TEST_ASSERT_EQUAL(23, m.errLen);
}

void test_clasp_roundtrip_all_encoders() {
  uint8_t buf[256];
  clasp::Message m;
  size_t n = clasp::encodePublishString(buf, sizeof(buf), clasp::SIG_EVENT, "/x/say", "hi");
  TEST_ASSERT_TRUE(clasp::decode(buf, n, m));
  TEST_ASSERT_EQUAL_MEMORY("hi", m.value.str, 2);
  n = clasp::encodeStreamFloat(buf, sizeof(buf), "/x/jaw", 0.5);
  TEST_ASSERT_TRUE(clasp::decode(buf, n, m));
  TEST_ASSERT_EQUAL(clasp::SIG_STREAM, m.signal);
  TEST_ASSERT_EQUAL(0, m.qos);
  TEST_ASSERT_EQUAL_DOUBLE(0.5, m.value.f);
  n = clasp::encodeSetBool(buf, sizeof(buf), "/x/on", true);
  TEST_ASSERT_TRUE(clasp::decode(buf, n, m));
  TEST_ASSERT_TRUE(m.value.b);
  n = clasp::encodeSetInt(buf, sizeof(buf), "/x/n", -42);
  TEST_ASSERT_TRUE(clasp::decode(buf, n, m));
  TEST_ASSERT_EQUAL_INT64(-42, m.value.i);
  n = clasp::encodeSetString(buf, sizeof(buf), "/x/s", "deer");
  TEST_ASSERT_TRUE(clasp::decode(buf, n, m));
  TEST_ASSERT_EQUAL_MEMORY("deer", m.value.str, 4);
  n = clasp::encodePing(buf, sizeof(buf));
  TEST_ASSERT_TRUE(clasp::decode(buf, n, m));
  TEST_ASSERT_EQUAL(clasp::MSG_PING, m.type);
}

void test_clasp_encoder_refuses_small_buffer() {
  uint8_t buf[12];
  TEST_ASSERT_EQUAL(0, clasp::encodeSetString(buf, sizeof(buf), "/a/long/address", "text"));
  TEST_ASSERT_EQUAL(0, clasp::encodeHello(buf, 3, "x", ""));
}

void test_clasp_decode_skips_nested_map_and_rejects_deep_nesting() {
  uint8_t f[512];
  size_t n = 0;
  f[n++] = 0x53; f[n++] = 0x41; f[n++] = 0; f[n++] = 0;
  f[n++] = 0x21; f[n++] = clasp::VAL_MAP;
  appendStr(f, n, "/m");
  f[n++] = 0; f[n++] = 1;                       // one entry
  appendStr(f, n, "k");
  f[n++] = clasp::VAL_ARRAY; f[n++] = 0; f[n++] = 2;
  f[n++] = clasp::VAL_I32; f[n++] = 0; f[n++] = 0; f[n++] = 0; f[n++] = 9;
  f[n++] = clasp::VAL_NULL;
  f[2] = (uint8_t)((n - 4) >> 8); f[3] = (uint8_t)(n - 4);
  clasp::Message m;
  TEST_ASSERT_TRUE(clasp::decode(f, n, m));
  TEST_ASSERT_EQUAL(clasp::VAL_MAP, m.value.type);

  // 40 nested arrays exceeds the depth limit and must fail cleanly.
  n = 0;
  f[n++] = 0x53; f[n++] = 0x41; f[n++] = 0; f[n++] = 0;
  f[n++] = 0x21; f[n++] = clasp::VAL_ARRAY;
  appendStr(f, n, "/d");
  for (int d = 0; d < 40; d++) { f[n++] = 0; f[n++] = 1; f[n++] = clasp::VAL_ARRAY; }
  f[n++] = 0; f[n++] = 0;
  f[2] = (uint8_t)((n - 4) >> 8); f[3] = (uint8_t)(n - 4);
  TEST_ASSERT_FALSE(clasp::decode(f, n, m));
}

void test_clasp_decode_rejects_bad_frames() {
  uint8_t f[64];
  clasp::Message m;
  size_t n = hex("53 41 00 26 20 20 00 14 2f 68", f);            // truncated payload
  TEST_ASSERT_FALSE(clasp::decode(f, n, m));
  n = hex("52 41 00 01 41", f);                                  // wrong magic
  TEST_ASSERT_FALSE(clasp::decode(f, n, m));
  n = hex("53 40 00 01 41", f);                                  // MessagePack encoding
  TEST_ASSERT_FALSE(clasp::decode(f, n, m));
  n = hex("53 41 00 08 20 20 00 40 2f 2f 2f 2f", f);             // address longer than frame
  TEST_ASSERT_FALSE(clasp::decode(f, n, m));
  TEST_ASSERT_FALSE(clasp::decode(nullptr, 0, m));
}

void test_clasp_decode_survives_random_bytes() {
  uint8_t f[300];
  clasp::Message m;
  srand(1234);
  for (int round = 0; round < 200000; round++) {
    size_t n = (size_t)(rand() % sizeof(f));
    for (size_t i = 0; i < n; i++) f[i] = (uint8_t)rand();
    if (n >= 4) {
      f[0] = 0x53;
      f[1] = (uint8_t)((rand() & 0xC0) | 0x01);
      size_t pl = n - 4;
      f[2] = (uint8_t)(pl >> 8); f[3] = (uint8_t)pl;
    }
    clasp::decode(f, n, m);
    if (m.addr) TEST_ASSERT_TRUE(m.addr >= (const char *)f && m.addr + m.addrLen <= (const char *)f + n);
    if (m.value.str) TEST_ASSERT_TRUE(m.value.str + m.value.len <= (const char *)f + n);
  }
}

// ------------------------------------------------------------------ MQTT

void test_mqtt_connect_with_will() {
  uint8_t buf[128];
  mqtt::Will w;
  w.topic = "b/online"; w.message = "false"; w.retain = true;
  size_t n = mqtt::encodeConnect(buf, sizeof(buf), "buck-1", 30, w);
  uint8_t want[128];
  size_t wn = hex("10 23 00 04 4d 51 54 54 04 26 00 1e 00 06 62 75 63 6b 2d 31 "
                  "00 08 62 2f 6f 6e 6c 69 6e 65 00 05 66 61 6c 73 65", want);
  TEST_ASSERT_EQUAL(wn, n);
  TEST_ASSERT_EQUAL_MEMORY(want, buf, n);
}

void test_mqtt_subscribe_and_publish_bytes() {
  uint8_t buf[64];
  size_t n = mqtt::encodeSubscribe(buf, sizeof(buf), 1, "a/b");
  uint8_t want[64];
  size_t wn = hex("82 08 00 01 00 03 61 2f 62 00", want);
  TEST_ASSERT_EQUAL(wn, n);
  TEST_ASSERT_EQUAL_MEMORY(want, buf, n);
  n = mqtt::encodePublish(buf, sizeof(buf), "a/b", (const uint8_t *)"hi", 2, true);
  wn = hex("31 07 00 03 61 2f 62 68 69", want);
  TEST_ASSERT_EQUAL(wn, n);
  TEST_ASSERT_EQUAL_MEMORY(want, buf, n);
}

void test_mqtt_reader_parses_split_stream() {
  // CONNACK, SUBACK, and a PUBLISH, delivered one byte at a time.
  uint8_t stream[64];
  size_t n = hex("20 02 00 00 90 03 00 01 00 30 0c 00 05 61 2f 73 61 79 68 65 6c 6c 6f", stream);
  uint8_t buf[32];
  mqtt::Reader r(buf, sizeof(buf));
  int seen = 0;
  for (size_t i = 0; i < n; i++) {
    if (!r.push(stream[i])) continue;
    const mqtt::Packet &p = r.packet();
    if (seen == 0) TEST_ASSERT_EQUAL(0, mqtt::parseConnack(p));
    if (seen == 1) {
      uint16_t id; uint8_t g;
      TEST_ASSERT_TRUE(mqtt::parseSuback(p, id, g));
      TEST_ASSERT_EQUAL(1, id);
      TEST_ASSERT_EQUAL(0, g);
    }
    if (seen == 2) {
      mqtt::Publish pub;
      TEST_ASSERT_TRUE(mqtt::parsePublish(p, pub));
      TEST_ASSERT_EQUAL(5, pub.topicLen);
      TEST_ASSERT_EQUAL_MEMORY("a/say", pub.topic, 5);
      TEST_ASSERT_EQUAL(5, pub.payloadLen);
      TEST_ASSERT_EQUAL_MEMORY("hello", pub.payload, 5);
      TEST_ASSERT_FALSE(pub.retain);
    }
    seen++;
  }
  TEST_ASSERT_EQUAL(3, seen);
}

void test_mqtt_reader_truncates_oversize_and_stays_in_sync() {
  // A 300 byte PUBLISH (two byte length) into a 16 byte buffer: delivered cut
  // short and flagged, then the next packet still parses.
  uint8_t buf[16];
  mqtt::Reader r(buf, sizeof(buf));
  uint8_t big[3 + 300];
  big[0] = 0x30; big[1] = 0xAC; big[2] = 0x02;    // remaining length 300
  big[3] = 0; big[4] = 3; big[5] = 'a'; big[6] = '/'; big[7] = 'b';
  memset(big + 8, 'x', sizeof(big) - 8);
  for (size_t i = 0; i < sizeof(big) - 1; i++) TEST_ASSERT_FALSE(r.push(big[i]));
  TEST_ASSERT_TRUE(r.push(big[sizeof(big) - 1]));
  const mqtt::Packet &p = r.packet();
  TEST_ASSERT_TRUE(p.truncated);
  TEST_ASSERT_EQUAL(16, p.len);
  mqtt::Publish pub;
  TEST_ASSERT_TRUE(mqtt::parsePublish(p, pub));
  TEST_ASSERT_EQUAL_MEMORY("a/b", pub.topic, 3);
  TEST_ASSERT_EQUAL(11, pub.payloadLen);
  TEST_ASSERT_EQUAL(1, r.truncatedCount);
  TEST_ASSERT_FALSE(r.push(0xD0));
  TEST_ASSERT_TRUE(r.push(0x00));
  TEST_ASSERT_EQUAL(mqtt::PINGRESP, r.packet().type);
  TEST_ASSERT_FALSE(r.packet().truncated);
}

void test_mqtt_reader_flags_bad_length() {
  uint8_t buf[8];
  mqtt::Reader r(buf, sizeof(buf));
  r.push(0x30);
  for (int i = 0; i < 5; i++) r.push(0xFF);
  TEST_ASSERT_TRUE(r.malformed);
}

void test_mqtt_publish_with_qos1_and_retain() {
  uint8_t body[32];
  size_t n = hex("00 01 74 00 07 79 6f", body);
  mqtt::Packet p;
  p.type = mqtt::PUBLISH; p.flags = 0x03; p.body = body; p.len = n;
  mqtt::Publish pub;
  TEST_ASSERT_TRUE(mqtt::parsePublish(p, pub));
  TEST_ASSERT_TRUE(pub.retain);
  TEST_ASSERT_EQUAL(1, pub.qos);
  TEST_ASSERT_EQUAL(7, pub.packetId);
  TEST_ASSERT_EQUAL(2, pub.payloadLen);
  p.len = 2;                                       // topic length beyond the body
  TEST_ASSERT_FALSE(mqtt::parsePublish(p, pub));
}

// ------------------------------------------------------------------ text

void test_clean_text() {
  char out[64];
  const char *in = "  hello\tthere\r\n  deer  ";
  TEST_ASSERT_EQUAL_STRING("hello there deer", (buck::cleanText((const uint8_t *)in, strlen(in), out, sizeof(out)), out));
  const char *quotes = "it\xE2\x80\x99s \xE2\x80\x9C" "buck\xE2\x80\x9D\xE2\x80\xA6";
  buck::cleanText((const uint8_t *)quotes, strlen(quotes), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("it's \"buck\"...", out);
  const char *emoji = "hi \xF0\x9F\xA6\x8C there";
  buck::cleanText((const uint8_t *)emoji, strlen(emoji), out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("hi there", out);
  size_t n = buck::cleanText((const uint8_t *)"abcdefgh", 8, out, 5);
  TEST_ASSERT_EQUAL(4, n);
  TEST_ASSERT_EQUAL_STRING("abcd", out);
  TEST_ASSERT_EQUAL(0, buck::cleanText((const uint8_t *)"\x01\x02 ", 3, out, sizeof(out)));
}

void test_rate_limiter() {
  buck::RateLimiter r(3, 1000);
  TEST_ASSERT_TRUE(r.take(0));
  TEST_ASSERT_TRUE(r.take(10));
  TEST_ASSERT_TRUE(r.take(20));
  TEST_ASSERT_FALSE(r.take(30));
  TEST_ASSERT_TRUE(r.take(1000));
  TEST_ASSERT_FALSE(r.take(1500));
  TEST_ASSERT_TRUE(r.take(60000));
  TEST_ASSERT_TRUE(r.take(60000));
  TEST_ASSERT_TRUE(r.take(60000));
  TEST_ASSERT_FALSE(r.take(60000));
  buck::RateLimiter wrap(1, 1000);                 // millis() wrapping past 2^32
  TEST_ASSERT_TRUE(wrap.take(0xFFFFFF00u));
  TEST_ASSERT_TRUE(wrap.take(0x00000400u));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clasp_hello_matches_relay_handshake);
  RUN_TEST(test_clasp_subscribe_layout);
  RUN_TEST(test_clasp_set_float_matches_spec_example);
  RUN_TEST(test_clasp_decode_relay_publish_event);
  RUN_TEST(test_clasp_decode_relay_set_with_revision);
  RUN_TEST(test_clasp_decode_relay_snapshot);
  RUN_TEST(test_clasp_decode_relay_error);
  RUN_TEST(test_clasp_roundtrip_all_encoders);
  RUN_TEST(test_clasp_encoder_refuses_small_buffer);
  RUN_TEST(test_clasp_decode_skips_nested_map_and_rejects_deep_nesting);
  RUN_TEST(test_clasp_decode_rejects_bad_frames);
  RUN_TEST(test_clasp_decode_survives_random_bytes);
  RUN_TEST(test_mqtt_connect_with_will);
  RUN_TEST(test_mqtt_subscribe_and_publish_bytes);
  RUN_TEST(test_mqtt_reader_parses_split_stream);
  RUN_TEST(test_mqtt_reader_truncates_oversize_and_stays_in_sync);
  RUN_TEST(test_mqtt_reader_flags_bad_length);
  RUN_TEST(test_mqtt_publish_with_qos1_and_retain);
  RUN_TEST(test_clean_text);
  RUN_TEST(test_rate_limiter);
  return UNITY_END();
}
