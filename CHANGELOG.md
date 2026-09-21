# Changelog

All notable changes to SimpleTelnet will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
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
