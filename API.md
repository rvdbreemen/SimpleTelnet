# SimpleTelnet API Reference

SimpleTelnet is a multi-client telnet server library for ESP8266 and ESP32. It supports two
operating modes per instance: **streaming** (drop-in for TelnetStream) and **CLI / line-input**
(drop-in for ESPTelnet). The full API is an extension of both, so migration is mostly additive.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Template Parameter — MAX_CLIENTS and Memory](#template-parameter)
3. [Configuration Defines](#configuration-defines)
4. [Full Method Reference](#full-method-reference)
   - [Constructor](#constructor)
   - [Lifecycle](#lifecycle)
   - [Mode Selection](#mode-selection)
   - [Connection State](#connection-state)
   - [IP Helpers](#ip-helpers)
   - [Keep-Alive](#keep-alive)
   - [Callbacks](#callbacks)
   - [Stream Interface](#stream-interface)
   - [printf Helpers](#printf-helpers)
5. [Operating Modes in Depth](#operating-modes-in-depth)
6. [BREAKING CHANGE: Callbacks Use const char*, Not String](#breaking-change)
7. [Platform Notes](#platform-notes)
8. [Migration Guide](#migration-guide)
   - [From TelnetStream](#from-telnetstream)
   - [From ESPTelnet / ESPTelnetStream](#from-esptelnet--esptelnetstream)

---

## Quick Start

```cpp
#include <SimpleTelnet.h>

SimpleTelnet<1> telnet(23);   // single-client on port 23

void setup() {
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) delay(100);
  telnet.begin();
  telnet.onInputReceived([](const char* line) {
    telnet.printf("echo: %s\r\n", line);
  });
}

void loop() {
  telnet.loop();
}
```

---

## Template Parameter

```cpp
template<uint8_t MAX_CLIENTS = 1>
class SimpleTelnet : public Stream
```

`MAX_CLIENTS` sets the maximum number of simultaneously accepted TCP connections. It is a
compile-time constant; the library allocates all client slots statically — no heap allocation.

### Memory cost table

The values below are for the SimpleTelnet object itself (stack/BSS), excluding lwIP socket
buffers (which are allocated by the network stack per connection, typically 5-7 KB each).

| Instantiation | Approx. object size |
|--------------|---------------------|
| `SimpleTelnet<1>` | ~489 bytes |
| `SimpleTelnet<2>` | ~625 bytes |
| `SimpleTelnet<4>` | ~1033 bytes |

The dominant term is `_clients[MAX_CLIENTS]` (one `WiFiClient` per slot ≈ 136 bytes each on
ESP8266) plus `_ip[MAX_CLIENTS][16]`.

**Rule of thumb:** For a debug terminal use `<1>`. For a multi-reader OT log stream use `<4>`.
Higher values save nothing if clients rarely connect simultaneously.

---

## Configuration Defines

Override any of the following with a `#define` **before** `#include <SimpleTelnet.h>`.

| Define | Default | Description |
|--------|---------|-------------|
| `SIMPLETELNET_LINE_BUF_LEN` | `128` | CLI input line buffer length in bytes. Longer lines are silently truncated. |
| `SIMPLETELNET_IP_LEN` | `16` | IP string buffer length. Default fits `xxx.xxx.xxx.xxx\0` (15+1). |
| `SIMPLETELNET_MAX_WRITE_ERRORS` | `3` | Consecutive write failures before a client is evicted. |
| `SIMPLETELNET_KEEPALIVE_MS` | `1000` | Keep-alive polling interval in milliseconds. |

Example:

```cpp
#define SIMPLETELNET_LINE_BUF_LEN 256
#define SIMPLETELNET_KEEPALIVE_MS 500
#include <SimpleTelnet.h>
```

---

## Full Method Reference

### Constructor

```cpp
explicit SimpleTelnet(uint16_t port = 23);
```

Creates an instance bound to `port`. The port is fixed at construction time. Declare the object
as a global variable so it persists for the lifetime of the sketch.

```cpp
SimpleTelnet<1> debugTelnet(23);        // single-client debug terminal, port 23
SimpleTelnet<4> otgwStream(1234);       // four-client OT stream, port 1234
```

---

### Lifecycle

| Signature | Description |
|-----------|-------------|
| `bool begin(bool checkConnection = true)` | Start listening. Returns `false` if `checkConnection` is `true` and WiFi is not connected and the ESP is not in AP mode. Pass `false` to bind unconditionally (e.g. before WiFi is up). |
| `void stop()` | Disconnect all clients and close the server socket. |
| `void loop()` | **Must be called from `loop()`**. Polls for new connections, checks keep-alive, and reads incoming data from all clients. |

```cpp
// Bind unconditionally — useful when WiFi may not be up yet
telnet.begin(false);
```

---

### Mode Selection

| Signature | Description |
|-----------|-------------|
| `void setLineMode(bool enable = true)` | Enable (`true`) or disable (`false`) CLI / line-input mode. Default is streaming mode (disabled). |
| `bool isLineMode() const` | Returns `true` if CLI mode is currently active. |
| `void setNewlineChar(char c)` | Set the character that terminates a line in CLI mode. Default is `'\n'`. |

Calling `setLineMode(true)` after `onInputReceived` is already registered is allowed but
changes behavior mid-connection — see [Operating Modes in Depth](#operating-modes-in-depth).

---

### Connection State

| Signature | Description |
|-----------|-------------|
| `uint8_t connectedCount() const` | Number of currently active client slots. |
| `bool isConnected() const` | `true` if at least one client is connected. |
| `void disconnectClient()` | Forcibly disconnect **all** clients. Fires `onDisconnect` for each. |
| `void disconnectClient(uint8_t index, bool triggerEvent = true)` | Disconnect client at slot `index` (0-based). Set `triggerEvent = false` to suppress the `onDisconnect` callback. |

---

### IP Helpers

| Signature | Description |
|-----------|-------------|
| `const char* getIP() const` | IP address string of the most recently connected client. |
| `const char* getLastAttemptIP() const` | IP address string of the last connection attempt that was rejected (server full). |

Both return pointers to **internal `char[16]` buffers**. Copy the string immediately if you need
to keep it past the next `loop()` call, because the buffer may be overwritten.

```cpp
char savedIP[16];
strlcpy(savedIP, telnet.getIP(), sizeof(savedIP));
```

---

### Keep-Alive

| Signature | Description |
|-----------|-------------|
| `void setKeepAliveInterval(uint16_t ms)` | Set how often (in ms) `loop()` checks whether clients are still alive. Default: `SIMPLETELNET_KEEPALIVE_MS` (1000 ms). |
| `uint16_t getKeepAliveInterval() const` | Get the current keep-alive interval in ms. |

The keep-alive check uses `client.status() == ESTABLISHED` (TCP state 4) on ESP8266 and
`client.connected()` on ESP32. Dead clients are evicted and `onDisconnect` is fired.

---

### Callbacks

All callbacks have signature `void callback(const char* str)`. The `str` argument is:
- IP address string — for connect / disconnect / reconnect / attempt callbacks
- Received line — for `onInputReceived`

| Method | Fires when | `str` value |
|--------|------------|-------------|
| `onConnect(SimpleTelnetCallback f)` | A new client connects | Client IP |
| `onDisconnect(SimpleTelnetCallback f)` | A client disconnects or is evicted | Client IP |
| `onReconnect(SimpleTelnetCallback f)` | A client reconnects to a slot that was previously occupied | Client IP |
| `onConnectionAttempt(SimpleTelnetCallback f)` | A connection is rejected (all slots full) | Attempting client IP |
| `onInputReceived(SimpleTelnetCallback f)` | A complete line is received in CLI mode | Line content (no newline) |

`onInputReceived` is also the trigger that activates CLI mode implicitly — see
[Operating Modes in Depth](#operating-modes-in-depth).

```cpp
telnet.onConnect([](const char* ip) {
  Serial.printf("Client connected: %s\n", ip);
});

telnet.onInputReceived([](const char* line) {
  telnet.printf("You typed: %s\r\n", line);
});
```

---

### Stream Interface

`SimpleTelnet` inherits from Arduino `Stream` (which inherits from `Print`). All standard
`print()`, `println()`, and operator `<<` methods are available.

| Signature | Description |
|-----------|-------------|
| `size_t write(uint8_t val)` | Write one byte to **all** active clients. |
| `size_t write(const uint8_t* buf, size_t size)` | Write a buffer to **all** active clients. |
| `int available()` | Bytes available from the first client that has data. |
| `int read()` | Read one byte from the first client that has data. Returns -1 if none. |
| `int peek()` | Peek at the next byte from the first client that has data. Returns -1 if none. |
| `void flush()` | Flush the output buffer of all active clients. On ESP8266: blocks until all data is sent or a timeout occurs. |

Because `write()` broadcasts to all clients, SimpleTelnet can be used as a drop-in `Stream`
target for any library that calls `stream.print()` or `stream.write()`.

```cpp
// Use as output-only stream target
someLib.setOutput(telnet);
```

---

### printf Helpers

```cpp
void printf(const char* fmt, ...);
```

Formats a string and writes it to all active clients. Uses `vsnprintf` on ESP32 and
`vsnprintf_P`-compatible formatting on ESP8266. Maximum formatted output is 255 characters per
call (buffer is stack-allocated).

```cpp
telnet.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
```

```cpp
// ESP8266 only — format string in flash (PROGMEM)
void printf_P(PGM_P fmt, ...);
```

On ESP8266, format strings passed to `printf_P` must reside in flash memory. Use `PSTR()` or
the `F()` macro for the format string. This avoids copying the format string to RAM.

```cpp
// ESP8266 only
telnet.printf_P(PSTR("Value: %d\r\n"), value);
```

`printf_P` is not available on ESP32 (where RAM is less constrained and `PSTR()` is a no-op).

---

## Operating Modes in Depth

SimpleTelnet instances operate in one of two modes, selectable at runtime.

### Streaming Mode (default)

- Received bytes stay in the TCP receive buffer.
- `available()`, `read()`, and `peek()` give direct access to the incoming byte stream.
- `onInputReceived` is **not** called.
- Use this when you want to poll or forward raw data (e.g. an OT log stream).

### CLI Mode (line-buffered)

- Received bytes are accumulated in an internal `char[SIMPLETELNET_LINE_BUF_LEN]` buffer.
- When the newline character (default `'\n'`) is received, the accumulated line is passed to
  `onInputReceived` as a NUL-terminated string without the trailing newline.
- `available()` and `read()` return 0 / -1 while data is in the line buffer — bytes are consumed
  by the buffer, not left in the TCP stream.
- CR+LF sequences are handled: a `'\r'` followed by `'\n'` counts as one line terminator.

**How to activate CLI mode:**

```cpp
// Explicit:
telnet.setLineMode(true);

// Implicit (registering the callback enables CLI mode automatically
// if setLineMode was not called explicitly):
telnet.onInputReceived([](const char* line) { ... });
```

Note: registering `onInputReceived` alone does **not** call `setLineMode(true)` internally.
You should call `setLineMode(true)` explicitly if you want CLI mode, and *also* register the
callback. The two steps are independent: mode controls buffering, callback controls what fires
when a line is complete.

**Mode interaction summary:**

| `onInputReceived` registered | `setLineMode(true)` called | Behaviour |
|------------------------------|---------------------------|-----------|
| No | No | Streaming — bytes via `read()` |
| No | Yes | CLI buffering active, but no callback fires |
| Yes | No | Streaming — bytes via `read()`, callback never fires |
| Yes | Yes | CLI mode — callback fires per line, `read()` returns -1 |

---

## BREAKING CHANGE

### Callbacks use `const char*` instead of `String`

ESPTelnet and ESPTelnetStream callbacks used `String` parameters:

```cpp
// ESPTelnet / ESPTelnetStream — OLD
telnet.onConnect([](String ip) {
  Serial.println("Connected: " + ip);
});

telnet.onInputReceived([](String line) {
  if (line == "reboot") ESP.restart();
});
```

SimpleTelnet callbacks use `const char*` to avoid heap fragmentation:

```cpp
// SimpleTelnet — NEW
telnet.onConnect([](const char* ip) {
  Serial.print("Connected: ");
  Serial.println(ip);
});

telnet.onInputReceived([](const char* line) {
  if (strcmp(line, "reboot") == 0) ESP.restart();
});
```

**Migration checklist:**
- Change callback parameter type from `String` to `const char*`.
- Replace `==` string comparisons with `strcmp()` or `strcasecmp()`.
- Replace `+` string concatenation with `snprintf()` or `strlcat()`.
- Do not store the pointer past the callback scope — copy with `strlcpy()` if needed.

---

## Platform Notes

### ESP8266-only behaviour

| Feature | Detail |
|---------|--------|
| `printf_P(PGM_P fmt, ...)` | Only compiled on ESP8266. Uses `vsnprintf_P` internally so the format string can live in flash (PSTR / F()). |
| Keep-alive check | Uses `client.status() == 4` (lwIP `ESTABLISHED` state). This is the only reliable way to detect dead connections on ESP8266 without a round-trip. |
| `flush()` | Calls `client.flush()` which on ESP8266 blocks until the TCP send buffer drains or a stack-defined timeout is reached. |

### ESP32 behaviour

| Feature | Detail |
|---------|--------|
| Keep-alive check | Uses `client.connected()`. |
| `flush()` | Non-blocking on most ESP32 SDK versions. |
| `printf_P` | Not available. Use `printf()` — on ESP32 `PSTR()` is a no-op and RAM is not as constrained. |

### Both platforms

- The constructor does not start the server. Call `begin()` after WiFi is up (or `begin(false)` if you need to start before WiFi).
- `loop()` must be called from the Arduino `loop()` function or from a cooperative task. It is not interrupt-safe.
- `write()` broadcasts synchronously — if a client is slow, it will block the loop briefly until `SIMPLETELNET_MAX_WRITE_ERRORS` consecutive failures evict it.

---

## Migration Guide

### From TelnetStream

TelnetStream (`TelnetStreamClass`) is a single-client streaming telnet server. SimpleTelnet is a
drop-in superset.

| TelnetStream | SimpleTelnet equivalent | Notes |
|-------------|------------------------|-------|
| `TelnetStreamClass TelnetStream(23)` | `SimpleTelnet<1> telnet(23)` | Template default is 1 client |
| `TelnetStream.begin()` | `telnet.begin()` | Same signature |
| `TelnetStream.begin(port)` | Constructor takes port; call `telnet.begin()` | Port moved to constructor |
| `TelnetStream.stop()` | `telnet.stop()` | Same |
| `TelnetStream.available()` | `telnet.available()` | Same |
| `TelnetStream.read()` | `telnet.read()` | Same |
| `TelnetStream.peek()` | `telnet.peek()` | Same |
| `TelnetStream.write(byte)` | `telnet.write(byte)` | Same |
| `TelnetStream.write(buf, len)` | `telnet.write(buf, len)` | Same |
| `TelnetStream.flush()` | `telnet.flush()` | Same; ESP8266: may block briefly |
| `TelnetStream.print(...)` | `telnet.print(...)` | Inherited from Print |
| `TelnetStream.println(...)` | `telnet.println(...)` | Inherited from Print |
| *(no equivalent)* | `telnet.loop()` | **Must add** — TelnetStream had no loop() |
| *(no equivalent)* | `telnet.onConnect(cb)` | Optional callback |
| *(no equivalent)* | `telnet.onDisconnect(cb)` | Optional callback |

**Minimal migration:**

```cpp
// Before
#include <TelnetStream.h>
TelnetStream.begin(23);
// in loop(): nothing required

// After
#include <SimpleTelnet.h>
SimpleTelnet<1> telnet(23);
telnet.begin();
// in loop():
telnet.loop();
```

---

### From ESPTelnet / ESPTelnetStream

ESPTelnet is a single-client CLI telnet server. ESPTelnetStream is its streaming variant.
SimpleTelnet covers both.

| ESPTelnet / ESPTelnetStream | SimpleTelnet equivalent | Notes |
|-----------------------------|------------------------|-------|
| `ESPTelnet telnet` | `SimpleTelnet<1> telnet(23)` | Port moved to constructor |
| `telnet.begin(port)` | `SimpleTelnet<1> telnet(port); telnet.begin()` | Port is constructor arg |
| `telnet.begin(port, checkWiFi)` | `SimpleTelnet<1> telnet(port); telnet.begin(checkWiFi)` | checkConnection param preserved |
| `telnet.stop()` | `telnet.stop()` | Same |
| `telnet.loop()` | `telnet.loop()` | Same |
| `telnet.isConnected()` | `telnet.isConnected()` | Same |
| `telnet.disconnectClient()` | `telnet.disconnectClient()` | Same |
| `telnet.getIP()` | `telnet.getIP()` | Returns `const char*` instead of `String` |
| `telnet.getLastAttemptIP()` | `telnet.getLastAttemptIP()` | Returns `const char*` instead of `String` |
| `telnet.setKeepAliveInterval(ms)` | `telnet.setKeepAliveInterval(ms)` | Same |
| `telnet.getKeepAliveInterval()` | `telnet.getKeepAliveInterval()` | Same |
| `telnet.onConnect(f)` | `telnet.onConnect(f)` | **BREAKING**: `f` now takes `const char*` not `String` |
| `telnet.onDisconnect(f)` | `telnet.onDisconnect(f)` | **BREAKING**: `const char*` not `String` |
| `telnet.onReconnect(f)` | `telnet.onReconnect(f)` | **BREAKING**: `const char*` not `String` |
| `telnet.onConnectionAttempt(f)` | `telnet.onConnectionAttempt(f)` | **BREAKING**: `const char*` not `String` |
| `telnet.onInputReceived(f)` | `telnet.onInputReceived(f)` + `telnet.setLineMode(true)` | **BREAKING**: `const char*` not `String`; also call `setLineMode(true)` |
| `telnet.flush()` | `telnet.flush()` | Same |
| `telnet.write(...)` | `telnet.write(...)` | Same |
| `telnet.setLineMode(bool)` | `telnet.setLineMode(bool)` | Same |
| `telnet.setNewlineCharacter(c)` | `telnet.setNewlineChar(c)` | Renamed — shorter |
| *(no equivalent)* | `telnet.connectedCount()` | New: count of active clients |
| *(no equivalent)* | `telnet.disconnectClient(index)` | New: disconnect by slot |
| *(no equivalent)* | `telnet.printf(fmt, ...)` | New: printf to all clients |
| *(no equivalent)* | `telnet.printf_P(fmt, ...)` | New: PROGMEM printf (ESP8266) |
| *(no equivalent)* | `SimpleTelnet<N>` template | New: N simultaneous clients |

> **Note on `setNewlineCharacter`:** ESPTelnet used `setNewlineCharacter(char c)`. SimpleTelnet
> uses the shorter `setNewlineChar(char c)`. Functionality is identical.

**Full migration example:**

```cpp
// Before — ESPTelnet
#include <ESPTelnet.h>
ESPTelnet telnet;

telnet.begin(23);
telnet.onConnect([](String ip) {
  Serial.println("Connected: " + ip);
});
telnet.onInputReceived([](String line) {
  if (line == "help") telnet.println("Commands: help, reboot");
});

// After — SimpleTelnet
#include <SimpleTelnet.h>
SimpleTelnet<1> telnet(23);

telnet.setLineMode(true);
telnet.begin();
telnet.onConnect([](const char* ip) {
  Serial.print("Connected: ");
  Serial.println(ip);
});
telnet.onInputReceived([](const char* line) {
  if (strcmp(line, "help") == 0) telnet.println("Commands: help, reboot");
});
```
