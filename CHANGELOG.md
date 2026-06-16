# Changelog

All notable changes to SimpleTelnet will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Telnet (RFC 854) option negotiation** in the shared core (BL-001/001a/001b).
  `setTelnetNegotiation(NEG_OFF | NEG_REFUSE | NEG_CHAR_ECHO)`:
  - `NEG_REFUSE` (default): parse + strip IAC sequences, politely refuse every
    option (`DO`→`WONT`, `WILL`→`DONT`), skip subnegotiation (`SB…SE`), honour
    `IAC IAC` as a literal `0xFF`, and reply to `AYT`; `EC`/`EL` act as
    backspace / line-clear in line mode.
  - `NEG_CHAR_ECHO`: also negotiates `ECHO` + `SUPPRESS-GO-AHEAD` (scoped
    RFC 1143 Q method) and echoes input for character-at-a-time terminals.
  - `NEG_OFF`: legacy raw passthrough (no interpretation).
  - **Outbound IAC escaping**: a `0xFF` data byte is sent as `IAC IAC`
    (RFC 854) in negotiation-on modes.
  Implemented once in `SimpleTelnetCore`, inherited by both `SimpleTelnet`
  (sync) and `AsyncSimpleTelnet` (async).

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
