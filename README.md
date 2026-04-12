# SimpleTelnet

SimpleTelnet is a multi-client telnet server library for ESP8266 and ESP32. It supports two operating modes: a streaming mode that broadcasts writes to all connected clients (making it a drop-in replacement for TelnetStream), and a CLI mode with line-buffered input and `const char*` callbacks (making it a drop-in replacement for ESPTelnet). Both modes can run simultaneously, on different ports and with different client limits, using the same class. No `String` objects. No hidden globals. No heap surprises.

Built for Arduino C++, packaged as a self-contained library, and designed to keep its memory footprint honest.

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
| Callbacks | No | Yes | Yes | Yes |
| Callback type | N/A | `String` | `String` | `const char*` |
| No hidden globals | No | Yes | Yes | Yes |
| `printf_P` (PROGMEM) | No | No | No | Yes (ESP8266) |
| ESP8266 + ESP32 | Yes | Yes | Yes | Yes |
| Minimal RAM design | No | No | No | Yes |

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

---

## Memory Footprint

Measured on ESP8266. lwIP socket pools are statically allocated by the network stack regardless of which library you use; these numbers reflect only the SimpleTelnet object itself.

| Instance | Approx. RAM |
|---|---|
| `SimpleTelnet<1>` | ~489 B |
| `SimpleTelnet<4>` | ~1033 B |

No `String` objects are allocated. No heap fragmentation from callbacks. The CLI input buffer is shared across all client slots rather than allocated per slot: `MAX_CLIENTS * SIMPLETELNET_LINE_BUF_LEN` bytes become just `SIMPLETELNET_LINE_BUF_LEN` bytes (128 bytes by default, overridable at compile time).

---

## License

MIT: see [LICENSE](LICENSE)
