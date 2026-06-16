# AsyncSimpleTelnet (prototype)

Event-driven, multi-client telnet server for **ESP32**, built on **AsyncTCP**
(the same stack as `ESPAsyncWebServer`). It is the asynchronous counterpart of
[SimpleTelnet](../README.md), shipped in the **same library** (`src/AsyncSimpleTelnet.h`)
and sharing the exact same protocol core (`SimpleTelnetCore`), so it has the
**same public API**.

> **Status: prototype.** Host-compiled and unit-tested against API stubs, but
> **not yet validated on hardware.** It is `#include <AsyncSimpleTelnet.h>` in
> this library; AsyncTCP is an optional dependency you install yourself.

## Why

The synchronous `SimpleTelnet` already coexists with an `AsyncWebServer` on
ESP32 as long as you call `telnet.loop()`. AsyncSimpleTelnet removes that
requirement: connections, data and disconnects arrive as AsyncTCP events, so
there is **no polling** and writes are non-blocking with backpressure handling.

See [ASYNC_COMPARISON.md](ASYNC_COMPARISON.md) for how this compares to existing
ESP32 telnet libraries (ESPTelnet, robdobsn/AsyncTelnetServer, WiFiTelnetToSerial).

## Drop-in migration

```cpp
// synchronous (ESP8266 + ESP32)
#include <SimpleTelnet.h>
SimpleTelnet<4> telnet(23);

// asynchronous (ESP32)
#include <AsyncSimpleTelnet.h>
AsyncSimpleTelnet<4> telnet(23);
```

Everything else is identical: `begin()`, `setLineMode()`, the `onConnect` /
`onInputReceived` / … callbacks (`const char*`), `write()`, `printf()`,
`read()`/`available()`/`peek()`, `clientIP()`. `telnet.loop()` still exists as a
**no-op** so existing sketches compile unchanged.

## What is different under the hood

- **No `loop()`** — driven by AsyncTCP `onClient`/`onData`/`onAck`/`onDisconnect`.
- **RX ring buffer per client** keeps the Arduino `Stream` `read()` interface
  working (only used in pull mode; with an `onInputReceived` callback set, bytes
  are parsed immediately and the ring stays empty).
- **TX ring buffer per client** for backpressure: data that does not fit the
  AsyncClient send buffer is queued and flushed on the `onAck` event.
- **Recursive mutex** guards shared state, because AsyncTCP callbacks run on the
  AsyncTCP task while `write()`/`printf()` run on your task. Keep callbacks light.

## Tunables (define before include)

| Macro | Default | Meaning |
|---|---|---|
| `SIMPLETELNET_RX_BUF_LEN` | 256 | per-client RX ring (pull mode) |
| `SIMPLETELNET_TX_BUF_LEN` | 512 | per-client TX ring (backpressure) |
| `SIMPLETELNET_TX_CHUNK_LEN` | 256 | max bytes per `add()` step |
| `SIMPLETELNET_LINE_BUF_LEN` | 128 | shared CLI line buffer (from core) |

## Dependencies

- [ESP32Async/AsyncTCP](https://github.com/ESP32Async/AsyncTCP)
- (example also) [ESP32Async/ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer)

## To validate on hardware first

1. AsyncClient lifetime: we `delete` the client on `onDisconnect` and on
   explicit close — confirm no double-free against the installed AsyncTCP.
2. Mutex held briefly inside AsyncTCP-task callbacks — confirm it does not stall
   the stack under load.
3. TX backpressure under sustained broadcast (multiple clients, slow link).
