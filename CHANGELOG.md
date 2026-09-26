# Changelog

All notable changes to SimpleTelnet will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `availableFrom(idx)`, `readFrom(idx)` and `isSlotActive(idx)`: read one
  specific client slot. `available()`/`read()` serve whichever slot has data,
  so with several writers the caller cannot tell whose byte it got. Reading
  per slot lets a bidirectional bridge let one client write at a time while
  the others' bytes wait untouched in their own TCP buffer.

### Changed
- A reconnect from the address of a current occupant now takes over that
  slot for any `MAX_CLIENTS`, not only for one slot. It still applies only when
  every slot is taken, so two clients from one address each keep a slot.

### Fixed
- `write()` no longer discards the tail of a short write while reporting the
  full count. On ESP8266 a partial write is the normal result once the lwIP
  send buffer fills, and the unwritten remainder was dropped silently, so a
  caller could not tell a quiet console from a truncated one. It now returns
  the count actually accepted and counts the shortfall per client, readable
  via `txDropped()`/`txDroppedTotal()`.

  There is deliberately no retry. The underlying write already blocks while
  the peer keeps making progress and only returns short after the client's
  timeout passes with none (1000 ms, `setTimeout(_keepAliveInterval)`), so a
  retry after that point either waits another full timeout on a socket that
  just proved it is not draining, or, bounded tighter, never runs. A bounded
  retry was added and then removed for exactly that reason: it was inert.
- `_drainClient()` no longer discards pending inbound bytes. The loop ran at
  accept and at teardown and could not tell a telnet IAC sequence from payload,
  so a client that pipelined a command with `connect()` lost that command and a
  client that wrote then closed lost its last one. It now flushes outbound only.
  Streaming consumers (`_onInput == nullptr`) were the ones losing data, because
  `_processInput()` never runs for them. Backport of upstream `a909731` onto the
  1.x maintenance line.

## [1.0.0] - 2026-04-12

### Added
- Initial release
- Multi-client support via `SimpleTelnet<MAX_CLIENTS>` template
- Two operating modes: streaming (TelnetStream-compatible) and CLI/line-input (ESPTelnet-compatible)
- Callbacks with `const char*` — no `String` objects, no heap fragmentation
- ESP8266 and ESP32 support with platform-specific guards
- `printf()` and `printf_P()` (ESP8266 PROGMEM-safe) helpers
- `begin(false)` for unconditional server bind (before WiFi is up)
- Single shared input buffer (saves RAM for multi-client streaming instances)
- `millis()`-based keep-alive (no project-specific macro dependencies)
- Full Arduino `Stream` + `Print` inheritance
- Three example sketches: StreamingMode, CLIMode, DualInstance
- Arduino Library Manager and PlatformIO registry metadata
