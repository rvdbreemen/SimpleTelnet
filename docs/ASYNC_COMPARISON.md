# AsyncSimpleTelnet vs existing ESP32 telnet libraries

Research done 2026-06-16. Goal: check whether an async telnet library for ESP32
already exists, pick the most relevant projects, and compare them against this
prototype on functionality and implementation patterns.

## Key finding

There is **no widely-adopted, standalone _async_ telnet library** for ESP32.
The popular telnet libraries are all **synchronous** (`WiFiServer`/`WiFiClient`
+ a required `loop()`). The genuinely async telnet implementations that exist
are **embedded inside firmware projects**, not packaged as reusable libraries
(no Arduino Library Manager / PlatformIO registry entry). This validates the
niche AsyncSimpleTelnet targets.

## The three most relevant projects

| # | Project | Transport | Role |
|---|---------|-----------|------|
| 1 | [LennartHennigs/ESPTelnet](https://github.com/LennartHennigs/ESPTelnet) (~257★) | `WiFiServer` (**sync**) | The de-facto telnet lib; functionality baseline |
| 2 | [robdobsn/AsyncTelnetServer](https://github.com/robdobsn/RBotFirmware/tree/master/PlatformIO/lib/AsyncTelnetServer) | AsyncTCP (**async**) | Closest architectural peer (embedded lib) |
| 3 | [jeremypoulter/WiFiTelnetToSerial](https://github.com/jeremypoulter/WiFiTelnetToSerial) | ESPAsyncTCP (**async**) | Async telnet↔serial bridge (app) |

(Other sync references: [jandrassy/TelnetStream](https://github.com/jandrassy/TelnetStream),
[yasheena/telnetspy](https://github.com/yasheena/telnetspy).)

## Feature comparison

| Feature | ESPTelnet | robdobsn AsyncTelnetServer | jeremypoulter W.T.T.S. | **AsyncSimpleTelnet (this)** |
|---|---|---|---|---|
| Async / event-driven | ❌ sync, needs `loop()` | ✅ AsyncTCP | ✅ ESPAsyncTCP | ✅ AsyncTCP, `loop()` = no-op |
| Multi-client | ❌ single | ✅ 3 (`std::vector`) | ✅ unbounded (linked list) | ✅ `<N>` template, fixed |
| Callback payload type | `String` | `const char*` | `uint8_t*` | `const char*` |
| Heap in steady state | yes (String) | yes (`std::vector`/`new`) | yes (linked list/`new`) | **no** (static arrays) |
| Line / CLI mode parsing | ✅ | ❌ raw | ❌ raw | ✅ (CR/LF/CR+LF) |
| Telnet IAC byte handling | partial | ❌ | ❌ | ✅ RFC 854 (parse/strip, refuse, ECHO/SGA, IAC-escape) |
| Arduino `Stream` interface | ✅ (sync) | ❌ callback-only | ❌ callback-only | ✅ via RX ring |
| TX backpressure | n/a (blocking) | ❌ `canSend()` then drop | ~ tracks `toSend` | ✅ TX ring drained on `onAck` |
| Thread-safety (mutex) | n/a | ❌ none | ❌ none | ✅ recursive mutex |
| Packaged as a library | ✅ (LibMgr/PIO) | ❌ in-firmware | ❌ app | ✅ (this repo) |
| Hardware-proven | ✅ mature | ✅ (in its firmware) | ✅ (in its app) | ⚠️ **prototype, untested on HW** |

## Implementation-pattern contrasts (concrete)

### 1. Accepting a connection

**robdobsn** — dynamic, heap-backed sessions; reject by delete:
```cpp
_sessions.resize(MAX_SESSIONS);              // std::vector<AsyncTelnetSession*>
// on full: c->close(true); c->free(); delete c;
```

**AsyncSimpleTelnet** — fixed slots, no heap; same reject pattern:
```cpp
for (uint8_t i = 0; i < MAX_CLIENTS; i++)
  if (!this->_clientActive[i]) { _attachClient(i, c); return; }
// ... full: onConnectionAttempt(); c->close(true); delete c;
```
*Trade-off:* `std::vector`/`new` is flexible but fragments the heap; the fixed
template array is RAM-deterministic (matches SimpleTelnet's minimal-RAM ethos)
at the cost of a compile-time client cap.

### 2. Sending data + backpressure

**robdobsn** — best-effort, drops if the send buffer is busy:
```cpp
if (_pClient && _pClient->connected() && _pClient->canSend())
    _pClient->write(pChars, numChars);     // no queue, no retry
```

**AsyncSimpleTelnet** — queue + drain on ACK, so a momentarily full TCP buffer
does not lose data (up to the ring size):
```cpp
_tx[i].push(buf, n);                        // per-client ring
_flushTx(i);                                // add()/send() up to space()
// onAck -> _flushTx(idx) again
```
*Trade-off:* the TX ring costs RAM per client but avoids silent data loss on
bursty output (e.g. broadcast debug logs) — the exact regression that motivated
SimpleTelnet.

### 3. Receiving data

**robdobsn / jeremypoulter** — raw bytes straight to a callback, no parsing:
```cpp
typedef std::function<void(void*, const char* pData, size_t numChars)> ...;
```

**AsyncSimpleTelnet** — same const-char* philosophy, but routes through the
shared `SimpleTelnetCore`: in CLI mode it assembles complete lines (handling
CR/LF/CR+LF, backspace, and ignoring telnet IAC ≥0x80); in pull mode it fills a
per-client RX ring so `read()/available()/peek()` keep working.

### 4. Thread-safety

robdobsn and jeremypoulter register AsyncTCP callbacks (which run on the
AsyncTCP task) and also `write()` from the app task **without a mutex** — a
latent race on the session list / buffers. AsyncSimpleTelnet guards shared
state with a **recursive** mutex so user callbacks fired while locked may call
back into `write()/printf()`.
*Trade-off:* correctness vs. the small risk of holding a lock briefly on the
AsyncTCP task — callbacks must stay light (documented).

## Differences & trade-offs — summary

- **Closest peer is robdobsn/AsyncTelnetServer.** Same core idea (AsyncTCP,
  multi-client, `const char*` callbacks, lambda trampolines casting `void*`).
  AsyncSimpleTelnet adds: line/CLI parsing, IAC handling, the Arduino `Stream`
  interface, real TX backpressure, a mutex, and static (heap-free) allocation —
  plus a **drop-in API shared with the synchronous SimpleTelnet**, so the same
  sketch runs sync on ESP8266 and async on ESP32 by swapping the class name.
- **vs ESPTelnet:** ESPTelnet wins on maturity and ecosystem presence, but is
  synchronous, single-client and `String`-based. AsyncSimpleTelnet is async,
  multi-client and allocation-free.
- **Biggest honest gap:** maturity. ESPTelnet/robdobsn/jeremypoulter are proven
  in real deployments; AsyncSimpleTelnet is a **prototype not yet validated on
  hardware** (host-compiled and unit-tested only). The AsyncTCP delete-on-
  disconnect lifecycle and the mutex-in-callback behaviour are the two areas to
  validate on a device first.
