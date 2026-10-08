// A small RFC 6455 WebSocket client for one job: carrying CLASP frames.
//
// Binary messages only, client side masking, ping/pong and close handled here.
// Fixed buffers, no heap. Every blocking step has a deadline. A message larger
// than the receive buffer is read off the wire and dropped, and the stream stays
// in sync. Not thread safe: one task owns it.
#pragma once

#include <NetworkClient.h>
#include <stddef.h>
#include <stdint.h>

class WsClient {
 public:
  WsClient(uint8_t *rx, size_t rxCap, uint8_t *tx, size_t txCap) : rx_(rx), rxCap_(rxCap), tx_(tx), txCap_(txCap) {}

  // Opens the TCP or TLS connection on `net` and runs the HTTP upgrade. The
  // connect timeout also becomes the socket's send and receive timeout (and the
  // TLS socket timeout), which bounds every later write.
  bool open(NetworkClient &net, const char *host, uint16_t port, const char *path, const char *subprotocol,
            uint32_t connectMs, uint32_t upgradeMs);
  void close();
  bool isOpen();

  bool sendBinary(const uint8_t *data, size_t len);
  bool sendPing();

  // Reads what has arrived. Returns the length of one complete binary message,
  // now in the rx buffer and valid until the next poll(); 0 when nothing is
  // complete; -1 when the connection has closed.
  int poll();
  const uint8_t *message() const { return rx_; }

  uint32_t lastRxMs() const { return lastRx_; }
  uint32_t dropped() const { return dropped_; }
  const char *error() const { return err_; }

 private:
  enum State : uint8_t { H0, H1, LEN16, LEN64, PAYLOAD };

  bool sendFrame(uint8_t opcode, const uint8_t *data, size_t len);
  bool writeAll(const uint8_t *d, size_t n);
  bool readLine(char *line, size_t cap, uint32_t deadline);
  int feed(uint8_t b);
  int endFrame();

  NetworkClient *net_ = nullptr;
  uint8_t *rx_;
  size_t rxCap_;
  uint8_t *tx_;
  size_t txCap_;
  bool open_ = false;

  // receive state
  State st_ = H0;
  uint8_t op_ = 0;          // opcode of the current frame
  uint8_t msgOp_ = 0;       // opcode of the message being assembled (for continuations)
  bool fin_ = false;
  uint64_t need_ = 0;       // payload bytes left in this frame
  uint8_t lenBytes_ = 0;
  size_t msgLen_ = 0;       // bytes of the current message held in rx_
  bool overflow_ = false;
  uint8_t ctrl_[125];
  size_t ctrlLen_ = 0;

  uint32_t lastRx_ = 0;
  uint32_t dropped_ = 0;
  const char *err_ = "";
};
