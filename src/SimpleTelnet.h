/*
 * SimpleTelnet — multi-client telnet server for ESP8266 and ESP32
 *
 * Supports two modes per instance:
 *   - Streaming mode : broadcast write, polling read — drop-in for TelnetStream
 *   - CLI mode       : line-buffered input with on_input callback — drop-in for ESPTelnet
 *
 * API superset of TelnetStream, ESPTelnet, and ESPTelnetStream.
 * Callbacks use const char* (never String) to avoid heap fragmentation.
 * No project-specific dependencies — self-contained Arduino library.
 *
 * Copyright (c) 2026 Robert van den Breemen
 * MIT License — see LICENSE
 *
 * Inspired by / shout-out to:
 *   Lennart Hennigs  — ESPTelnet  https://github.com/LennartHennigs/ESPTelnet
 *   Juraj Andrassy   — TelnetStream https://github.com/jandrassy/TelnetStream
 */

#pragma once
#ifndef SimpleTelnet_h
#define SimpleTelnet_h

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
#ifndef SIMPLETELNET_MAX_WRITE_ERRORS
  #define SIMPLETELNET_MAX_WRITE_ERRORS  3
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
// SimpleTelnet<MAX_CLIENTS>
//
// MAX_CLIENTS: maximum simultaneous connections (template parameter).
//   SimpleTelnet<1> debugTelnet(23);          — single-client debug terminal
//   SimpleTelnet<4> otgwStream(OTGW_PORT);    — multi-client OT stream
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS = 1>
class SimpleTelnet : public Stream {

 public:
  // -----------------------------------------------------------------------
  // Construction
  // -----------------------------------------------------------------------

  /**
   * @brief Constructor — port fixed at construction time.
   *
   * TelnetStream compatibility: TelnetStreamClass(port) pattern.
   *
   * @param port TCP port to listen on (default 23).
   */
  explicit SimpleTelnet(uint16_t port = 23);

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------

  /**
   * @brief Start listening on the port given at construction.
   *
   * @param checkWiFi If true (default), returns false when WiFi is not
   *                  connected and the ESP is not in AP mode.
   *                  Pass false to bind unconditionally.
   * @return true if server started successfully, false if WiFi check failed.
   * @note TelnetStream compatibility: begin() / begin(checkWiFi).
   */
  bool begin(bool checkWiFi = true);

  /**
   * @brief Start listening on the given port (ESPTelnet compatibility).
   *
   * Stores the new port, recreates the server, then calls begin(checkWiFi).
   *
   * @param port     TCP port to listen on.
   * @param checkWiFi If true (default), checks WiFi/AP status before binding.
   * @return true if server started successfully.
   * @note ESPTelnet compatibility: begin(port, checkConnection) pattern.
   */
  bool begin(uint16_t port, bool checkWiFi = true);

  /**
   * @brief Stop all clients and the server socket.
   */
  void stop();

  /**
   * @brief Poll for new connections, keep-alive checks, and incoming data.
   *
   * Call from loop(). Internally calls _acceptNewClients(), _checkKeepAlive(),
   * and (when onInputReceived is registered) _processInput().
   *
   * @note ESPTelnet compatibility: loop().
   */
  void loop();

  // -----------------------------------------------------------------------
  // Mode selection
  // -----------------------------------------------------------------------

  /**
   * @brief Switch to CLI/line-input mode (default: streaming).
   *
   * In CLI mode input is accumulated until newline, then onInputReceived fires
   * with the complete line. In streaming mode every received byte stays in the
   * TCP buffer and is accessible via read() / available().
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

  /**
   * @brief Forcibly disconnect all clients (triggers onDisconnect for each).
   */
  void disconnectClient();

  /**
   * @brief Disconnect a specific client by slot index.
   * @param index      Slot index (0..MAX_CLIENTS-1).
   * @param triggerEvent If true (default), fires onDisconnect callback.
   */
  void disconnectClient(uint8_t index, bool triggerEvent = true);

  // -----------------------------------------------------------------------
  // IP helpers
  // -----------------------------------------------------------------------

  /**
   * @brief IP address of the client in slot idx.
   *
   * Canonical multi-client IP accessor. Returns a pointer into an internal
   * char array — valid until the slot is reused.
   *
   * @param idx Slot index (default 0).
   * @return Null-terminated dotted-decimal IP string, e.g. "192.168.1.5".
   */
  const char* clientIP(uint8_t idx = 0) const;

  /**
   * @brief ESPTelnet-compatible alias for clientIP(0).
   *
   * Returns the IP of the most recently / currently connected client.
   *
   * @note ESPTelnet compatibility: getIP() previously returned String; now
   *       returns const char*. Implicit conversion to String is preserved.
   * @return Null-terminated dotted-decimal IP string.
   */
  const char* getIP() const;

  /**
   * @brief IP of the last rejected / attempted connection.
   * @return Null-terminated dotted-decimal IP string (empty if none yet).
   * @note ESPTelnet compatibility: getLastAttemptIP().
   */
  const char* getLastAttemptIP() const;

  // -----------------------------------------------------------------------
  // Keep-alive
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

  /**
   * @brief Register callback fired when a new client connects.
   * @param f Callback receiving the client's IP as const char*.
   * @note Breaking change vs ESPTelnet: parameter is const char*, not String.
   */
  void onConnect(SimpleTelnetCallback f);

  /**
   * @brief Register callback fired when a client disconnects.
   * @param f Callback receiving the client's IP as const char*.
   * @note Breaking change vs ESPTelnet: parameter is const char*, not String.
   */
  void onDisconnect(SimpleTelnetCallback f);

  /**
   * @brief Register callback fired when the same IP reconnects (MAX_CLIENTS==1).
   * @param f Callback receiving the client's IP as const char*.
   * @note Breaking change vs ESPTelnet: parameter is const char*, not String.
   */
  void onReconnect(SimpleTelnetCallback f);

  /**
   * @brief Register callback fired when a connection is rejected (slots full).
   * @param f Callback receiving the rejected client's IP as const char*.
   * @note Breaking change vs ESPTelnet: parameter is const char*, not String.
   */
  void onConnectionAttempt(SimpleTelnetCallback f);

  /**
   * @brief Register callback fired on received input.
   *
   * In line mode: fires with complete null-terminated line (no newline).
   * In char mode: fires with single-character null-terminated string.
   * When not set, _processInput() is skipped entirely — bytes stay in the
   * TCP buffer for polling via read() / available().
   *
   * @param f Callback receiving the input as const char*.
   * @note Breaking change vs ESPTelnet: parameter is const char*, not String.
   */
  void onInputReceived(SimpleTelnetCallback f);

  // -----------------------------------------------------------------------
  // Stream interface (Arduino Stream + Print)
  // -----------------------------------------------------------------------

  /**
   * @brief Write one byte to all connected clients.
   * @param val Byte to send.
   * @return 1 if at least one client received it, 0 if all failed or none connected.
   */
  virtual size_t write(uint8_t val) override;

  /**
   * @brief Write a buffer to all connected clients.
   * @param buf  Pointer to data.
   * @param size Number of bytes.
   * @return size if at least one client received it, 0 if all failed or none connected.
   */
  virtual size_t write(const uint8_t* buf, size_t size) override;

  using Print::write;

  /**
   * @brief Returns the number of bytes available from the first client with data.
   * @return Byte count, or 0 if no data available.
   */
  virtual int available() override;

  /**
   * @brief Read one byte from the first client that has data.
   * @return Byte value (0..255), or -1 if no data.
   */
  virtual int read() override;

  /**
   * @brief Peek at the next byte from the first client that has data.
   * @return Byte value (0..255), or -1 if no data.
   */
  virtual int peek() override;

  /**
   * @brief Flush all connected clients.
   * @note ESP8266: calls client.flush(keepAliveInterval_ms) — returns bool,
   *       failure counts as a write error and may evict the client.
   * @note ESP32: calls client.flush() — void return, no error detection.
   */
  virtual void flush() override;

  // -----------------------------------------------------------------------
  // printf helpers (ESPTelnet compatibility)
  // -----------------------------------------------------------------------

  /**
   * @brief printf to all connected clients.
   *
   * Uses a 64-byte stack buffer; falls back to malloc for longer output.
   * Broadcasts the formatted result via write().
   *
   * @param fmt printf-style format string (RAM).
   * @return Number of bytes written (0 if no clients or format error).
   */
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#if defined(ARDUINO_ARCH_ESP8266)
  /**
   * @brief printf_P — PROGMEM format string (ESP8266 only).
   *
   * Uses vsnprintf_P() so the format string is read from flash,
   * saving RAM. Falls back to malloc for output longer than 64 bytes.
   *
   * @param fmt PROGMEM format string — use PSTR() or F() macro.
   * @return Number of bytes written (0 if no clients or format error).
   * @note ESP8266 only. On ESP32, PROGMEM is a no-op; use printf() directly.
   */
  size_t printf_P(PGM_P fmt, ...);
#endif

 private:
  // -----------------------------------------------------------------------
  // Internal state
  // -----------------------------------------------------------------------

  WiFiServer  _server;                          // listening socket
  WiFiClient  _clients[MAX_CLIENTS];            // active client slots
  bool        _clientActive[MAX_CLIENTS];       // slot in use?
  char        _ip[MAX_CLIENTS][SIMPLETELNET_IP_LEN]; // IP per slot
  char        _attemptIp[SIMPLETELNET_IP_LEN];  // last rejected IP
  uint8_t     _writeErrors[MAX_CLIENTS];        // consecutive write failures

  uint8_t     _connectedCount;
  uint16_t    _port;
  uint16_t    _keepAliveInterval;
  uint32_t    _lastKeepAliveCheck;

  bool        _lineMode;
  char        _newlineChar;

  // Single shared input buffer — saves (MAX_CLIENTS-1)*SIMPLETELNET_LINE_BUF_LEN.
  // Only used when _onInput != nullptr. Streaming mode never touches it.
  bool        _lastWasCR;
  uint8_t     _inputLen;
  char        _inputBuf[SIMPLETELNET_LINE_BUF_LEN];

  // Callbacks
  SimpleTelnetCallback _onConnect;
  SimpleTelnetCallback _onDisconnect;
  SimpleTelnetCallback _onReconnect;
  SimpleTelnetCallback _onConnectionAttempt;
  SimpleTelnetCallback _onInput;

  // -----------------------------------------------------------------------
  // Private helpers
  // -----------------------------------------------------------------------

  /** @brief Poll WiFiServer for new connections; fill first free slot. */
  void _acceptNewClients();

  /**
   * @brief Accept a client into slot idx; set up socket options; fire onConnect.
   * @param idx Slot index.
   * @param c   Newly accepted WiFiClient (copied into slot).
   */
  void _connectClient(uint8_t idx, WiFiClient& c);

  /**
   * @brief Evict slot idx; fire onDisconnect if triggerEvent.
   * @param idx          Slot index.
   * @param triggerEvent If true, fires onDisconnect callback.
   */
  void _disconnectClient(uint8_t idx, bool triggerEvent);

  /**
   * @brief Periodic liveness check for all active slots.
   * @note ESP8266: client.status() == ESTABLISHED (lwIP TCP state value 4).
   * @note ESP32: client.connected().
   */
  void _checkKeepAlive();

  /**
   * @brief Read pending bytes from all active clients; dispatch to input handler.
   *
   * Called by loop() only when _onInput != nullptr.
   * In streaming mode (no callback) this method is never called — bytes
   * remain in the TCP buffer for read() / available().
   */
  void _processInput();

  /**
   * @brief Process one character in line (CLI) mode.
   *
   * Handles CR, LF, CR+LF, backspace (0x08 and 0x7F), bell (0x07 — ignored),
   * high bytes >= 0x80 (ignored, covers telnet IAC 0xFF), and printable ASCII.
   *
   * @param c Received character.
   */
  void _handleLineInput(char c);

  /**
   * @brief Process one character in char (streaming-callback) mode.
   *
   * Fires _onInput with a 2-byte null-terminated buffer containing c.
   * All bytes (including control chars) are passed through — consistent
   * with ESPTelnet char mode.
   *
   * @param c Received character.
   */
  void _handleCharInput(char c);

  /**
   * @brief Flush and drain a client slot without blocking.
   *
   * Replaces ESPTelnet emptyClientStream() but without the delay(50).
   * ESP8266: calls client.flush(timeout_ms).
   * ESP32: calls client.flush().
   * Then reads and discards any remaining bytes.
   *
   * @param idx Slot index.
   */
  void _drainClient(uint8_t idx);

  /**
   * @brief Increment write-error counter for slot idx; evict at threshold.
   * @param idx Slot index.
   */
  void _onWriteError(uint8_t idx);

  /**
   * @brief Write IP address of addr into buf without using String or toString().
   * @param addr IPAddress to format.
   * @param buf  Output buffer (at least SIMPLETELNET_IP_LEN bytes).
   * @param len  Buffer size.
   */
  static void _extractIP(IPAddress addr, char* buf, size_t len);

  /** @brief Returns true if at least one slot is currently active. */
  bool _hasActiveClient() const;
};

// -------------------------------------------------------------------------
// Template implementation — included here so the compiler sees it
// -------------------------------------------------------------------------
#include "SimpleTelnet_impl.tpp"

#endif // SimpleTelnet_h
