/*
 * SimpleTelnet_impl.tpp — synchronous transport implementation
 *
 * This file is included at the bottom of SimpleTelnet.h.
 * Do NOT include it directly.
 *
 * Contains only the WiFiServer/WiFiClient transport: connection management,
 * the Stream read/write interface and keep-alive. The transport-agnostic
 * protocol logic (line parsing, modes, callbacks, printf) lives in the base
 * class SimpleTelnetCore (SimpleTelnetCore.h). Base-class members are accessed
 * through this-> because the base is a dependent template type.
 *
 * Design constraints (all methods):
 *   - No String objects anywhere
 *   - No dynamic allocation in normal operation
 *   - No project-specific headers — Arduino.h + platform WiFi header only
 *   - ESP8266 and ESP32 differences isolated behind #ifdef guards
 */

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
SimpleTelnet<MAX_CLIENTS>::SimpleTelnet(uint16_t port)
  : SimpleTelnetCore<MAX_CLIENTS>(port)
  , _server(port)
  , _lastKeepAliveCheck(0)
{
  memset(_writeErrors, 0, sizeof(_writeErrors));
}

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
bool SimpleTelnet<MAX_CLIENTS>::begin(bool checkWiFi) {
  if (checkWiFi) {
#if defined(ARDUINO_ARCH_ESP8266)
    // isSet() exists on ESP8266 IPAddress
    if (WiFi.status() != WL_CONNECTED && !WiFi.softAPIP().isSet()) return false;
#elif defined(ARDUINO_ARCH_ESP32)
    // ESP32 IPAddress has no isSet(); compare string instead
    if (WiFi.status() != WL_CONNECTED &&
        WiFi.softAPIP().toString() == "0.0.0.0") return false;
#endif
  }
  _server.begin();
  _server.setNoDelay(true);
  _lastKeepAliveCheck = millis();  // anchor keep-alive timer to now
  return true;
}

template<uint8_t MAX_CLIENTS>
bool SimpleTelnet<MAX_CLIENTS>::begin(uint16_t port, bool checkWiFi) {
  this->_port = port;
  _server = WiFiServer(port);
  return begin(checkWiFi);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::stop() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i]) _disconnectClient(i, true);
  }
  _server.stop();
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::loop() {
  _acceptNewClients();
  _checkKeepAlive();
  if (this->_onInput) _processInput();
}

// -------------------------------------------------------------------------
// Connection state (transport actions)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::disconnectClient() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i]) _disconnectClient(i, true);
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::disconnectClient(uint8_t index, bool triggerEvent) {
  if (index < MAX_CLIENTS && this->_clientActive[index]) {
    _disconnectClient(index, triggerEvent);
  }
}

// -------------------------------------------------------------------------
// Stream — write (broadcast to all active clients)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
size_t SimpleTelnet<MAX_CLIENTS>::write(uint8_t val) {
  // Route through the buffer overload so 0xFF escaping (RFC 854) is applied
  // in one place.
  return write(&val, 1);
}

template<uint8_t MAX_CLIENTS>
size_t SimpleTelnet<MAX_CLIENTS>::write(const uint8_t* buf, size_t size) {
  if (this->_connectedCount == 0) return 0;
  bool anyOk = false;
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    size_t n = _writeClient(i, buf, size);
    if (n == 0) {
      // Total write failure: the TCP stack couldn't accept even one byte.
      // This is the reliable signal of a dead connection.
      _onWriteError(i);
    } else {
      // Partial success (n < size) is normal when the TCP send buffer is
      // temporarily full.  Clear error counter and let the data already
      // delivered count as a heartbeat.  The keep-alive check detects dead
      // connections via status() independently of write success.
      _writeErrors[i] = 0;
      anyOk = true;
    }
  }
  // Return size if at least one client accepted at least one byte.
  return anyOk ? size : 0;
}

// -------------------------------------------------------------------------
// Stream — read (from first client with available data)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
int SimpleTelnet<MAX_CLIENTS>::available() {
  if (this->_negMode == SimpleTelnetCore<MAX_CLIENTS>::NEG_OFF) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
      if (this->_clientActive[i]) {
        int n = _clients[i].available();
        if (n > 0) return n;   // return first non-zero, not sum
      }
    }
    return 0;
  }
  // Negotiation on: count is a hint (raw bytes may include IAC sequences).
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    if (this->_peeked[i] >= 0) return 1 + _clients[i].available();
    int n = _clients[i].available();
    if (n > 0) return n;
  }
  return 0;
}

template<uint8_t MAX_CLIENTS>
int SimpleTelnet<MAX_CLIENTS>::read() {
  if (this->_negMode == SimpleTelnetCore<MAX_CLIENTS>::NEG_OFF) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
      if (this->_clientActive[i] && _clients[i].available()) {
        return _clients[i].read();
      }
    }
    return -1;
  }
  // Negotiation on: filter IAC sequences out of the pull path.
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    if (this->_peeked[i] >= 0) { int v = this->_peeked[i]; this->_peeked[i] = -1; return v; }
    while (_clients[i].available()) {
      int d = this->_filterByte(i, (uint8_t)_clients[i].read());
      if (d >= 0) return d;
    }
  }
  return -1;
}

template<uint8_t MAX_CLIENTS>
int SimpleTelnet<MAX_CLIENTS>::peek() {
  if (this->_negMode == SimpleTelnetCore<MAX_CLIENTS>::NEG_OFF) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
      if (this->_clientActive[i] && _clients[i].available()) {
        return _clients[i].peek();
      }
    }
    return -1;
  }
  // Negotiation on: consume/answer IAC, hold the first NVT byte in _peeked.
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    if (this->_peeked[i] >= 0) return this->_peeked[i];
    while (_clients[i].available()) {
      int d = this->_filterByte(i, (uint8_t)_clients[i].read());
      if (d >= 0) { this->_peeked[i] = (int16_t)d; return d; }
    }
  }
  return -1;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::flush() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
#if defined(ARDUINO_ARCH_ESP8266)
    // flush(0): non-blocking nudge — tells the TCP stack to send any data
    // sitting in the send buffer without blocking for ACKs.
    //
    // Deliberately ignoring the return value.  ESPTelnet had a no-op flush()
    // (inherited from Stream), and callers like DebugFlush() invoke this very
    // frequently.  Treating flush timeouts as write errors caused spurious
    // disconnects because:
    //   - TCP delayed ACK (up to 200 ms) makes flush(timeout_ms) return false
    //     even when the connection is perfectly healthy.
    //   - DebugFlush() is called after every OT message and from MQTT/sensor
    //     code, so 3 timeout failures accumulate quickly and evict the client.
    //
    // Dead connections are detected reliably by _checkKeepAlive() via
    // status() == ESTABLISHED, which is the correct mechanism for this.
    _clients[i].flush(0);
#else
    _clients[i].flush();
#endif
  }
}

// -------------------------------------------------------------------------
// Private helpers — connection management
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_acceptNewClients() {
  if (!_server.hasClient()) return;
  // TASK-398: ESP8266 Arduino Core 3.0+ renamed available() -> accept(); the
  // old method was kept as deprecated. Pre-3.0 cores (2.7.4, 2.6.3, 2.5.2)
  // have ONLY available(). Pick the right call at compile time so this
  // library builds on both legacy (2.x) and modern (3.x) cores.
#if defined(ARDUINO_ESP8266_RELEASE_2_7_4) \
 || defined(ARDUINO_ESP8266_RELEASE_2_7_3) \
 || defined(ARDUINO_ESP8266_RELEASE_2_7_2) \
 || defined(ARDUINO_ESP8266_RELEASE_2_7_1) \
 || defined(ARDUINO_ESP8266_RELEASE_2_6_3) \
 || defined(ARDUINO_ESP8266_RELEASE_2_5_2)
  WiFiClient newClient = _server.available();
#else
  WiFiClient newClient = _server.accept();
#endif
  if (!newClient) return;

  // Find a free slot first.
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) {
      _connectClient(i, newClient);
      return;
    }
  }

  // All slots full.
  // Store the attempt IP and fire the notification callback.
  this->_extractIP(newClient.remoteIP(), this->_attemptIp, sizeof(this->_attemptIp));
  if (this->_onConnectionAttempt) this->_onConnectionAttempt(this->_attemptIp);

  // Reconnect check: MAX_CLIENTS==1 AND same IP as current occupant.
  // The connecting side likely tore down an old connection and reconnected;
  // silently rotate the slot rather than refusing them.
  if (MAX_CLIENTS == 1 && this->_clientActive[0] &&
      strncmp(this->_attemptIp, this->_ip[0], SIMPLETELNET_IP_LEN) == 0) {
    _disconnectClient(0, false);       // evict old, no disconnect event
    _connectClient(0, newClient);      // accept new
    if (this->_onReconnect) this->_onReconnect(this->_ip[0]);
    return;
  }

  newClient.stop();
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_connectClient(uint8_t idx, WiFiClient& c) {
  // Copy the client handle into the slot — WiFiClient copy is valid on
  // both ESP8266 and ESP32 (ref-counted handle internally).
  _clients[idx] = c;
  _clients[idx].setNoDelay(true);
  _clients[idx].setTimeout(this->_keepAliveInterval);
  this->_extractIP(_clients[idx].remoteIP(), this->_ip[idx], sizeof(this->_ip[idx]));
  this->_clientActive[idx] = true;
  _writeErrors[idx] = 0;
  this->_connectedCount++;
  this->_resetNegotiation(idx);                // fresh IAC parser state
  _drainClient(idx);                           // flush telnet negotiation
  if (this->_onConnect) this->_onConnect(this->_ip[idx]);
  this->_startNegotiation(idx);                // proactive ECHO/SGA (NEG_CHAR_ECHO)
#if defined(ARDUINO_ARCH_ESP8266)
  // Non-blocking flush: nudge the TCP stack to send the connect banner
  // immediately rather than waiting for the next yield().  Without this the
  // banner may sit in the send buffer for tens of milliseconds.
  _clients[idx].flush(0);
#endif
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_disconnectClient(uint8_t idx, bool triggerEvent) {
  if (!this->_clientActive[idx]) return;
  _drainClient(idx);
  _clients[idx].stop();
  if (triggerEvent && this->_onDisconnect) this->_onDisconnect(this->_ip[idx]);
  this->_ip[idx][0] = '\0';
  this->_clientActive[idx] = false;
  _writeErrors[idx] = 0;
  this->_resetNegotiation(idx);
  if (this->_connectedCount > 0) this->_connectedCount--;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_checkKeepAlive() {
  uint32_t now = millis();
  if ((uint32_t)(now - _lastKeepAliveCheck) < this->_keepAliveInterval) return;
  _lastKeepAliveCheck = now;

  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    bool alive;
#if defined(ARDUINO_ARCH_ESP8266)
    // client.status() == ESTABLISHED is the reliable liveness check on ESP8266.
    // client.connected() can return stale true after abrupt disconnect.
    // ESTABLISHED = 4 is the lwIP TCP_STATE enum value.
    alive = (_clients[i].status() == 4 /* ESTABLISHED */);
#elif defined(ARDUINO_ARCH_ESP32)
    alive = _clients[i].connected();
#endif
    if (!alive) _disconnectClient(i, true);
  }
}

// -------------------------------------------------------------------------
// Private helpers — input processing (feeds the protocol core)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_processInput() {
  // Only called when _onInput != nullptr (checked in loop()).
  // In streaming mode this still feeds char-mode callbacks; when no callback
  // is set the bytes simply remain in the TCP buffer for read()/available().
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    while (_clients[i].available()) {
      int b = _clients[i].read();
      if (b < 0) break;
      uint8_t bb = (uint8_t)b;
      this->_feed(i, &bb, 1);   // filters IAC, then dispatches NVT data
    }
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_drainClient(uint8_t idx) {
  // Flush outgoing data, then discard any incoming bytes (telnet negotiation).
  // Deliberately no delay() — we are in a cooperative scheduler.
#if defined(ARDUINO_ARCH_ESP8266)
  // ESP8266: flush(timeout_ms) returns bool — ignore result during drain.
  _clients[idx].flush(this->_keepAliveInterval);
#else
  _clients[idx].flush();
#endif
  while (_clients[idx].available()) {
    _clients[idx].read();
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_onWriteError(uint8_t idx) {
  _writeErrors[idx]++;
  if (_writeErrors[idx] >= SIMPLETELNET_MAX_WRITE_ERRORS) {
    _writeErrors[idx] = 0;
    _disconnectClient(idx, true);
  }
}

// -------------------------------------------------------------------------
// Telnet transport hooks (RFC 854)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_sendToClient(uint8_t idx, const uint8_t* buf, size_t len) {
  // Raw, un-escaped write for negotiation replies (already valid IAC bytes).
  if (!this->_clientActive[idx]) return;
  _clients[idx].write(buf, len);
#if defined(ARDUINO_ARCH_ESP8266)
  _clients[idx].flush(0);   // nudge the small reply out immediately
#endif
}

template<uint8_t MAX_CLIENTS>
size_t SimpleTelnet<MAX_CLIENTS>::_writeClient(uint8_t idx, const uint8_t* buf, size_t size) {
  if (size == 0) return 0;
  if (this->_negMode == SimpleTelnetCore<MAX_CLIENTS>::NEG_OFF) {
    return _clients[idx].write(buf, size);
  }
  // RFC 854: a data byte 0xFF must be doubled to IAC IAC. Write runs between
  // 0xFF bytes; insert a second 0xFF for each. No temp buffer needed.
  static const uint8_t ff2[2] = { 0xFF, 0xFF };
  bool anyOk = false;
  size_t start = 0;
  for (size_t i = 0; i < size; i++) {
    if (buf[i] == 0xFF) {
      if (i > start && _clients[idx].write(buf + start, i - start) > 0) anyOk = true;
      if (_clients[idx].write(ff2, 2) > 0) anyOk = true;
      start = i + 1;
    }
  }
  if (start < size && _clients[idx].write(buf + start, size - start) > 0) anyOk = true;
  return anyOk ? size : 0;
}
