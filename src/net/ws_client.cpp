#include "ws_client.h"

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

namespace {

const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
enum : uint8_t { OP_CONT = 0x0, OP_TEXT = 0x1, OP_BINARY = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA };

bool startsWithNoCase(const char *s, const char *prefix) { return strncasecmp(s, prefix, strlen(prefix)) == 0; }

}  // namespace

bool WsClient::writeAll(const uint8_t *d, size_t n) {
  uint32_t deadline = millis() + 5000;
  while (n) {
    if (!net_ || !net_->connected()) { err_ = "write on closed socket"; return false; }
    size_t w = net_->write(d, n);
    if (w == 0) {
      if ((int32_t)(millis() - deadline) > 0) { err_ = "write timed out"; return false; }
      vTaskDelay(1);
      continue;
    }
    d += w;
    n -= w;
  }
  return true;
}

bool WsClient::readLine(char *line, size_t cap, uint32_t deadline) {
  size_t n = 0;
  for (;;) {
    if ((int32_t)(millis() - deadline) > 0) return false;
    if (!net_->connected() && !net_->available()) return false;
    int c = net_->read();
    if (c < 0) { vTaskDelay(2); continue; }
    if (c == '\r') continue;
    if (c == '\n') { line[n] = 0; return true; }
    if (n + 1 < cap) line[n++] = (char)c;   // long header lines are truncated, not fatal
  }
}

bool WsClient::open(NetworkClient &net, const char *host, uint16_t port, const char *path, const char *subprotocol,
                    uint32_t timeoutMs) {
  close();
  net_ = &net;
  err_ = "";
  uint32_t deadline = millis() + timeoutMs;
  if (!net.connect(host, port, (int32_t)timeoutMs)) { err_ = "connect failed"; return false; }
  net.setTimeout(2000);

  uint8_t keyRaw[16];
  esp_fill_random(keyRaw, sizeof(keyRaw));
  unsigned char key[32];
  size_t keyLen = 0;
  mbedtls_base64_encode(key, sizeof(key), &keyLen, keyRaw, sizeof(keyRaw));
  key[keyLen] = 0;

  char req[384];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: %s\r\n"
                   "User-Agent: hackbuild-buck\r\n\r\n",
                   path, host, (const char *)key, subprotocol);
  if (n <= 0 || n >= (int)sizeof(req) || !writeAll((const uint8_t *)req, n)) {
    err_ = "upgrade request failed";
    net.stop();
    return false;
  }

  // Expected accept value: base64(sha1(key + GUID)).
  char concat[80];
  snprintf(concat, sizeof(concat), "%s%s", (const char *)key, WS_GUID);
  unsigned char sha[20];
  mbedtls_sha1((const unsigned char *)concat, strlen(concat), sha);
  unsigned char want[40];
  size_t wantLen = 0;
  mbedtls_base64_encode(want, sizeof(want), &wantLen, sha, sizeof(sha));
  want[wantLen] = 0;

  char line[160];
  if (!readLine(line, sizeof(line), deadline)) { err_ = "no upgrade response"; net.stop(); return false; }
  if (strncmp(line, "HTTP/1.1 101", 12) != 0) { err_ = "upgrade refused"; net.stop(); return false; }
  bool accepted = false;
  for (;;) {
    if (!readLine(line, sizeof(line), deadline)) { err_ = "upgrade headers cut off"; net.stop(); return false; }
    if (!line[0]) break;
    if (startsWithNoCase(line, "Sec-WebSocket-Accept:")) {
      const char *v = line + 21;
      while (*v == ' ') v++;
      accepted = strcmp(v, (const char *)want) == 0;
    }
  }
  if (!accepted) { err_ = "bad Sec-WebSocket-Accept"; net.stop(); return false; }

  st_ = H0;
  msgLen_ = 0;
  overflow_ = false;
  open_ = true;
  lastRx_ = millis();
  return true;
}

void WsClient::close() {
  if (open_) {
    uint8_t code[2] = {0x03, 0xE8};   // 1000, normal closure
    sendFrame(OP_CLOSE, code, 2);
  }
  open_ = false;
  if (net_) net_->stop();
}

bool WsClient::isOpen() { return open_ && net_ && net_->connected(); }

bool WsClient::sendFrame(uint8_t opcode, const uint8_t *data, size_t len) {
  if (!net_) return false;
  size_t head = 2 + (len < 126 ? 0 : len <= 0xFFFF ? 2 : 8) + 4;
  if (head + len > txCap_) { err_ = "frame too large to send"; return false; }
  uint8_t *p = tx_;
  *p++ = 0x80 | opcode;                       // FIN
  if (len < 126) {
    *p++ = 0x80 | (uint8_t)len;               // masked
  } else if (len <= 0xFFFF) {
    *p++ = 0x80 | 126;
    *p++ = (uint8_t)(len >> 8);
    *p++ = (uint8_t)len;
  } else {
    *p++ = 0x80 | 127;
    for (int s = 56; s >= 0; s -= 8) *p++ = (uint8_t)((uint64_t)len >> s);
  }
  uint8_t mask[4];
  esp_fill_random(mask, 4);
  memcpy(p, mask, 4);
  p += 4;
  for (size_t i = 0; i < len; i++) *p++ = data[i] ^ mask[i & 3];
  return writeAll(tx_, head + len);
}

bool WsClient::sendBinary(const uint8_t *data, size_t len) {
  if (!open_) return false;
  if (!sendFrame(OP_BINARY, data, len)) { open_ = false; return false; }
  return true;
}

bool WsClient::sendPing() {
  if (!open_) return false;
  if (!sendFrame(OP_PING, nullptr, 0)) { open_ = false; return false; }
  return true;
}

// End of a frame's payload: handle control frames, or finish a data message.
int WsClient::endFrame() {
  st_ = H0;
  if (op_ >= OP_CLOSE) {
    if (op_ == OP_PING) sendFrame(OP_PONG, ctrl_, ctrlLen_);
    if (op_ == OP_CLOSE) {
      sendFrame(OP_CLOSE, ctrl_, ctrlLen_ >= 2 ? 2 : 0);
      open_ = false;
      err_ = "closed by server";
      return -1;
    }
    return 0;
  }
  if (!fin_) return 0;                        // more continuation frames to come
  size_t len = msgLen_;
  bool keep = !overflow_ && msgOp_ == OP_BINARY;
  if (overflow_) dropped_++;
  msgLen_ = 0;
  overflow_ = false;
  return keep ? (int)len : 0;
}

int WsClient::feed(uint8_t b) {
  switch (st_) {
    case H0:
      fin_ = b & 0x80;
      op_ = b & 0x0F;
      if (op_ != OP_CONT && op_ < OP_CLOSE) { msgOp_ = op_; msgLen_ = 0; overflow_ = false; }
      ctrlLen_ = 0;
      st_ = H1;
      return 0;
    case H1: {
      uint8_t l = b & 0x7F;
      if (b & 0x80) { err_ = "server frame was masked"; open_ = false; return -1; }
      if (l == 126) { need_ = 0; lenBytes_ = 2; st_ = LEN16; return 0; }
      if (l == 127) { need_ = 0; lenBytes_ = 8; st_ = LEN64; return 0; }
      need_ = l;
      break;
    }
    case LEN16:
    case LEN64:
      need_ = (need_ << 8) | b;
      if (--lenBytes_) return 0;
      break;
    case PAYLOAD:
      if (op_ >= OP_CLOSE) {
        if (ctrlLen_ < sizeof(ctrl_)) ctrl_[ctrlLen_++] = b;
      } else if (msgLen_ < rxCap_) {
        rx_[msgLen_++] = b;
      } else {
        overflow_ = true;
      }
      if (--need_) return 0;
      return endFrame();
  }
  // Header complete.
  if (op_ >= OP_CLOSE && need_ > 125) { err_ = "oversize control frame"; open_ = false; return -1; }
  if (need_ == 0) return endFrame();
  st_ = PAYLOAD;
  return 0;
}

int WsClient::poll() {
  if (!open_) return -1;
  if (!net_->connected() && !net_->available()) { open_ = false; err_ = "connection lost"; return -1; }
  // Bounded work per call so one busy connection cannot hog the task.
  for (int budget = 2048; budget > 0 && net_->available() > 0; budget--) {
    int c = net_->read();
    if (c < 0) break;
    lastRx_ = millis();
    int r = feed((uint8_t)c);
    if (r != 0) return r;
  }
  return 0;
}
