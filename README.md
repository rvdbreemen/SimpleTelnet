# SimpleTelnet

SimpleTelnet is a multi-client telnet server library for ESP8266 and ESP32. It supports two operating modes: a streaming mode that broadcasts writes to all connected clients (making it a drop-in replacement for TelnetStream), and a CLI mode with line-buffered input and `const char*` callbacks (making it a drop-in replacement for ESPTelnet). Both modes can run simultaneously, on different ports and with different client limits, using the same class. No `String` objects. No hidden globals. No heap surprises.

Built for Arduino C++, packaged as a self-contained library, and designed to keep its memory footprint honest.

**Since 2.0, the library ships two transports in one package, both on a shared protocol core:**

- **`SimpleTelnet`** — the **synchronous** transport (`WiFiServer`/`WiFiClient`), driven by `loop()`. Runs on **ESP8266 and ESP32**. `#include <SimpleTelnet.h>`.
- **`AsyncSimpleTelnet`** — the **event-driven async** transport on **AsyncTCP** (same stack as `AsyncWebServer`), with **no `loop()` polling**. **ESP32 only.** `#include <AsyncSimpleTelnet.h>`.

Both expose the same API — migrating is just swapping the include and the class name. And both are **protocol-aware telnet servers**: they implement RFC 854 option negotiation (see [Telnet protocol (RFC 854) compliance](#telnet-protocol-rfc-854-compliance)), not just a raw byte pipe.

---

## Why SimpleTelnet?

The honest version: SimpleTelnet was born from a specific heap regression in the [OTGW firmware](https://github.com/rvdbreemen/OTGW-firmware). The firmware was running three `WiFiServer` instances simultaneously: one for OTGW stream access, one for TelnetStream debug output, and one for a second debug channel. That's three separate TCP server objects, each with its own overhead, and the combination was enough to push available heap below the comfort zone.

The natural next step was to look at the existing libraries and figure out if one of them could do everything needed. Two candidates presented themselves.

**TelnetStream** (Juraj Andrassy) is excellent for the streaming use case: it treats the telnet connection as a `Stream`, writes go to all clients via `server.write()`, and reads come back through a clean polling interface. The weakness is that it has a hidden global instance (`TelnetStream.cpp:69`), which costs memory even when you never touch it. And it has no callbacks: no `onConnect`, no `onInputReceived`, nothing.

**ESPTelnet** (Lennart Hennigs) solves the callback side beautifully. `onConnect`, `onDisconnect`, `onInputReceived` with keepalive, line buffering, and a well-thought-out event model. The weakness: single-client only, and callbacks take `String` objects, which means heap allocation on every keystroke. (ESPTelnet also ships `ESPTelnetStream`, which is a hybrid combining `Stream` with callbacks. It's a good idea. It's still single-client.)

Could either be forked and extended? That's the question that eventually got answered.

Converting `ESPTelnetBase` (the internal base class in ESPTelnet) to a template for multi-client would have required around 270 lines of changes, touching nearly every method. It would have become a permanent fork to maintain, diverging from upstream on every release. And the `String` to `const char*` change in the callback signatures is not a minor tweak: it changes the API contract semantically. At that point, you're not patching a library; you're writing a new one wearing someone else's clothes.

The clean-sheet version came in around 365 lines of implementation. Full control over the memory layout. A single shared input buffer for CLI mode instead of one per client slot. No `String` anywhere. No globals.

To be absolutely clear: this is not because TelnetStream or ESPTelnet are bad. They're both good libraries that do their stated jobs well. The requirements here just grew beyond what either could cover without compromises. SimpleTelnet is what happens when you need both at once, with more clients, on hardware with 40KB of usable RAM.

---

## Credits and Shoutouts

SimpleTelnet owes a real debt to both libraries it learned from. These aren't pro-forma credits.

**Lennart Hennigs** (ESPTelnet: https://github.com/LennartHennigs/ESPTelnet): The callback architecture in SimpleTelnet is directly inspired by ESPTelnet. The keepalive design, the `onConnect`/`onDisconnect`/`onReconnect` event model, the `onConnectionAttempt` for rejected connections, the `emptyClientStream` pattern for draining stale data on reconnect: all of these were studied carefully in ESPTelnet and either adopted directly or improved upon. Lennart's code is clean and well-structured; it was a pleasure to read.

**Juraj Andrassy** (TelnetStream: https://github.com/jandrassy/TelnetStream): The streaming side of SimpleTelnet takes its cues from TelnetStream. Using `server.write()` for broadcast writes, the clean polling read pattern, the approach to ESP8266 vs ESP32 client state detection: these came from reading TelnetStream's source and understanding why the choices were made.

If you don't need multi-client or if you don't need callbacks, go use those libraries. They're good.

---

## Features

| Feature | TelnetStream | ESPTelnet | ESPTelnetStream | SimpleTelnet |
|---|---|---|---|---|
| Multi-client | broadcast only | No | No | Yes (`SimpleTelnet<N>`) |
| Streaming mode | Yes | No | Yes | Yes |
| CLI / line-input mode | No | Yes | Yes | Yes |
| Async / event-driven (AsyncTCP) | No | No | No | Yes (`AsyncSimpleTelnet`, ESP32) |
| Callbacks | No | Yes | Yes | Yes |
| Callback type | N/A | `String` | `String` | `const char*` |
| RFC 854 negotiation | minimal | partial | partial | Yes (parse/strip + refuse + ECHO/SGA) |
| No hidden globals | No | Yes | Yes | Yes |
| `printf_P` (PROGMEM) | No | No | No | Yes (ESP8266) |
| ESP8266 + ESP32 | Yes | Yes | Yes | Yes |
| Minimal RAM design | No | No | No | Yes |

### Complete feature list

**Transports**
- Synchronous `SimpleTelnet<N>` (`WiFiServer`/`WiFiClient`), **ESP8266 + ESP32**, driven by `loop()`.
- Asynchronous `AsyncSimpleTelnet<N>` (**AsyncTCP**), **ESP32**, fully event-driven, **no `loop()`**.
- Both derive from a shared transport-agnostic core (`SimpleTelnetCore`) → identical behaviour, no code drift.
- `loop()` exists in both (a no-op in async) so sketches port unchanged.

**Connections**
- Multi-client up to a compile-time `N` (template parameter); **static** per-client slots, no dynamic allocation.
- Broadcast writes to every connected client.
- Per-client IP tracking: `clientIP(i)`, `getIP()`, `getLastAttemptIP()`.
- Events: `onConnect`, `onDisconnect`, `onReconnect` (when `N==1`), `onConnectionAttempt` (rejected when full).
- `millis()`-based keep-alive with configurable interval (sync); RX-timeout mapping (async).
- Forced disconnect: `disconnectClient()` / `disconnectClient(index)`; `connectedCount()`, `isConnected()`.

**I/O modes**
- **Streaming** — Arduino `Stream`: `write`/`print`/`printf` broadcast; `available`/`read`/`peek` polling.
- **CLI line mode** — buffered line input (CR / LF / CR+LF, backspace), fires `onInputReceived(const char* line)`.
- **CLI char mode** — per-keystroke `onInputReceived(const char* key)`.
- Configurable line terminator (`setNewlineChar`); single shared CLI input buffer (RAM-saving); modes selectable per instance and can coexist.

**Telnet protocol (RFC 854 / 855 / 857 / 858 + RFC 1143)**
- `setTelnetNegotiation`: `NEG_OFF` (raw) / `NEG_REFUSE` (default) / `NEG_CHAR_ECHO`.
- Full IAC command-set parsing and `SB…SE` subnegotiation skipping.
- Refuse-by-default (`DO`→`WONT`, `WILL`→`DONT`) via a loop-safe scoped Q method.
- `ECHO` + `SUPPRESS-GO-AHEAD` negotiation with input echo (character-at-a-time).
- `IAC IAC` inbound → literal `0xFF`; outbound `0xFF` → `IAC IAC` escaping.
- `AYT` reply; `EC` → backspace, `EL` → line-clear.

**Output helpers**
- Full Arduino `Print`/`Stream` (`print`, `println`, `write`, `F()`).
- `printf()`; `printf_P()` (PROGMEM format strings, ESP8266) — 256-byte stack buffer with heap fallback.
- Async: non-blocking writes with **TX backpressure** (queued, drained on `onAck`).

**Memory & design**
- **No `String`** anywhere; `const char*` callbacks → no heap per keystroke.
- No hidden globals; explicit instantiation.
- Tunable buffer sizes via `#define` (CLI line buffer, IP length, printf buffer, async RX/TX rings).
- Footprint ≈ 489 B (`<1>`) … 1033 B (`<4>`) for the sync object.

**Platform & packaging**
- ESP8266 (sync) and ESP32 (sync + async).
- One library; **AsyncTCP is an optional dependency** (only pulled in when you include the async header).
- Arduino Library Manager + PlatformIO metadata; MIT licensed.

## Concepts

### Raw telnet vs RFC-854-compliant telnet

A telnet connection is *not* a plain TCP byte stream: clients send in-band
control bytes (the `IAC` escape `0xFF` and `WILL/WONT/DO/DONT` option
negotiation, usually right after connecting).

- **Raw** (`NEG_OFF`) — those control bytes are passed through untouched. Simple
  and fine for `nc` or binary pipes, but with a real telnet client the
  negotiation bytes can corrupt your input (a printable option code lands in your
  command), some clients stall waiting for a reply, and a `0xFF` in your output is
  misread as a command. This was the pre-2.0 behaviour.
- **Compliant** (`NEG_REFUSE`, default; or `NEG_CHAR_ECHO`) — the server speaks
  telnet: it parses and strips `IAC` sequences, answers negotiation
  (refuse-by-default), escapes outbound `0xFF`, and can negotiate ECHO/SGA. Your
  callbacks and `read()` see clean data, and PuTTY / Windows `telnet` behave
  predictably. Details: [RFC 854 compliance](#telnet-protocol-rfc-854-compliance).

### CLI style vs streaming style

Both are selected per instance; they can run side by side (see `DualInstance`).

- **Streaming** (default; no `onInputReceived`) — the instance behaves like an
  Arduino `Stream`. Output via `write`/`print`/`printf` (broadcast to all
  clients); input by polling `available()` / `read()` / `peek()`. Ideal for log
  or serial mirroring and any `Stream`-based code (drop-in for TelnetStream).
- **CLI** (register `onInputReceived`) — input is delivered to your callback:
  - **line mode** (`setLineMode(true)`): buffered until a newline, then fired once
    with the full line (no newline) — ideal for typed commands.
  - **char mode** (`setLineMode(false)`): fired per keystroke with a 1-char string
    — ideal for single-key menus (pair with `NEG_CHAR_ECHO` so keys echo).

## How it compares

**Synchronous peers** (see the table above):

- **TelnetStream** (Juraj Andrassy) — great streaming `Stream` server, but a
  hidden global, no callbacks, broadcast-only multi-client.
- **ESPTelnet / ESPTelnetStream** (Lennart Hennigs) — excellent callback/event
  model, but single-client and `String`-based (heap per keystroke).

**Asynchronous peers** — there is **no widely-adopted standalone async telnet
library**; the popular ones are synchronous, and the async implementations that
exist are embedded inside firmware projects:

- **robdobsn/AsyncTelnetServer** — closest peer (AsyncTCP, multi-client,
  `const char*`). SimpleTelnet adds: line/CLI parsing, RFC 854 negotiation, the
  Arduino `Stream` interface, real TX backpressure, a mutex, heap-free static
  allocation, and a drop-in shared API with the sync library.
- **jeremypoulter/WiFiTelnetToSerial** — ESPAsyncTCP telnet↔serial bridge (an
  app, not a packaged library).

A detailed, code-level comparison (functionality + implementation patterns) is in
[`docs/ASYNC_COMPARISON.md`](docs/ASYNC_COMPARISON.md).

---

## Installation

### Arduino Library Manager

Search for **SimpleTelnet** and click Install.

### PlatformIO

```ini
lib_deps = rvdbreemen/SimpleTelnet
```

### Manual

Copy `src/libraries/SimpleTelnet/` into your Arduino libraries folder.

---

## Quick Start

### Streaming mode (TelnetStream replacement)

Up to four clients can connect simultaneously. All writes go to every connected client.

```cpp
#include <SimpleTelnet.h>

SimpleTelnet<4> telnet(23);   // up to 4 clients, port 23

void setup() {
  WiFi.begin("ssid", "pass");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  telnet.begin();
}

void loop() {
  telnet.loop();
  telnet.println(F("Hello from ESP!"));
}
```

### CLI mode (ESPTelnet replacement)

Single client, line-buffered input, callbacks for connect and input events.

```cpp
#include <SimpleTelnet.h>

SimpleTelnet<1> telnet(23);

void setup() {
  WiFi.begin("ssid", "pass");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  telnet.setLineMode(true);

  telnet.onConnect([](const char* ip) {
    telnet.printf_P(PSTR("Connected from %s\r\nType something:\r\n"), ip);
  });

  telnet.onInputReceived([](const char* line) {
    telnet.printf_P(PSTR("You typed: %s\r\n"), line);
  });

  telnet.begin();
}

void loop() {
  telnet.loop();
}
```

### Async mode (ESP32, no `loop()` polling)

Same API on AsyncTCP — events arrive on their own, so there is no polling.
Install [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) and just swap the
include + class name.

```cpp
#include <AsyncSimpleTelnet.h>

AsyncSimpleTelnet<4> telnet(23);   // up to 4 clients, ESP32 only

void setup() {
  WiFi.begin("ssid", "pass");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  telnet.setLineMode(true);
  telnet.onConnect([](const char* ip) {
    telnet.printf("Connected from %s\r\n", ip);
  });
  telnet.onInputReceived([](const char* line) {
    telnet.printf("You typed: %s\r\n", line);
  });

  telnet.begin();   // event-driven from here on
}

void loop() {
  // No telnet.loop() needed — AsyncTCP delivers events on its own task.
  // (loop() still exists as a no-op, so sync sketches port unchanged.)
}
```

Runs alongside `AsyncWebServer` out of the box — see the full
[`examples/AsyncWebServerTelnet`](examples/AsyncWebServerTelnet) sketch.

### Interactive character-at-a-time CLI (RFC 854 ECHO/SGA)

```cpp
telnet.setLineMode(false);                              // per-keystroke
telnet.setTelnetNegotiation(SimpleTelnet<1>::NEG_CHAR_ECHO);  // server echoes
telnet.onInputReceived([](const char* key) {
  if (key[0] == 'h') telnet.println(F("help / info / quit"));
});
```

---

## Breaking Change

If you are migrating from **ESPTelnet**, callbacks now take `const char*` instead of `String`. This is intentional: it eliminates heap allocation on every callback invocation, which matters on ESP8266.

**Before (ESPTelnet):**
```cpp
telnet.onInputReceived([](String str) {
  Serial.println(str);
});
```

**After (SimpleTelnet):**
```cpp
telnet.onInputReceived([](const char* str) {
  Serial.println(str);
});
```

The port is also passed to the constructor rather than to `begin()`:

**Before (ESPTelnet):**
```cpp
ESPTelnet telnet;
telnet.begin(23);
```

**After (SimpleTelnet):**
```cpp
SimpleTelnet<1> telnet(23);
telnet.begin();
```

---

## API Reference

See [API.md](API.md) for the full reference.

Key differences from ESPTelnet at a glance:
- All callbacks take `const char*`, never `String`
- Port is a constructor argument: `SimpleTelnet<1> t(23)` rather than `t.begin(23)`
- `begin(bool checkConnection = true)`: pass `false` to bind unconditionally (useful when the server must start before WiFi is fully up)
- `MAX_CLIENTS` is a template parameter, resolved at compile time

Key differences from TelnetStream at a glance:
- No hidden global: you instantiate explicitly
- `loop()` must be called from `loop()` for keepalive and callbacks
- `MAX_CLIENTS` is a template parameter, not a runtime value

### Telnet (RFC 854) negotiation

`setTelnetNegotiation(mode)` controls IAC option negotiation:
- `NEG_REFUSE` (default) — parse and strip IAC sequences, politely refuse every
  option (`DO`→`WONT`, `WILL`→`DONT`), skip subnegotiation, honour `IAC IAC`,
  reply to `AYT`, and treat `EC`/`EL` as backspace / line-clear. Outbound `0xFF`
  is escaped as `IAC IAC`.
- `NEG_CHAR_ECHO` — also negotiates `ECHO` + `SUPPRESS-GO-AHEAD` and echoes
  input, for an interactive character-at-a-time CLI.
- `NEG_OFF` — legacy raw passthrough (no interpretation).

---

## Memory Footprint

Measured on ESP8266. lwIP socket pools are statically allocated by the network stack regardless of which library you use; these numbers reflect only the SimpleTelnet object itself.

| Instance | Approx. RAM |
|---|---|
| `SimpleTelnet<1>` | ~489 B |
| `SimpleTelnet<4>` | ~1033 B |

No `String` objects are allocated. No heap fragmentation from callbacks. The CLI input buffer is shared across all client slots rather than allocated per slot: `MAX_CLIENTS * SIMPLETELNET_LINE_BUF_LEN` bytes become just `SIMPLETELNET_LINE_BUF_LEN` bytes (128 bytes by default, overridable at compile time).

---

## Architecture

The library ships **two transports in one package**, layered on a shared,
transport-agnostic protocol core so the parsing logic is never duplicated:

- **`SimpleTelnetCore`** (`src/SimpleTelnetCore.h`) — transport-agnostic base
  class with all the protocol logic: line/CR-LF parsing, streaming vs CLI mode,
  callbacks, IP formatting, `printf`/`printf_P` and RFC 854 negotiation. It knows
  nothing about sockets; the transport feeds received bytes in and output goes
  out through the virtual Arduino `Stream` `write()`.
- **`SimpleTelnet`** (`src/SimpleTelnet.h`) — synchronous transport on
  `WiFiServer`/`WiFiClient[]`, driven by `loop()`. ESP8266 + ESP32.
  `#include <SimpleTelnet.h>`.
- **`AsyncSimpleTelnet`** (`src/AsyncSimpleTelnet.h`) — event-driven transport on
  **AsyncTCP** (same ecosystem as `AsyncWebServer`), no `loop()` polling. ESP32
  only. `#include <AsyncSimpleTelnet.h>`. **Prototype — not yet hardware-validated.**

Both derive from the same core, so they share one public API: migrate by swapping
the include + class name (`loop()` becomes a no-op in async). See
[docs/ASYNC.md](docs/ASYNC.md) and the library comparison in
[docs/ASYNC_COMPARISON.md](docs/ASYNC_COMPARISON.md).

### Choosing a variant / dependencies

| | include | platforms | extra dependency |
|---|---|---|---|
| Synchronous | `SimpleTelnet.h` | ESP8266 + ESP32 | none |
| Asynchronous | `AsyncSimpleTelnet.h` | ESP32 | **AsyncTCP** (install yourself) |

`AsyncTCP` is intentionally **not** a hard dependency of this library (it is not
listed in `depends=`), so sync/ESP8266 users are unaffected. The async header is
header-only and only pulls in `<AsyncTCP.h>` when you include it — install
[ESP32Async/AsyncTCP](https://github.com/ESP32Async/AsyncTCP) (PlatformIO:
add it to `lib_deps`) to use the async variant.

---

## Telnet protocol (RFC 854) compliance

SimpleTelnet is a protocol-aware telnet server, not just a raw TCP pipe. All IAC
handling lives in the shared core, so the **sync and async variants behave
identically**. Behaviour is selected with `setTelnetNegotiation(mode)` (default
`NEG_REFUSE`).

**Implemented (RFC 854 / 855 / 857 / 858, plus RFC 1143 for loop-safe negotiation):**

- **Full IAC command parsing** — the entire command set (`SE, NOP, DM, BRK, IP,
  AO, AYT, EC, EL, GA, SB, WILL, WONT, DO, DONT, IAC`) is recognised and removed
  from the data stream, so option bytes never leak into your input (a printable
  option code can no longer end up in the line buffer).
- **Option negotiation** — every request is answered: `DO`→`WONT`,
  `WILL`→`DONT` (refuse), driven by a scoped **RFC 1143 "Q method"** state
  machine so it never loops.
- **ECHO (RFC 857) + SUPPRESS-GO-AHEAD (RFC 858)** — in `NEG_CHAR_ECHO` the
  server proactively negotiates both and echoes input → a true
  character-at-a-time CLI.
- **Subnegotiation** — `IAC SB … IAC SE` blocks are skipped safely.
- **Data transparency** — an incoming `IAC IAC` becomes a single literal
  `0xFF`; an outgoing `0xFF` data byte is escaped to `IAC IAC`, so binary output
  stays safe on the wire.
- **AYT** (Are You There) gets a visible reply; **EC** / **EL** act as
  backspace / clear-line in CLI mode.

**Intentionally not implemented (and still compliant):**

- The option "zoo" — NAWS, TERMINAL-TYPE, LINEMODE, NEW-ENVIRON, CHARSET,
  BINARY. We correctly **refuse** these, which RFC 855 permits; they add
  subnegotiation state for negligible value on a debug/CLI server.
- **Go-Ahead** signalling and the **Synch / Data-Mark** (TCP urgent) mechanism —
  obsolete half-duplex concepts no modern client/server uses. We suppress GA via
  SGA and consume DM harmlessly, which is the standard modern behaviour.

**Modes:**

| Mode | Behaviour |
|---|---|
| `NEG_REFUSE` *(default)* | parse + strip IAC, refuse all options, escape output, handle AYT/EC/EL |
| `NEG_CHAR_ECHO` | as above, **plus** negotiate ECHO/SGA and echo input (interactive CLI) |
| `NEG_OFF` | legacy raw passthrough — no interpretation (pre-2.0 behaviour) |

> **Note (2.0 default change):** before 2.0, IAC bytes were passed through raw.
> Since 2.0 the default (`NEG_REFUSE`) strips and answers them, so `read()` and
> char-mode `onInputReceived` see clean NVT data. Use `NEG_OFF` to restore the
> old raw behaviour.

---

## License

MIT: see [LICENSE](LICENSE)
