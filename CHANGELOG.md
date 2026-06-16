# Changelog

All notable changes to SimpleTelnet will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- Internal refactor (no public API or behaviour change): the transport-agnostic
  protocol logic — line/CR-LF parsing, mode handling, callbacks, IP formatting
  and `printf`/`printf_P` — moved into a new base class `SimpleTelnetCore`
  (`src/SimpleTelnetCore.h`). `SimpleTelnet` now derives from it and contains
  only the synchronous `WiFiServer`/`WiFiClient` transport. This lets the
  parsing/protocol code be shared 1:1 with the planned event-driven async
  ESP32 fork (`AsyncSimpleTelnet`, built on AsyncTCP) so the two cannot drift.

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
