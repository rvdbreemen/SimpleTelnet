/*
 * SimpleTelnet_impl.tpp — template method implementations
 *
 * This file is included at the bottom of SimpleTelnet.h.
 * Do NOT include it directly.
 *
 * Covers TASK-258 (connection management), TASK-259 (Stream interface),
 * TASK-260 (CLI input mode), and TASK-261 (printf helpers).
 *
 * Design constraints (all methods):
 *   - No String objects anywhere
 *   - No dynamic allocation in normal operation (only in printf overflow path)
 *   - No project-specific headers — Arduino.h + platform WiFi header only
 *   - ESP8266 and ESP32 differences isolated behind #ifdef guards
 */

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
SimpleTelnet<MAX_CLIENTS>::SimpleTelnet(uint16_t port)
  : _server(port)
  , _connectedCount(0)
  , _port(port)
  , _keepAliveInterval(SIMPLETELNET_KEEPALIVE_MS)
  , _lastKeepAliveCheck(0)
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
  memset(_writeErrors, 0, sizeof(_writeErrors));
  memset(_inputBuf, 0, sizeof(_inputBuf));
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
  _port = port;
  _server = WiFiServer(port);
  return begin(checkWiFi);
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::stop() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) _disconnectClient(i, true);
  }
  _server.stop();
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::loop() {
  _acceptNewClients();
  _checkKeepAlive();
  if (_onInput) _processInput();
}

// -------------------------------------------------------------------------
// Mode selection
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::setLineMode(bool enable) {
  _lineMode = enable;
}

template<uint8_t MAX_CLIENTS>
bool SimpleTelnet<MAX_CLIENTS>::isLineMode() const {
  return _lineMode;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::setNewlineChar(char c) {
  _newlineChar = c;
}

// -------------------------------------------------------------------------
// Connection state
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
uint8_t SimpleTelnet<MAX_CLIENTS>::connectedCount() const {
  return _connectedCount;
}

template<uint8_t MAX_CLIENTS>
bool SimpleTelnet<MAX_CLIENTS>::isConnected() const {
  return _connectedCount > 0;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::disconnectClient() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) _disconnectClient(i, true);
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::disconnectClient(uint8_t index, bool triggerEvent) {
  if (index < MAX_CLIENTS && _clientActive[index]) {
    _disconnectClient(index, triggerEvent);
  }
}

// -------------------------------------------------------------------------
// IP helpers
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
const char* SimpleTelnet<MAX_CLIENTS>::clientIP(uint8_t idx) const {
  if (idx < MAX_CLIENTS) return _ip[idx];
  return _ip[0];
}

template<uint8_t MAX_CLIENTS>
const char* SimpleTelnet<MAX_CLIENTS>::getIP() const {
  // ESPTelnet compatibility alias: return IP of first active slot, or slot 0
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) return _ip[i];
  }
  return _ip[0];
}

template<uint8_t MAX_CLIENTS>
const char* SimpleTelnet<MAX_CLIENTS>::getLastAttemptIP() const {
  return _attemptIp;
}

// -------------------------------------------------------------------------
// Keep-alive
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::setKeepAliveInterval(uint16_t ms) {
  _keepAliveInterval = ms;
}

template<uint8_t MAX_CLIENTS>
uint16_t SimpleTelnet<MAX_CLIENTS>::getKeepAliveInterval() const {
  return _keepAliveInterval;
}

// -------------------------------------------------------------------------
// Callbacks
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::onConnect(SimpleTelnetCallback f) {
  _onConnect = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::onDisconnect(SimpleTelnetCallback f) {
  _onDisconnect = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::onReconnect(SimpleTelnetCallback f) {
  _onReconnect = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::onConnectionAttempt(SimpleTelnetCallback f) {
  _onConnectionAttempt = f;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::onInputReceived(SimpleTelnetCallback f) {
  _onInput = f;
}

// -------------------------------------------------------------------------
// Stream — write (broadcast to all active clients)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
size_t SimpleTelnet<MAX_CLIENTS>::write(uint8_t val) {
  if (_connectedCount == 0) return 0;
  bool anyOk = false;
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!_clientActive[i]) continue;
    if (_clients[i].write(val) == 0) {
      _onWriteError(i);
    } else {
      _writeErrors[i] = 0;
      anyOk = true;
    }
  }
  // Return 1 if at least one client received it, 0 if all failed.
  // Stream contract: return bytes written to "the stream", not client count.
  return anyOk ? 1 : 0;
}

template<uint8_t MAX_CLIENTS>
size_t SimpleTelnet<MAX_CLIENTS>::write(const uint8_t* buf, size_t size) {
  if (_connectedCount == 0) return 0;
  bool anyOk = false;
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!_clientActive[i]) continue;
    size_t n = _clients[i].write(buf, size);
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
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) {
      int n = _clients[i].available();
      if (n > 0) return n;   // return first non-zero, not sum
    }
  }
  return 0;
}

template<uint8_t MAX_CLIENTS>
int SimpleTelnet<MAX_CLIENTS>::read() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i] && _clients[i].available()) {
      return _clients[i].read();
    }
  }
  return -1;
}

template<uint8_t MAX_CLIENTS>
int SimpleTelnet<MAX_CLIENTS>::peek() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i] && _clients[i].available()) {
      return _clients[i].peek();
    }
  }
  return -1;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::flush() {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!_clientActive[i]) continue;
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
// printf helpers (TASK-261)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
size_t SimpleTelnet<MAX_CLIENTS>::printf(const char* fmt, ...) {
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
size_t SimpleTelnet<MAX_CLIENTS>::printf_P(PGM_P fmt, ...) {
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
// Private helpers — connection management (TASK-258)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_acceptNewClients() {
  if (!_server.hasClient()) return;
  WiFiClient newClient = _server.accept();
  if (!newClient) return;

  // Find a free slot first.
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!_clientActive[i]) {
      _connectClient(i, newClient);
      return;
    }
  }

  // All slots full.
  // Store the attempt IP and fire the notification callback.
  _extractIP(newClient.remoteIP(), _attemptIp, sizeof(_attemptIp));
  if (_onConnectionAttempt) _onConnectionAttempt(_attemptIp);

  // Reconnect check: MAX_CLIENTS==1 AND same IP as current occupant.
  // The connecting side likely tore down an old connection and reconnected;
  // silently rotate the slot rather than refusing them.
  if (MAX_CLIENTS == 1 && _clientActive[0] &&
      strncmp(_attemptIp, _ip[0], SIMPLETELNET_IP_LEN) == 0) {
    _disconnectClient(0, false);       // evict old, no disconnect event
    _connectClient(0, newClient);      // accept new
    if (_onReconnect) _onReconnect(_ip[0]);
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
  _clients[idx].setTimeout(_keepAliveInterval);
  _extractIP(_clients[idx].remoteIP(), _ip[idx], sizeof(_ip[idx]));
  _clientActive[idx] = true;
  _writeErrors[idx] = 0;
  _connectedCount++;
  _drainClient(idx);                           // flush telnet negotiation
  if (_onConnect) _onConnect(_ip[idx]);
#if defined(ARDUINO_ARCH_ESP8266)
  // Non-blocking flush: nudge the TCP stack to send the connect banner
  // immediately rather than waiting for the next yield().  Without this the
  // banner may sit in the send buffer for tens of milliseconds.
  _clients[idx].flush(0);
#endif
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_disconnectClient(uint8_t idx, bool triggerEvent) {
  if (!_clientActive[idx]) return;
  _drainClient(idx);
  _clients[idx].stop();
  if (triggerEvent && _onDisconnect) _onDisconnect(_ip[idx]);
  _ip[idx][0] = '\0';
  _clientActive[idx] = false;
  _writeErrors[idx] = 0;
  if (_connectedCount > 0) _connectedCount--;
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_checkKeepAlive() {
  uint32_t now = millis();
  if ((uint32_t)(now - _lastKeepAliveCheck) < _keepAliveInterval) return;
  _lastKeepAliveCheck = now;

  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!_clientActive[i]) continue;
    bool alive;
#if defined(ARDUINO_ARCH_ESP8266)
    // client.status() == ESTABLISHED is the reliable liveness check on ESP8266.
    // client.connected() can return stale true after abrupt disconnect.
    // ESTABLISHED = 4 is the lwIP TCP_STATE enum value.
    // Reference: ESPTelnetBase.cpp isConnected().
    alive = (_clients[i].status() == 4 /* ESTABLISHED */);
#elif defined(ARDUINO_ARCH_ESP32)
    alive = _clients[i].connected();
#endif
    if (!alive) _disconnectClient(i, true);
  }
}

// -------------------------------------------------------------------------
// Private helpers — input processing (TASK-260)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_processInput() {
  // Only called when _onInput != nullptr (checked in loop()).
  // In streaming mode this method is never entered — bytes stay in TCP buffer.
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!_clientActive[i]) continue;
    while (_clients[i].available()) {
      int b = _clients[i].read();
      if (b < 0) break;
      char c = (char)b;
      if (_lineMode) {
        _handleLineInput(c);
      } else {
        _handleCharInput(c);
      }
    }
  }
}

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_handleLineInput(char c) {
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
void SimpleTelnet<MAX_CLIENTS>::_handleCharInput(char c) {
  // Char mode: dispatch every byte immediately as a single-char string.
  // Consistent with ESPTelnet char mode — all bytes including control chars
  // are passed through (ref: ESPTelnet.cpp handleInput() non-lineMode branch).
  // Using a local char[2] avoids any String or heap allocation.
  char buf[2] = {c, '\0'};
  _onInput(buf);
}

// -------------------------------------------------------------------------
// Private helpers — utilities
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_drainClient(uint8_t idx) {
  // Flush outgoing data, then discard any incoming bytes (telnet negotiation).
  // Deliberately no delay() — we are in a cooperative scheduler.
  // Reference: ESPTelnetBase.cpp emptyClientStream() had delay(50); we don't.
#if defined(ARDUINO_ARCH_ESP8266)
  // ESP8266: flush(timeout_ms) returns bool — ignore result during drain.
  _clients[idx].flush(_keepAliveInterval);
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

template<uint8_t MAX_CLIENTS>
void SimpleTelnet<MAX_CLIENTS>::_extractIP(IPAddress addr, char* buf, size_t len) {
  // No String, no toString() — plain snprintf with IPAddress operator[].
  // Works identically on ESP8266 and ESP32.
  snprintf(buf, len, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
}

template<uint8_t MAX_CLIENTS>
bool SimpleTelnet<MAX_CLIENTS>::_hasActiveClient() const {
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (_clientActive[i]) return true;
  }
  return false;
}
