/*
 * SimpleTelnetCore — transport-agnostic protocol core for SimpleTelnet
 *
 * Holds everything that is independent of the network transport:
 *   - line / CR-LF parsing and char dispatch
 *   - mode + newline state
 *   - connection/session metadata (active slots, per-slot IP, counts)
 *   - callbacks and their dispatch
 *   - IP formatting and printf / printf_P helpers
 *
 * It contains NO transport code — no WiFiServer, no WiFiClient, no AsyncClient.
 * Bytes are pushed in by the transport via _feedChar() / _feed(); output goes
 * out through the virtual Arduino Stream write() implemented by the transport.
 *
 * This lets one protocol implementation be shared between:
 *   - SimpleTelnet        (synchronous WiFiServer/WiFiClient — ESP8266 + ESP32)
 *   - AsyncSimpleTelnet   (event-driven AsyncTCP — ESP32, separate library)
 *
 * CANONICAL SOURCE: this file lives in the SimpleTelnet repository. The async
 * fork vendors a copy of this exact file — keep the two in sync.
 *
 * Copyright (c) 2026 Robert van den Breemen
 * MIT License — see LICENSE
 */

#pragma once
#ifndef SimpleTelnetCore_h
#define SimpleTelnetCore_h

// -------------------------------------------------------------------------
// Platform guard — ESP8266 or ESP32 required
// -------------------------------------------------------------------------
#if !defined(ARDUINO_ARCH_ESP8266) && !defined(ARDUINO_ARCH_ESP32)
  #error "SimpleTelnet requires ESP8266 or ESP32"
#endif

#include <Arduino.h>
#if defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
#elif defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
#endif

// -------------------------------------------------------------------------
// Compile-time tunables (override with #define before #include)
// -------------------------------------------------------------------------
#ifndef SIMPLETELNET_LINE_BUF_LEN
  #define SIMPLETELNET_LINE_BUF_LEN   128
#endif
#ifndef SIMPLETELNET_IP_LEN
  #define SIMPLETELNET_IP_LEN          16
#endif
#ifndef SIMPLETELNET_KEEPALIVE_MS
  #define SIMPLETELNET_KEEPALIVE_MS   1000
#endif
// printf()/printf_P() stack buffer size. Output longer than this falls back
// to malloc+free per call, which fragments the heap on tight-RAM targets
// (ESP8266). Bumped from the original 64 to 256 because typical application
// debug lines (OT frame logs, timestamped status messages, heap traces) are
// commonly 80-150 bytes and were triggering the fallback on every call.
// Users can override this with a #define before the #include if RAM is
// severely constrained; the buffer lives on the stack only during the call.
#ifndef SIMPLETELNET_PRINTF_STACK_LEN
  #define SIMPLETELNET_PRINTF_STACK_LEN 256
#endif

// -------------------------------------------------------------------------
// Callback type — const char*, never String
// @note Breaking change vs ESPTelnet: callbacks used to receive String, now
//       receive const char*. Update any on_connect / on_input handlers.
// -------------------------------------------------------------------------
typedef void (*SimpleTelnetCallback)(const char* str);

// -------------------------------------------------------------------------
// SimpleTelnetCore<MAX_CLIENTS>
//
// Abstract base: a transport must derive from it and implement the Arduino
// Stream methods (write/read/peek/available/flush). The transport feeds
// received bytes into the parser via _feedChar() / _feed().
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS = 1>
class SimpleTelnetCore : public Stream {

 public:
  explicit SimpleTelnetCore(uint16_t port = 23);

  // -----------------------------------------------------------------------
  // Mode selection
  // -----------------------------------------------------------------------

  /**
   * @brief Switch to CLI/line-input mode (default: streaming).
   *
   * In CLI mode input is accumulated until newline, then onInputReceived fires
   * with the complete line. In streaming mode every received byte stays
   * available via read() / available().
   *
   * @param enable true = line mode, false = streaming mode (default false).
   */
  void setLineMode(bool enable = true);

  /** @brief Returns true if line-input (CLI) mode is active. */
  bool isLineMode() const;

  /**
   * @brief Set the character that terminates a line in CLI mode.
   * @param c Line terminator character (default '\n').
   */
  void setNewlineChar(char c);

  // -----------------------------------------------------------------------
  // Connection state
  // -----------------------------------------------------------------------

  /** @brief Returns the number of currently active clients. */
  uint8_t connectedCount() const;

  /** @brief Returns true if at least one client is connected. */
  bool isConnected() const;

  // -----------------------------------------------------------------------
  // IP helpers
  // -----------------------------------------------------------------------

  /**
   * @brief IP address of the client in slot idx.
   * @param idx Slot index (default 0).
   * @return Null-terminated dotted-decimal IP string, e.g. "192.168.1.5".
   */
  const char* clientIP(uint8_t idx = 0) const;

  /**
   * @brief ESPTelnet-compatible alias: IP of the first active client (or slot 0).
   */
  const char* getIP() const;

  /** @brief IP of the last rejected / attempted connection. */
  const char* getLastAttemptIP() const;

  // -----------------------------------------------------------------------
  // Keep-alive configuration
  // -----------------------------------------------------------------------

  /**
   * @brief Set the keep-alive check interval.
   * @param ms Interval in milliseconds (default SIMPLETELNET_KEEPALIVE_MS = 1000).
   */
  void setKeepAliveInterval(uint16_t ms);

  /** @brief Returns the current keep-alive check interval in milliseconds. */
  uint16_t getKeepAliveInterval() const;

  // -----------------------------------------------------------------------
  // Callbacks (ESPTelnet compatibility — all use const char*)
  // -----------------------------------------------------------------------

  /** @brief Fired when a new client connects (str = client IP). */
  void onConnect(SimpleTelnetCallback f);
  /** @brief Fired when a client disconnects (str = client IP). */
  void onDisconnect(SimpleTelnetCallback f);
  /** @brief Fired when the same IP reconnects, MAX_CLIENTS==1 (str = client IP). */
  void onReconnect(SimpleTelnetCallback f);
  /** @brief Fired when a connection is rejected, slots full (str = rejected IP). */
  void onConnectionAttempt(SimpleTelnetCallback f);
  /**
   * @brief Fired on received input.
   * Line mode: complete null-terminated line (no newline).
   * Char mode: single-character null-terminated string.
   */
  void onInputReceived(SimpleTelnetCallback f);

  // -----------------------------------------------------------------------
  // printf helpers (ESPTelnet compatibility) — emit via the virtual write()
  // -----------------------------------------------------------------------

  /**
   * @brief printf to all connected clients.
   *
   * Uses a SIMPLETELNET_PRINTF_STACK_LEN stack buffer; falls back to malloc
   * for longer output. Broadcasts the formatted result via write().
   */
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#if defined(ARDUINO_ARCH_ESP8266)
  /**
   * @brief printf_P — PROGMEM format string (ESP8266 only).
   * @param fmt PROGMEM format string — use PSTR() or F() macro.
   */
  size_t printf_P(PGM_P fmt, ...);
#endif

 protected:
  // -----------------------------------------------------------------------
  // Shared session state (read/written by the transport too)
  // -----------------------------------------------------------------------
  bool        _clientActive[MAX_CLIENTS];            // slot in use?
  char        _ip[MAX_CLIENTS][SIMPLETELNET_IP_LEN]; // IP per slot
  char        _attemptIp[SIMPLETELNET_IP_LEN];       // last rejected IP
  uint8_t     _connectedCount;
  uint16_t    _port;
  uint16_t    _keepAliveInterval;

  // -----------------------------------------------------------------------
  // Mode + line-parsing state. Single shared input buffer — saves
  // (MAX_CLIENTS-1)*SIMPLETELNET_LINE_BUF_LEN. Only used when _onInput != 0.
  // -----------------------------------------------------------------------
  bool        _lineMode;
  char        _newlineChar;
  bool        _lastWasCR;
  uint8_t     _inputLen;
  char        _inputBuf[SIMPLETELNET_LINE_BUF_LEN];

  // -----------------------------------------------------------------------
  // Callbacks
  // -----------------------------------------------------------------------
  SimpleTelnetCallback _onConnect;
  SimpleTelnetCallback _onDisconnect;
  SimpleTelnetCallback _onReconnect;
  SimpleTelnetCallback _onConnectionAttempt;
  SimpleTelnetCallback _onInput;

  // -----------------------------------------------------------------------
  // Protocol helpers — the transport pushes received bytes in here
  // -----------------------------------------------------------------------

  /** @brief Dispatch one received byte to the active input handler. */
  void _feedChar(char c);

  /** @brief Dispatch a buffer of received bytes (calls _feedChar per byte). */
  void _feed(const uint8_t* buf, size_t len);

  /**
   * @brief Process one character in line (CLI) mode.
   * Handles CR, LF, CR+LF, backspace (0x08/0x7F), bell (0x07 — ignored),
   * high bytes >= 0x80 (ignored, covers telnet IAC 0xFF), printable ASCII.
   */
  void _handleLineInput(char c);

  /** @brief Process one character in char mode: fire _onInput with a 1-char string. */
  void _handleCharInput(char c);

  /** @brief Returns true if at least one slot is currently active. */
  bool _hasActiveClient() const;

  /**
   * @brief Write IP address of addr into buf without using String/toString().
   * @param addr IPAddress to format.
   * @param buf  Output buffer (at least SIMPLETELNET_IP_LEN bytes).
   * @param len  Buffer size.
   */
  static void _extractIP(IPAddress addr, char* buf, size_t len);
};

// =========================================================================
// Inline template implementation (header-only so the compiler sees it)
// =========================================================================

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
SimpleTelnetCore<MAX_CLIENTS>::SimpleTelnetCore(uint16_t port)
  : _connectedCount(0)
  , _port(port)
  , _keepAliveInterval(SIMPLETELNET_KEEPALIVE_MS)
  , _lineMode(false)
  , _newlineChar('\n')
  , _lastWasCR(false)
  , _inputLen(0)
  , _onConnect(nullptr)
  , _onDisconnect(nullptr)
  , _onReconnect(nullptr)
  , _onConnectionAttempt(nullptr)
  , _onInput(nullptr)
{
  memset(_clientActive, false, sizeof(_clientActive));
  memset(_ip, 0, sizeof(_ip));
  memset(_attemptIp, 0, sizeof(_attemptIp));
  memset(_inputBuf, 0, sizeof(_inputBuf));
}

// -------------------------------------------------------------------------
// Mode selection
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::setLineMode(bool enable) {
  _lineMode = enable;
}

template<uint8_t MAX_CLIENTS>
bool SimpleTelnetCore<MAX_CLIENTS>::isLineMode() const {
  return _lineMode;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::setNewlineChar(char c) {
  _newlineChar = c;
}

// -------------------------------------------------------------------------
// Connection state
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
uint8_t SimpleTelnetCore<MAX_CLIENTS>::connectedCount() const {
  return _connectedCount;
}

template<uint8_t MAX_CLIENTS>
bool SimpleTelnetCore<MAX_CLIENTS>::isConnected() const {
  return _connectedCount > 0;
}

// -------------------------------------------------------------------------
// IP helpers
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
const char* SimpleTelnetCore<MAX_CLIENTS>::clientIP(uint8_t idx) const {
  if (idx < MAX_CLIENTS) return _ip[idx];
  return _ip[0];
}

template<uint8_t MAX_CLIENTS>
const char* SimpleTelnetCore<MAX_CLIENTS>::getIP() const {
  // ESPTelnet compatibility alias: return IP of first active slot, or slot 0
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) return _ip[i];
  }
  return _ip[0];
}

template<uint8_t MAX_CLIENTS>
const char* SimpleTelnetCore<MAX_CLIENTS>::getLastAttemptIP() const {
  return _attemptIp;
}

// -------------------------------------------------------------------------
// Keep-alive configuration
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::setKeepAliveInterval(uint16_t ms) {
  _keepAliveInterval = ms;
}

template<uint8_t MAX_CLIENTS>
uint16_t SimpleTelnetCore<MAX_CLIENTS>::getKeepAliveInterval() const {
  return _keepAliveInterval;
}

// -------------------------------------------------------------------------
// Callbacks
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::onConnect(SimpleTelnetCallback f) {
  _onConnect = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::onDisconnect(SimpleTelnetCallback f) {
  _onDisconnect = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::onReconnect(SimpleTelnetCallback f) {
  _onReconnect = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::onConnectionAttempt(SimpleTelnetCallback f) {
  _onConnectionAttempt = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::onInputReceived(SimpleTelnetCallback f) {
  _onInput = f;
}

// -------------------------------------------------------------------------
// printf helpers — emit through the virtual Stream write()
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
size_t SimpleTelnetCore<MAX_CLIENTS>::printf(const char* fmt, ...) {
  if (!_hasActiveClient()) return 0;

  char loc_buf[SIMPLETELNET_PRINTF_STACK_LEN];
  va_list arg;
  va_start(arg, fmt);
  int len = vsnprintf(loc_buf, sizeof(loc_buf), fmt, arg);
  va_end(arg);

  if (len < 0) return 0;
  if (len < (int)sizeof(loc_buf)) {
    return write((uint8_t*)loc_buf, (size_t)len);
  }

  // Output exceeds stack buffer — fall back to heap.
  char* temp = (char*)malloc((size_t)len + 1);
  if (!temp) return 0;
  // MUST restart va_list — the first one was consumed by vsnprintf above.
  va_start(arg, fmt);
  vsnprintf(temp, (size_t)len + 1, fmt, arg);
  va_end(arg);
  size_t written = write((uint8_t*)temp, (size_t)len);
  free(temp);
  return written;
}

#if defined(ARDUINO_ARCH_ESP8266)
template<uint8_t MAX_CLIENTS>
size_t SimpleTelnetCore<MAX_CLIENTS>::printf_P(PGM_P fmt, ...) {
  if (!_hasActiveClient()) return 0;

  char loc_buf[SIMPLETELNET_PRINTF_STACK_LEN];
  va_list arg;
  va_start(arg, fmt);
  // vsnprintf_P reads the format string from flash (PROGMEM) on ESP8266.
  int len = vsnprintf_P(loc_buf, sizeof(loc_buf), fmt, arg);
  va_end(arg);

  if (len < 0) return 0;
  if (len < (int)sizeof(loc_buf)) {
    return write((uint8_t*)loc_buf, (size_t)len);
  }

  // Output exceeds stack buffer — fall back to heap.
  char* temp = (char*)malloc((size_t)len + 1);
  if (!temp) return 0;
  // MUST restart va_list — the first one was consumed above.
  va_start(arg, fmt);
  vsnprintf_P(temp, (size_t)len + 1, fmt, arg);
  va_end(arg);
  size_t written = write((uint8_t*)temp, (size_t)len);
  free(temp);
  return written;
}
#endif

// -------------------------------------------------------------------------
// Protocol helpers — input processing
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_feedChar(char c) {
  if (_lineMode) {
    _handleLineInput(c);
  } else {
    _handleCharInput(c);
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_feed(const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) _feedChar((char)buf[i]);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_handleLineInput(char c) {
  // CR/LF handling (precise):
  //   '\r'       : mark _lastWasCR, return — do NOT fire yet; wait to see '\n'
  //   '\n' after '\r': skip (already fired on CR) — clear flag
  //   '\n' alone : fire and reset
  //   anything after pending CR (not '\n'): fire pending line, reset, then
  //                                         process c normally
  if (c == '\r') {
    _lastWasCR = true;
    return;  // hold — may be followed by '\n'
  }
  if (c == '\n') {
    if (_lastWasCR) {
      // CR+LF sequence — fire was deferred; do it now, skip the LF.
      _inputBuf[_inputLen] = '\0';
      _onInput(_inputBuf);
      _inputLen = 0;
      _lastWasCR = false;
    } else {
      // Bare LF — fire and reset.
      _inputBuf[_inputLen] = '\0';
      _onInput(_inputBuf);
      _inputLen = 0;
    }
    return;
  }
  if (_lastWasCR) {
    // Bare CR (not followed by LF) — dispatch pending line, then fall through
    // to process c as normal input in the new line.
    _inputBuf[_inputLen] = '\0';
    _onInput(_inputBuf);
    _inputLen = 0;
    _lastWasCR = false;
    // fall through — process c in the new line
  }

  // Backspace: 0x08 (ASCII BS) or 0x7F (DEL) — remove last char, no echo.
  if (c == '\x08' || c == '\x7F') {
    if (_inputLen > 0) _inputLen--;
    return;
  }

  // Bell (0x07) — ignore.
  if (c == '\x07') return;

  // High bytes >= 0x80 — ignore. This covers telnet IAC (0xFF) and any
  // multi-byte telnet negotiation sequences, preventing buffer corruption.
  if ((uint8_t)c >= 0x80) return;

  // Printable ASCII (0x20..0x7E) — append, silently truncate if full.
  if (c >= '\x20' && c < '\x7F') {
    if (_inputLen < SIMPLETELNET_LINE_BUF_LEN - 1) {
      _inputBuf[_inputLen++] = c;
    }
    // else: silent truncation — no crash, no error.
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_handleCharInput(char c) {
  // Char mode: dispatch every byte immediately as a single-char string.
  // Consistent with ESPTelnet char mode — all bytes including control chars
  // are passed through. Using a local char[2] avoids any String/heap alloc.
  char buf[2] = {c, '\0'};
  _onInput(buf);
}

// -------------------------------------------------------------------------
// Protocol helpers — utilities
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
bool SimpleTelnetCore<MAX_CLIENTS>::_hasActiveClient() const {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) return true;
  }
  return false;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_extractIP(IPAddress addr, char* buf, size_t len) {
  // No String, no toString() — plain snprintf with IPAddress operator[].
  // Works identically on ESP8266 and ESP32.
  snprintf(buf, len, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
}

#endif // SimpleTelnetCore_h
