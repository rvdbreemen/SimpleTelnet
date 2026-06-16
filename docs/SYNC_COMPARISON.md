# SimpleTelnet (sync) vs the popular ESP telnet libraries

How the synchronous `SimpleTelnet` compares to the three most popular telnet
libraries for ESP8266/ESP32, on functionality and implementation patterns.

## The three most popular

| # | Library | ★ (approx) | Transport | Role |
|---|---------|-----------|-----------|------|
| 1 | [LennartHennigs/ESPTelnet](https://github.com/LennartHennigs/ESPTelnet) | ~257 | `WiFiServer` (sync) | Callback/CLI telnet for debugging |
| 2 | [jandrassy/TelnetStream](https://github.com/jandrassy/TelnetStream) | ~150 | `WiFiServer` (sync) | `Stream` telnet for `print()` debug output |
| 3 | [yasheena/TelnetSpy](https://github.com/yasheena/telnetspy) | ~60 | `WiFiServer` (sync) | Mirrors `Serial` over telnet ("debug over the air") |

(ESPTelnet also ships `ESPTelnetStream`, a `Stream`+callbacks hybrid; still single-client.)

## Feature comparison

| Feature | ESPTelnet | TelnetStream | TelnetSpy | **SimpleTelnet (sync)** |
|---|---|---|---|---|
| Multi-client | ❌ single | broadcast only | ❌ single | ✅ `SimpleTelnet<N>` (compile-time) |
| Streaming (`Stream`) mode | hybrid only | ✅ | ✅ (Serial mirror) | ✅ |
| CLI / line + char input | ✅ | ❌ (poll only) | partial | ✅ (line **and** char) |
| Callbacks | ✅ | ❌ | ✅ | ✅ |
| **On-connect message** | ✅ (String) | ❌ | ✅ | ✅ **`onConnect(const char* ip)`** |
| Callback / message type | `String` | n/a | `const char*` + `String` | **`const char*` only** |
| No `String` anywhere | ❌ | ✅ | ❌ | ✅ |
| No hidden global | ✅ | ❌ (global instance) | ✅ | ✅ |
| Compile-time client cap (template) | ❌ | ❌ | ❌ | ✅ **`<N>` sizes RAM** |
| RFC 854 negotiation | partial | minimal | partial (NVT cmds) | ✅ refuse + ECHO/SGA + escaping |
| Steady-state heap allocation | per keystroke (String) | none | 3000 B TX ring + buffers | **none** |
| `printf` / `printf_P` | ❌ / ❌ | ❌ / ❌ | `printf`-ish | ✅ / ✅ (PROGMEM) |

## Implementation-pattern contrasts (the things this library leans on)

### 1. On-connect message (welcome banner)

`onConnect` hands you the **client IP as `const char*`**, so you can print a
banner the moment a client attaches — no `String`, no allocation:

```cpp
telnet.onConnect([](const char* ip) {
  telnet.printf("Welcome %s — type 'h' for help\r\n", ip);
});
```

- **ESPTelnet** has `onConnect`, but the IP arrives as a `String`.
- **TelnetStream** has no callbacks at all — you cannot greet a new client.
- **TelnetSpy** has a connect callback (it is a Serial mirror, so banners are less
  central to its use case).

### 2. The template approach — you choose the client cap at compile time

The number of simultaneous clients is a **template parameter**, fixed at compile
time, so all per-client state is allocated statically and sized to exactly what
you need:

```cpp
SimpleTelnet<1> cli(23);       // one client  → smallest footprint
SimpleTelnet<4> stream(2323);  // four clients → 4 slots, sized at build time
```

No other popular library does this: ESPTelnet and TelnetSpy are hard single-client;
TelnetStream broadcasts but is not a sized, multi-slot server. With `<N>` you pay
for exactly the slots you ask for — no runtime `new`, no over-allocation.

### 3. Memory-use minimization is the design goal, not an afterthought

SimpleTelnet was born from a **heap regression** (three `WiFiServer` instances in
OTGW firmware pushed free heap below the safe line). Every design choice follows
from "spend as little RAM as possible, deterministically":

- **No `String` anywhere** → zero heap churn per keystroke/callback (ESPTelnet
  allocates a `String` on every input event; TelnetSpy keeps a 3000-byte TX ring).
- **One shared CLI input buffer** across all client slots, not one per slot →
  `MAX_CLIENTS × LINE_BUF` becomes just `LINE_BUF`.
- **Static, compile-time slots** (the `<N>` template) → no dynamic allocation, no
  fragmentation.
- **No hidden global** (unlike TelnetStream) → you pay only for instances you
  create.
- **Tunable buffers** via `#define` (`SIMPLETELNET_LINE_BUF_LEN`,
  `SIMPLETELNET_IP_LEN`, `SIMPLETELNET_PRINTF_STACK_LEN`).
- Result: ≈ **489 B** for `SimpleTelnet<1>`, ≈ **1033 B** for `SimpleTelnet<4>`
  (the object itself; lwIP socket pools are the same for any library).

## Differences & trade-offs — summary

- **vs ESPTelnet:** SimpleTelnet adds multi-client, `const char*` callbacks (no
  per-keystroke heap), streaming + CLI in one class, and RFC 854 negotiation.
  ESPTelnet wins on maturity and ecosystem presence.
- **vs TelnetStream:** SimpleTelnet adds callbacks (incl. on-connect), CLI input,
  multi-client sizing and no hidden global. TelnetStream is the leaner choice if
  you literally only need `print()` to a broadcast `Stream`.
- **vs TelnetSpy:** SimpleTelnet is a general multi-client server with a tiny,
  deterministic footprint; TelnetSpy is a specialised single-client Serial mirror
  with a large TX ring (great when you want buffered "debug over the air", heavier
  on RAM).
- **The trade-off of `<N>`:** the client cap is fixed at compile time (no runtime
  resize) — which is exactly what makes the footprint deterministic.
