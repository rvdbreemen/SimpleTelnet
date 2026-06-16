# SimpleTelnet — Backlog

This is the project's own backlog: planned work, design notes and acceptance
criteria, tracked in-repo so the history travels with the code.

## How to use

- Each item has an ID (`BL-NNN`), a status, a priority and an owner area.
- Keep the **plan** and **acceptance criteria** with the item so it is ready to
  pick up without re-research.
- Move items between the sections below as they progress.

**Status legend:** `TODO` · `IN PROGRESS` · `BLOCKED` · `DONE`
**Priority:** `P1` (high) · `P2` (medium) · `P3` (low)

---

## Open

### BL-001 — Telnet option negotiation (RFC 854 / IAC handling)

- **Status:** TODO
- **Priority:** P2
- **Area:** protocol core (`src/SimpleTelnetCore.h`) + both transports
- **Applies to:** `SimpleTelnet` (sync) and `AsyncSimpleTelnet` (async) — fix
  belongs in the shared core so both inherit it.

#### Context / problem

SimpleTelnet treats telnet as a raw transport. In line mode every byte `>= 0x80`
is dropped (`_handleLineInput`, covers `IAC = 0xFF`); in char mode every byte is
passed through to the callback. Neither path implements **RFC 854** option
negotiation. Two concrete defects follow:

1. **No response to negotiation.** A client sending `IAC DO <opt>` /
   `IAC WILL <opt>` gets no reply. Per RFC 854 a conforming endpoint must answer;
   some clients wait or fall back to a degraded mode.
2. **Data-stream corruption.** Only bytes `>= 0x80` are dropped. The 3-byte
   sequence `IAC DO ECHO` = `255 253 1` drops `255` and `253` but the **option
   code** can be a printable byte. e.g. `IAC DO <opt=0x28>` leaks `'('` into the
   line buffer. Subnegotiation (`IAC SB … IAC SE`) is not skipped at all.

This is the "telnet-as-transport, not full RFC 854" gap noted in the library
comparison (`async/COMPARISON.md`). Peers (ESPTelnet, robdobsn/AsyncTelnetServer,
WiFiTelnetToSerial) have the same or weaker handling, so this also differentiates.

#### Goal

A minimal, RFC-compliant negotiation layer in the core that:
- parses and **strips** all IAC sequences from the data stream so only NVT data
  reaches the line/char handlers and the RX ring;
- **answers** negotiation politely (refuse-by-default);
- optionally enables ECHO + SUPPRESS-GO-AHEAD together for character-at-a-time
  CLI use;
- changes default behaviour as little as possible and stays allocation-free.

#### Relevant codes (RFC 854/855)

| Command | Code | | Option | Code | RFC |
|---|---|---|---|---|---|
| SE  | 240 | | ECHO              | 1  | 857 |
| NOP | 241 | | SUPPRESS-GO-AHEAD | 3  | 858 |
| SB  | 250 | | TERMINAL-TYPE     | 24 | 1091 |
| WILL| 251 | | NAWS (win size)   | 31 | 1073 |
| WONT| 252 | | | | |
| DO  | 253 | | | | |
| DONT| 254 | | | | |
| IAC | 255 | | | | |

#### Negotiation policy (minimal, non-looping)

- `IAC DO  <opt>` → reply `IAC WONT <opt>` for everything **except** the options
  we support (ECHO, SGA when char-mode negotiation is enabled → `IAC WILL <opt>`).
- `IAC WILL <opt>` → reply `IAC DONT <opt>` for everything (we request nothing
  from the client by default).
- `IAC IAC` → deliver a single literal `0xFF` as data.
- `IAC SB … IAC SE` → consume and ignore (no SB options supported).
- Only send a reply when it changes state (track a tiny per-option "sent" flag)
  so we never loop. `WONT`/`DONT` are terminal.
- Optional proactive negotiation on connect (behind a flag): server sends
  `WILL ECHO` + `WILL SGA` for char-at-a-time terminals.

#### Implementation plan

1. **Core state machine.** Add a per-client IAC parser state
   `{ DATA, IAC, OPT_WILL, OPT_WONT, OPT_DO, OPT_DONT, SB, SB_IAC }` plus a few
   bytes of per-client negotiation state. Replace the current `>= 0x80` drop with
   this parser. Only `DATA` bytes flow on to `_handleLineInput` /
   `_handleCharInput` / RX ring.
2. **Thread the client index.** Change `_feed(const uint8_t*, size_t)` →
   `_feed(uint8_t idx, const uint8_t*, size_t)` and `_feedChar(char)` →
   `_feedChar(uint8_t idx, char)` so a reply can be sent to the originating
   client. Update both transports' input paths
   (`SimpleTelnet::_processInput`, `AsyncSimpleTelnet::_onData`).
3. **Per-client write hook.** Add a protected `virtual void _sendToClient(
   uint8_t idx, const uint8_t* buf, size_t len) = 0;` implemented by each
   transport (sync: `_clients[idx].write(...)`; async: push to `_tx[idx]` +
   `_flushTx(idx)`). The core uses it to emit 3-byte negotiation replies.
4. **Public API.** Add `setTelnetNegotiation(mode)` with at least:
   `OFF` (legacy raw passthrough), `REFUSE` (default — strip + refuse), and
   `CHAR_ECHO` (REFUSE + proactively negotiate ECHO/SGA). Document the default
   change (we now strip + answer instead of silently dropping).
5. **Keep it allocation-free.** Fixed-size per-client state, no String, no heap —
   consistent with the library's ethos and the shared-core contract.

#### Acceptance criteria

- `IAC DO <opt>` yields exactly `IAC WONT <opt>` (or `WILL` for ECHO/SGA in
  `CHAR_ECHO`); `IAC WILL <opt>` yields `IAC DONT <opt>`.
- No negotiation byte (command **or** option code, including printable option
  codes) ever reaches the line buffer / char callback / RX ring.
- `IAC IAC` delivers one `0xFF` data byte.
- `IAC SB … IAC SE` is fully consumed and produces no data and no reply.
- No negotiation loop with a standard client (`telnet`, PuTTY raw/telnet mode).
- `negotiation = OFF` reproduces today's raw-passthrough behaviour.
- Fix lives in `SimpleTelnetCore.h`; both `SimpleTelnet` and `AsyncSimpleTelnet`
  inherit it; existing examples still build and behave.

#### Verification

- Extend the host stub harness (`/tmp/sttest`-style) with byte sequences:
  `IAC DO ECHO`, `IAC WILL NAWS`, `IAC DO 0x28` (printable option code),
  `IAC IAC`, `IAC SB TTYPE … IAC SE`. Assert the emitted replies (captured via a
  fake `_sendToClient`) and that the surfaced data is clean.
- Manual: connect with `telnet` and PuTTY (telnet mode); confirm no stray glyphs
  in CLI input and a working prompt; with `CHAR_ECHO`, confirm character-at-a-time.

#### Effort / risk

- **Effort:** ~M (state machine + idx threading + per-transport hook + tests).
- **Risk:** low–medium. Main risks: negotiation loops (mitigated by the
  state-change-only rule) and a subtle behaviour change in char mode (mitigated
  by the `OFF` mode). Touches the shared `_feed` signature → update both
  transports together.

#### References

- [RFC 854 — Telnet Protocol Specification](https://www.rfc-editor.org/rfc/rfc854.html)
- [RFC 855 — Telnet Option Specifications](https://www.rfc-editor.org/rfc/rfc855.html)
- [RFC 857 — Telnet Echo Option](https://datatracker.ietf.org/doc/html/rfc857)
- [RFC 858 — Telnet Suppress Go Ahead Option](https://datatracker.ietf.org/doc/html/rfc858)

---

## Done

_(none yet)_
