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
 * This is the SYNCHRONOUS transport: it owns a WiFiServer + WiFiClient[] and
 * requires loop() to be called from the Arduino loop(). The transport-agnostic
 * protocol logic (line parsing, modes, callbacks, printf) lives in
 * SimpleTelnetCore.h, which is shared with the async ESP32 fork.
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

#include "SimpleTelnetCore.h"

// -------------------------------------------------------------------------
// Compile-time tunables specific to the synchronous transport
// -------------------------------------------------------------------------
#ifndef SIMPLETELNET_MAX_WRITE_ERRORS
  #define SIMPLETELNET_MAX_WRITE_ERRORS  3
#endif

// -------------------------------------------------------------------------
// SimpleTelnet<MAX_CLIENTS>
//
// MAX_CLIENTS: maximum simultaneous connections (template parameter).
//   SimpleTelnet<1> debugTelnet(23);          — single-client debug terminal
//   SimpleTelnet<4> otgwStream(OTGW_PORT);    — multi-client OT stream
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS = 1>
class SimpleTelnet : public SimpleTelnetCore<MAX_CLIENTS> {

 public:
  // -----------------------------------------------------------------------
  // Construction
  // -----------------------------------------------------------------------

  /**
   * @brief Constructor — port fixed at construction time.
   * @param port TCP port to listen on (default 23).
   */
  explicit SimpleTelnet(uint16_t port = 23);

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------

  /**
   * @brief Start listening on the port given at construction.
   * @param checkWiFi If true (default), returns false when WiFi is not
   *                  connected and the ESP is not in AP mode.
   * @return true if server started successfully, false if WiFi check failed.
   */
  bool begin(bool checkWiFi = true);

  /**
   * @brief Start listening on the given port (ESPTelnet compatibility).
   * @param port      TCP port to listen on.
   * @param checkWiFi If true (default), checks WiFi/AP status before binding.
   * @return true if server started successfully.
   */
  bool begin(uint16_t port, bool checkWiFi = true);

  /** @brief Stop all clients and the server socket. */
  void stop();

  /**
   * @brief Poll for new connections, keep-alive checks, and incoming data.
   * Call from loop(). Internally calls _acceptNewClients(), _checkKeepAlive(),
   * and (when onInputReceived is registered) _processInput().
   */
  void loop();

  // -----------------------------------------------------------------------
  // Connection state (transport-specific actions)
  // -----------------------------------------------------------------------

  /** @brief Forcibly disconnect all clients (triggers onDisconnect for each). */
  void disconnectClient();

  /**
   * @brief Disconnect a specific client by slot index.
   * @param index        Slot index (0..MAX_CLIENTS-1).
   * @param triggerEvent If true (default), fires onDisconnect callback.
   */
  void disconnectClient(uint8_t index, bool triggerEvent = true);

  // -----------------------------------------------------------------------
  // Stream interface (Arduino Stream + Print)
  // -----------------------------------------------------------------------

  /**
   * @brief Write one byte to all connected clients.
   * @return 1 if at least one client received it, 0 if all failed or none connected.
   */
  virtual size_t write(uint8_t val) override;

  /**
   * @brief Write a buffer to all connected clients.
   * @return size if at least one client received it, 0 if all failed or none connected.
   */
  virtual size_t write(const uint8_t* buf, size_t size) override;

  using Print::write;

  /** @brief Bytes available from the first client with data (0 if none). */
  virtual int available() override;

  /** @brief Read one byte from the first client that has data (-1 if none). */
  virtual int read() override;

  /** @brief Peek at the next byte from the first client that has data (-1 if none). */
  virtual int peek() override;

  /** @brief Flush all connected clients. */
  virtual void flush() override;

 private:
  // -----------------------------------------------------------------------
  // Transport state (the protocol/session state lives in the base)
  // -----------------------------------------------------------------------
  WiFiServer  _server;                          // listening socket
  WiFiClient  _clients[MAX_CLIENTS];            // active client slots
  uint8_t     _writeErrors[MAX_CLIENTS];        // consecutive write failures
  uint32_t    _lastKeepAliveCheck;              // timestamp of last keep-alive

  // -----------------------------------------------------------------------
  // Private helpers
  // -----------------------------------------------------------------------

  /** @brief Poll WiFiServer for new connections; fill first free slot. */
  void _acceptNewClients();

  /** @brief Accept a client into slot idx; set up socket options; fire onConnect. */
  void _connectClient(uint8_t idx, WiFiClient& c);

  /** @brief Evict slot idx; fire onDisconnect if triggerEvent. */
  void _disconnectClient(uint8_t idx, bool triggerEvent);

  /** @brief Periodic liveness check for all active slots. */
  void _checkKeepAlive();

  /** @brief Read pending bytes from all active clients; feed the parser. */
  void _processInput();

  /** @brief Flush and drain a client slot without blocking. */
  void _drainClient(uint8_t idx);

  /** @brief Increment write-error counter for slot idx; evict at threshold. */
  void _onWriteError(uint8_t idx);

  /** @brief Transport hook for negotiation replies — raw write to one client. */
  void _sendToClient(uint8_t idx, const uint8_t* buf, size_t len) override;

  /**
   * @brief Write a data buffer to one client, doubling 0xFF -> IAC IAC when
   * negotiation is enabled (RFC 854). Returns the original byte count if any
   * byte was accepted, else 0.
   */
  size_t _writeClient(uint8_t idx, const uint8_t* buf, size_t size);
};

// -------------------------------------------------------------------------
// Template implementation — included here so the compiler sees it
// -------------------------------------------------------------------------
#include "SimpleTelnet_impl.tpp"

#endif // SimpleTelnet_h
