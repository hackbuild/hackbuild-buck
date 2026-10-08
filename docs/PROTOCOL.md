# Talking to BUCK

Every BUCK listens on two public relays at once and reports what it is doing on both. This page is the full contract: addresses, payloads, limits, and what comes back.

## Two doors, one deer

[CLASP](https://clasp.to) is a signal routing protocol: clients connect to a relay, publish to addresses that look like `/a/b/c`, and subscribe to patterns. BUCK keeps two connections open, one to each of two CLASP relays:

| door | relay | who it is for |
|---|---|---|
| CLASP over WebSocket | `wss://relay.clasp.to` | browsers, the CLASP SDKs, anything that speaks CLASP |
| MQTT 3.1.1 over TCP | `relay.clasp.chat:1883` | curl, mosquitto, any MQTT client |

MQTT stands for Message Queuing Telemetry Transport, a small publish and subscribe protocol that curl happens to speak. The MQTT door is the MQTT bridge built into the CLASP relay, so an MQTT topic `hackbuild/buck/heatsync/say` is the CLASP address `/mqtt/hackbuild/buck/heatsync/say` on that relay.

The two relays are separate routers. A message sent on one is not visible on the other, so pick one door and read status from the same one. Neither needs an account.

## Addresses

Each BUCK has an id. The one at HeatSync Labs is `heatsync`. Yours is whatever you set with `/id`, or `buck-` plus the last six hex digits of its MAC address.

| what | CLASP address (relay.clasp.to) | MQTT topic (relay.clasp.chat) |
|---|---|---|
| say something | `/hackbuild/buck/<id>/say` | `hackbuild/buck/<id>/say` |
| change voice | `/hackbuild/buck/<id>/voice` | `hackbuild/buck/<id>/voice` |
| status | `/hackbuild/buck/<id>/status/...` | `hackbuild/buck/<id>/status/...` |

## Sending

### say

The payload is the text. In CLASP, send it as a string, either as an event (`emit`) or a param (`set`); BUCK treats both the same, and a number is read out as a number. In MQTT, the payload bytes are the text.

```
curl -d "hello deer" mqtt://relay.clasp.chat/hackbuild/buck/heatsync/say
mosquitto_pub -h relay.clasp.chat -t hackbuild/buck/heatsync/say -m "hello deer"
python3 tools/buck.py say "hello deer"
python3 tools/buck.py say "hello deer" --via clasp
```

```js
// @clasp-to/core 4.3.2, Node or browser
const buck = new Clasp('wss://relay.clasp.to', { name: 'my app' });
await buck.connect();
buck.emit('/hackbuild/buck/heatsync/say', 'hello deer');
```

### voice

The payload is one of `sam`, `elf`, `robot`, `stuffy`, `oldlady`, `et`. Anything else is ignored. A voice set over the network lasts until BUCK reboots; a voice set over serial is saved.

```
curl -d "robot" mqtt://relay.clasp.chat/hackbuild/buck/heatsync/voice
```

### What BUCK does with your text

1. Cleans it: curly quotes become straight ones, emoji and other non-ASCII become spaces, whitespace collapses.
2. Cuts it to 200 characters. Over MQTT any length is accepted and cut; over CLASP a frame larger than 2 KB is ignored.
3. Checks the rate limit: a bucket of four messages, refilled at one every three seconds, shared by everyone on both doors.
4. Checks the queue: at most four network messages wait at once.
5. Speaks it, after whatever is already queued.

A message that fails step 3 or 4 is dropped. There is no reply; watch `status/said` to see what made it.

SAM reads English spelling with its own rules from 1982. Short sentences with ordinary words sound best. Digits are read one at a time, so write numbers as words. Punctuation shapes the intonation: a period drops the pitch, a question mark raises it.

## Status

BUCK publishes these whenever they change. On relay.clasp.to they are CLASP params, and on relay.clasp.chat they are MQTT messages with the retain flag. Either way the relay stores the last value, so anyone subscribing late gets the current state straight away.

| leaf | type | meaning |
|---|---|---|
| `status/online` | bool | `true` when the link comes up. MQTT also sets a will of `false`, so the broker flips it if BUCK vanishes. CLASP has no will, so check `info` for freshness |
| `status/speaking` | bool | talking right now |
| `status/said` | string | the line it started speaking most recently |
| `status/info` | string, JSON | refreshed every 60 s, see below |
| `status/jaw` | float 0 to 1, CLASP stream | jaw openness about 15 times a second while talking, then a final 0. Not stored, CLASP only |
| `status/beat` | int (CLASP) or text (MQTT) | BUCK's heartbeat, every 45 s: seconds since boot. See "staying subscribed" below |

`info` looks like this:

```json
{"fw":"1.1.0","id":"heatsync","up":3600,"heap":94816,"minHeap":74560,"rssi":-54,
 "reset":"power","crashes":0,"queue":0,"heard":42,"limited":3,"voice":"sam",
 "clasp":"ready","mqtt":"ready"}
```

`up` is seconds since boot. `clasp` and `mqtt` are the state of each link: `ready`, `handshake`, `waiting` (backing off before a retry), or `off`. `reset` is why it last booted: `power`, `restart`, `usb`, `brownout`, `panic`, `task-wdt`, and a few rarer ones. `heard` and `limited` count network messages since boot.

Watch status:

```
python3 tools/buck.py watch                  # MQTT
python3 tools/buck.py watch --via clasp      # CLASP, needs: pip install websockets
mosquitto_sub -h relay.clasp.chat -t 'hackbuild/buck/heatsync/status/#' -v
```

```js
buck.on('/hackbuild/buck/heatsync/status/**', (value, address) => {
  console.log(address, value);
});
```

The CLASP callback gets `(value, address)` in that order.

## On the wire

You do not need this to use BUCK. It is here for anyone writing a client from scratch.

CLASP v3 frames are binary. Each WebSocket message carries exactly one frame, and the WebSocket subprotocol is `clasp`.

```
53            magic 'S'
41            flags: QoS 1 (bits 7-6), binary encoding (bits 2-0 = 1)
00 2e         payload length, big endian
...           payload
```

A client sends HELLO first and waits for WELCOME:

```
53 41 00 0d   01 01 e0   00 06 "my app"   00 00
              HELLO, version 1, features param+event+stream, name, empty token
```

Then an event with a string value:

```
53 41 00 2e   20 20   00 1c "/hackbuild/buck/heatsync/say"   01 08 00 0a "hello deer"
              PUBLISH, signal event (1 << 5), address, has value, string, text
```

The relay drops a PUBLISH that arrives before HELLO. It also reads one frame per WebSocket message, so two frames in one message lose the second. That rules out sending CLASP straight from `curl wss://`, which is why the curl examples use the MQTT door.

The full codec BUCK uses is `lib/buckproto/src/clasp_codec.cpp`, and `test/test_protocols` has frames captured from relay.clasp.to.

## Staying subscribed

The relays drop a session's subscriptions after five minutes in which the relay has sent that client nothing, and they do it without closing the socket. Pings keep getting answered, so the client looks connected and hears nothing. A listener that rarely gets messages, which is exactly what a deer on a wall is, falls into this.

BUCK guards against it with a heartbeat on each link. Every 45 s it publishes to its own `status/beat` and listens for that message to come back through its subscription:

- on CLASP, the relay echoes a SET to every subscriber of the address, the sender included
- on MQTT, the relay never sends a client its own publishes, so BUCK opens a second, short lived connection, publishes the beat from there, and disconnects

Either way the relay has something to deliver to BUCK every 45 s, which keeps the session active, and the delivery proves the subscription works end to end. If no beat comes back for 140 s, BUCK drops that link and reconnects.

Clients of your own that only listen should do the same, or reconnect every few minutes.

## Relay behaviour worth knowing

- On relay.clasp.chat every MQTT publish is stored, retained or not, and replayed to new subscribers before their SUBACK, with the retain flag cleared. BUCK ignores anything on a topic until its SUBACK arrives, so old messages are never spoken again after a reconnect.
- On relay.clasp.to a CLASP subscriber gets a SNAPSHOT of stored params first. BUCK ignores snapshots for the same reason.
- relay.clasp.chat requires a token for CLASP over WebSocket, and its guest tokens are scoped to `/chat/**`. That is why CLASP clients use relay.clasp.to.
