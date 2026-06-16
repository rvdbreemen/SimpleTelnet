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
  // Telnet option negotiation (RFC 854 / 855)
  // -----------------------------------------------------------------------

  /**
   * @brief Telnet IAC negotiation behaviour.
   *  - NEG_OFF       : raw passthrough — IAC bytes are NOT interpreted (legacy).
   *  - NEG_REFUSE    : parse + strip IAC, politely refuse every option
   *                    (DO->WONT, WILL->DONT). Default.
   *  - NEG_CHAR_ECHO : NEG_REFUSE plus proactively negotiate ECHO + SGA on
   *                    connect for character-at-a-time terminals.
   */
  enum TelnetNegotiation { NEG_OFF = 0, NEG_REFUSE = 1, NEG_CHAR_ECHO = 2 };

  /** @brief Select telnet negotiation behaviour (default NEG_REFUSE). */
  void setTelnetNegotiation(TelnetNegotiation mode);
  /** @brief Returns the current telnet negotiation behaviour. */
  TelnetNegotiation getTelnetNegotiation() const;

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
  // Telnet (RFC 854) negotiation state
  // -----------------------------------------------------------------------
  // Command codes (RFC 854).
  enum { TC_SE = 240, TC_NOP = 241, TC_DM = 242, TC_BRK = 243, TC_IP = 244,
         TC_AO = 245, TC_AYT = 246, TC_EC = 247, TC_EL = 248, TC_GA = 249,
         TC_SB = 250, TC_WILL = 251, TC_WONT = 252, TC_DO = 253, TC_DONT = 254,
         TC_IAC = 255 };
  // Options we understand (RFC 857 / 858); everything else is refused.
  enum { OPT_ECHO = 1, OPT_SGA = 3 };
  // IAC parser states.
  enum { S_DATA = 0, S_IAC, S_WILL, S_WONT, S_DO, S_DONT, S_SB, S_SB_IAC };
  // Per-option "us" negotiation state (scoped RFC 1143 Q method).
  enum { US_NO = 0, US_WANTYES = 1, US_YES = 2 };

  TelnetNegotiation _negMode;
  uint8_t  _iacState[MAX_CLIENTS];   // S_* parser state, persists across reads
  uint8_t  _optFlags[MAX_CLIENTS];   // bits 0-1: ECHO us-state, bits 2-3: SGA us-state
  int16_t  _peeked[MAX_CLIENTS];     // sync pull-path 1-byte lookahead (-1 = none)

  // -----------------------------------------------------------------------
  // Protocol helpers — the transport pushes received bytes in here
  // -----------------------------------------------------------------------

  /**
   * @brief Feed received bytes for client idx through the IAC filter, then the
   * line/char input handlers (callback path). Negotiation is stripped and
   * answered; only NVT data reaches the handlers.
   */
  void _feed(uint8_t idx, const uint8_t* buf, size_t len);

  /**
   * @brief Run one received byte through the telnet IAC state machine.
   * @return the NVT data byte (0..255) to surface, or -1 if the byte was
   *         consumed by negotiation. Emits replies via _sendToClient().
   *         In NEG_OFF mode the byte is returned unchanged.
   */
  int _filterByte(uint8_t idx, uint8_t b);

  /** @brief Reset per-client negotiation state (call on connect/disconnect). */
  void _resetNegotiation(uint8_t idx);

  /** @brief Proactively negotiate ECHO+SGA on connect (NEG_CHAR_ECHO only). */
  void _startNegotiation(uint8_t idx);

  /**
   * @brief Transport hook: send raw bytes to a single client (NOT escaped).
   * Used for negotiation replies. Sync: client.write(); async: TX ring.
   */
  virtual void _sendToClient(uint8_t idx, const uint8_t* buf, size_t len) = 0;

  // Negotiation internals (scoped RFC 1143 Q method for ECHO/SGA).
  uint8_t _usState(uint8_t idx, uint8_t opt) const;
  void    _setUs(uint8_t idx, uint8_t opt, uint8_t st);
  void    _negReply(uint8_t idx, uint8_t cmd, uint8_t opt);
  void    _negOnDo(uint8_t idx, uint8_t opt);
  void    _negOnDont(uint8_t idx, uint8_t opt);
  void    _negOnWill(uint8_t idx, uint8_t opt);
  void    _negOnWont(uint8_t idx, uint8_t opt);
  void    _maybeEcho(uint8_t idx, uint8_t d);

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
  , _negMode(NEG_REFUSE)
{
  memset(_clientActive, false, sizeof(_clientActive));
  memset(_ip, 0, sizeof(_ip));
  memset(_attemptIp, 0, sizeof(_attemptIp));
  memset(_inputBuf, 0, sizeof(_inputBuf));
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    _iacState[i] = S_DATA;
    _optFlags[i] = 0;
    _peeked[i]   = -1;
  }
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
void SimpleTelnetCore<MAX_CLIENTS>::_feed(uint8_t idx, const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    int d = _filterByte(idx, buf[i]);
    if (d < 0) continue;                 // consumed by telnet negotiation
    if (_lineMode) _handleLineInput((char)d);
    else           _handleCharInput((char)d);
  }
}

// -------------------------------------------------------------------------
// Telnet (RFC 854) IAC state machine + scoped RFC 1143 (ECHO/SGA) negotiation
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
int SimpleTelnetCore<MAX_CLIENTS>::_filterByte(uint8_t idx, uint8_t b) {
  if (_negMode == NEG_OFF) return b;     // raw passthrough (legacy)

  switch (_iacState[idx]) {
    case S_DATA:
      if (b == TC_IAC) { _iacState[idx] = S_IAC; return -1; }
      _maybeEcho(idx, b);
      return b;

    case S_IAC:
      switch (b) {
        case TC_IAC:  _iacState[idx] = S_DATA; _maybeEcho(idx, 0xFF); return 0xFF; // escaped data 255
        case TC_WILL: _iacState[idx] = S_WILL; return -1;
        case TC_WONT: _iacState[idx] = S_WONT; return -1;
        case TC_DO:   _iacState[idx] = S_DO;   return -1;
        case TC_DONT: _iacState[idx] = S_DONT; return -1;
        case TC_SB:   _iacState[idx] = S_SB;   return -1;
        case TC_AYT: {                          // SHOULD: visible liveness reply
          static const uint8_t ayt[] = { '\r', '\n', '[', 't', 'e', 'l', 'n',
                                         'e', 't', ']', ' ', 'y', 'e', 's',
                                         '\r', '\n' };
          _sendToClient(idx, ayt, sizeof(ayt));
          _iacState[idx] = S_DATA; return -1;
        }
        case TC_EC:                              // SHOULD: erase last char (line mode)
          if (_lineMode && _inputLen > 0) _inputLen--;
          _iacState[idx] = S_DATA; return -1;
        case TC_EL:                              // SHOULD: erase current line
          if (_lineMode) { _inputLen = 0; _lastWasCR = false; }
          _iacState[idx] = S_DATA; return -1;
        default:                                 // NOP/DM/BRK/IP/AO/GA/... : ignore
          _iacState[idx] = S_DATA; return -1;
      }

    case S_WILL: _negOnWill(idx, b); _iacState[idx] = S_DATA; return -1;
    case S_WONT: _negOnWont(idx, b); _iacState[idx] = S_DATA; return -1;
    case S_DO:   _negOnDo(idx, b);   _iacState[idx] = S_DATA; return -1;
    case S_DONT: _negOnDont(idx, b); _iacState[idx] = S_DATA; return -1;

    case S_SB:                                   // skip subnegotiation payload
      if (b == TC_IAC) _iacState[idx] = S_SB_IAC;
      return -1;
    case S_SB_IAC:                               // IAC inside SB: SE ends it, IAC IAC = data
      _iacState[idx] = (b == TC_IAC) ? S_SB : S_DATA;
      return -1;
  }
  _iacState[idx] = S_DATA;
  return -1;
}

template<uint8_t MAX_CLIENTS>
uint8_t SimpleTelnetCore<MAX_CLIENTS>::_usState(uint8_t idx, uint8_t opt) const {
  return (opt == OPT_ECHO) ? (_optFlags[idx] & 0x03)
                           : ((_optFlags[idx] >> 2) & 0x03);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_setUs(uint8_t idx, uint8_t opt, uint8_t st) {
  if (opt == OPT_ECHO) _optFlags[idx] = (_optFlags[idx] & ~0x03) | (st & 0x03);
  else                 _optFlags[idx] = (_optFlags[idx] & ~0x0C) | ((st & 0x03) << 2);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_negReply(uint8_t idx, uint8_t cmd, uint8_t opt) {
  uint8_t r[3] = { (uint8_t)TC_IAC, cmd, opt };
  _sendToClient(idx, r, 3);
}

// Peer asks US to enable <opt>.
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_negOnDo(uint8_t idx, uint8_t opt) {
  bool weSupport = (_negMode == NEG_CHAR_ECHO) && (opt == OPT_ECHO || opt == OPT_SGA);
  if (!weSupport) { _negReply(idx, TC_WONT, opt); return; }   // refuse everything else
  uint8_t st = _usState(idx, opt);
  if (st == US_NO)        { _setUs(idx, opt, US_YES); _negReply(idx, TC_WILL, opt); }
  else if (st == US_WANTYES) { _setUs(idx, opt, US_YES); }     // agreement to our WILL
  // US_YES: already on — ignore (loop guard)
}

// Peer asks US to disable <opt>.
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_negOnDont(uint8_t idx, uint8_t opt) {
  if (opt != OPT_ECHO && opt != OPT_SGA) return;              // we never enabled others
  uint8_t st = _usState(idx, opt);
  if (st == US_YES)        { _setUs(idx, opt, US_NO); _negReply(idx, TC_WONT, opt); }
  else if (st == US_WANTYES) { _setUs(idx, opt, US_NO); }      // peer refused our WILL
  // US_NO: ignore
}

// Peer offers to perform <opt>. We want nothing from the peer -> refuse.
template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_negOnWill(uint8_t idx, uint8_t opt) {
  _negReply(idx, TC_DONT, opt);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_negOnWont(uint8_t idx, uint8_t opt) {
  (void)idx; (void)opt;   // terminal — nothing to do
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_maybeEcho(uint8_t idx, uint8_t d) {
  if (_usState(idx, OPT_ECHO) != US_YES) return;   // only when WE echo (CHAR_ECHO agreed)
  if (_lineMode) {
    if (d == '\r' || d == '\n') { uint8_t crlf[2] = { '\r', '\n' }; _sendToClient(idx, crlf, 2); }
    else if (d == '\x08' || d == '\x7F') { static const uint8_t bs[3] = { '\b', ' ', '\b' }; _sendToClient(idx, bs, 3); }
    else if (d >= 0x20 && d < 0x7F) { _sendToClient(idx, &d, 1); }
    // else: control byte — do not echo
  } else {
    _sendToClient(idx, &d, 1);   // char mode: echo raw byte
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_resetNegotiation(uint8_t idx) {
  _iacState[idx] = S_DATA;
  _optFlags[idx] = 0;
  _peeked[idx]   = -1;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::_startNegotiation(uint8_t idx) {
  if (_negMode != NEG_CHAR_ECHO) return;
  // We offer to ECHO and to SUPPRESS-GO-AHEAD (character-at-a-time terminal).
  _setUs(idx, OPT_ECHO, US_WANTYES); _negReply(idx, TC_WILL, OPT_ECHO);
  _setUs(idx, OPT_SGA,  US_WANTYES); _negReply(idx, TC_WILL, OPT_SGA);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnetCore<MAX_CLIENTS>::setTelnetNegotiation(TelnetNegotiation mode) {
  _negMode = mode;
}

template<uint8_t MAX_CLIENTS>
typename SimpleTelnetCore<MAX_CLIENTS>::TelnetNegotiation
SimpleTelnetCore<MAX_CLIENTS>::getTelnetNegotiation() const {
  return _negMode;
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
