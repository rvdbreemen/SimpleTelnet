/*
 * AsyncSimpleTelnet — event-driven telnet server for ESP32 (AsyncTCP)
 *
 * Drop-in async counterpart of SimpleTelnet. Same public API, same callbacks,
 * same Stream interface — but built on AsyncTCP (the library behind
 * ESPAsyncWebServer) instead of a polled WiFiServer. There is NO required
 * loop(): connections, data and disconnects are delivered as AsyncTCP events.
 *
 * Migration from synchronous SimpleTelnet:
 *     #include <SimpleTelnet.h>            ->  #include <AsyncSimpleTelnet.h>
 *     SimpleTelnet<4> telnet(23);          ->  AsyncSimpleTelnet<4> telnet(23);
 * Everything else stays the same. telnet.loop() still exists as a no-op so
 * existing sketches compile unchanged.
 *
 * The protocol logic (line parsing, modes, callbacks, printf) is shared with
 * the synchronous library via the SimpleTelnetCore base class.
 *
 * !!! PROTOTYPE !!!  This file compiles against the AsyncTCP API but has not
 * yet been validated on hardware. See README in this folder.
 *
 * Copyright (c) 2026 Robert van den Breemen — MIT License
 */

#pragma once
#ifndef AsyncSimpleTelnet_h
#define AsyncSimpleTelnet_h

#if !defined(ARDUINO_ARCH_ESP32)
  #error "AsyncSimpleTelnet is ESP32-only (AsyncTCP). Use SimpleTelnet for ESP8266."
#endif

#include "SimpleTelnetCore.h"
#include <AsyncTCP.h>
#include <new>   // placement new (AsyncServer is not assignable)

// -------------------------------------------------------------------------
// Async-specific tunables
// -------------------------------------------------------------------------
// RX ring: only used in PULL mode (no onInputReceived callback) so read()/
// available()/peek() keep working. With a callback set, bytes are parsed
// immediately and this buffer stays empty.
#ifndef SIMPLETELNET_RX_BUF_LEN
  #define SIMPLETELNET_RX_BUF_LEN   256
#endif
// TX ring: holds bytes that did not fit in the AsyncClient send buffer; it is
// drained on the onAck event. Sized for one or two debug lines.
#ifndef SIMPLETELNET_TX_BUF_LEN
  #define SIMPLETELNET_TX_BUF_LEN   512
#endif
// Largest chunk copied out of the TX ring into AsyncClient::add() per step.
#ifndef SIMPLETELNET_TX_CHUNK_LEN
  #define SIMPLETELNET_TX_CHUNK_LEN 256
#endif

// -------------------------------------------------------------------------
// Tiny byte ring buffer (no heap, fixed capacity N)
// -------------------------------------------------------------------------
template<uint16_t N>
struct SimpleTelnetRing {
  uint8_t  buf[N];
  uint16_t head = 0;   // write index
  uint16_t tail = 0;   // read index
  bool     full = false;

  void clear() { head = tail = 0; full = false; }
  bool empty() const { return !full && head == tail; }
  uint16_t size() const {
    if (full) return N;
    return (head >= tail) ? (uint16_t)(head - tail) : (uint16_t)(N - tail + head);
  }
  uint16_t space() const { return (uint16_t)(N - size()); }

  // Push up to len bytes; returns the number actually stored (drops overflow).
  uint16_t push(const uint8_t* d, uint16_t len) {
    uint16_t n = 0;
    while (n < len && !full) {
      buf[head] = d[n++];
      head = (uint16_t)((head + 1) % N);
      if (head == tail) full = true;
    }
    return n;
  }
  int pop() {
    if (empty()) return -1;
    uint8_t v = buf[tail];
    tail = (uint16_t)((tail + 1) % N);
    full = false;
    return v;
  }
  int peek() const {
    if (empty()) return -1;
    return buf[tail];
  }
  // Copy up to n bytes from the front without removing them.
  uint16_t peekN(uint8_t* dst, uint16_t n) const {
    uint16_t avail = size();
    if (n > avail) n = avail;
    uint16_t t = tail;
    for (uint16_t i = 0; i < n; i++) { dst[i] = buf[t]; t = (uint16_t)((t + 1) % N); }
    return n;
  }
  void discard(uint16_t n) {
    uint16_t avail = size();
    if (n > avail) n = avail;
    tail = (uint16_t)((tail + n) % N);
    if (n > 0) full = false;
  }
};

// -------------------------------------------------------------------------
// AsyncSimpleTelnet<MAX_CLIENTS>
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS = 1>
class AsyncSimpleTelnet : public SimpleTelnetCore<MAX_CLIENTS> {
 public:
  explicit AsyncSimpleTelnet(uint16_t port = 23);
  ~AsyncSimpleTelnet();

  // Lifecycle ---------------------------------------------------------------
  bool begin(bool checkWiFi = true);
  bool begin(uint16_t port, bool checkWiFi = true);
  void stop();
  /** @brief No-op in async mode (events are pushed). Kept for drop-in use. */
  void loop() {}

  // Connection actions ------------------------------------------------------
  void disconnectClient();
  void disconnectClient(uint8_t index, bool triggerEvent = true);

  // Stream interface --------------------------------------------------------
  virtual size_t write(uint8_t val) override;
  virtual size_t write(const uint8_t* buf, size_t size) override;
  using Print::write;
  virtual int available() override;
  virtual int read() override;
  virtual int peek() override;
  virtual void flush() override;

 private:
  // Per-slot callback context passed to AsyncTCP handlers.
  struct Ctx { AsyncSimpleTelnet* self; uint8_t idx; };

  AsyncServer  _server;
  AsyncClient* _clientPtr[MAX_CLIENTS];
  Ctx          _ctx[MAX_CLIENTS];
  SimpleTelnetRing<SIMPLETELNET_RX_BUF_LEN> _rx[MAX_CLIENTS];
  SimpleTelnetRing<SIMPLETELNET_TX_BUF_LEN> _tx[MAX_CLIENTS];
  SemaphoreHandle_t _mutex;

  // AsyncTCP callbacks run on the AsyncTCP task; write()/printf() run on the
  // caller task. A recursive mutex guards the shared slot/ring state and lets
  // user callbacks (fired while locked) call back into write()/printf().
  void _lock()   { xSemaphoreTakeRecursive(_mutex, portMAX_DELAY); }
  void _unlock() { xSemaphoreGiveRecursive(_mutex); }

  void _installServerHandler();
  void _handleNewClient(AsyncClient* c);
  int8_t _findSlot(AsyncClient* c) const;        // -1 if not found
  void _attachClient(uint8_t idx, AsyncClient* c);
  void _onData(uint8_t idx, const uint8_t* data, size_t len);
  void _onClientDisconnect(uint8_t idx);
  void _releaseSlot(uint8_t idx, bool triggerEvent);
  void _flushTx(uint8_t idx);

  // Telnet (RFC 854) transport hooks.
  void _sendToClient(uint8_t idx, const uint8_t* buf, size_t len) override;  // raw, unescaped
  uint16_t _pushEscaped(uint8_t idx, const uint8_t* buf, uint16_t len);      // doubles 0xFF
};

// =========================================================================
// Implementation
// =========================================================================
template<uint8_t MAX_CLIENTS>
AsyncSimpleTelnet<MAX_CLIENTS>::AsyncSimpleTelnet(uint16_t port)
  : SimpleTelnetCore<MAX_CLIENTS>(port)
  , _server(port)
{
  _mutex = xSemaphoreCreateRecursiveMutex();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    _clientPtr[i] = nullptr;
    _ctx[i].self = this;
    _ctx[i].idx  = i;
  }
}

template<uint8_t MAX_CLIENTS>
AsyncSimpleTelnet<MAX_CLIENTS>::~AsyncSimpleTelnet() {
  stop();
  if (_mutex) vSemaphoreDelete(_mutex);
}

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_installServerHandler() {
  // Non-capturing lambda -> function pointer; 'this' arrives via arg.
  _server.onClient([](void* arg, AsyncClient* c) {
    static_cast<AsyncSimpleTelnet*>(arg)->_handleNewClient(c);
  }, this);
  _server.setNoDelay(true);
}

template<uint8_t MAX_CLIENTS>
bool AsyncSimpleTelnet<MAX_CLIENTS>::begin(bool checkWiFi) {
  if (checkWiFi) {
    if (WiFi.status() != WL_CONNECTED &&
        WiFi.softAPIP().toString() == "0.0.0.0") return false;
  }
  _installServerHandler();
  _server.begin();
  return true;
}

template<uint8_t MAX_CLIENTS>
bool AsyncSimpleTelnet<MAX_CLIENTS>::begin(uint16_t port, bool checkWiFi) {
  this->_port = port;
  _server.end();
  // AsyncServer has no setPort and is not assignable — reconstruct in place.
  _server.~AsyncServer();
  new (&_server) AsyncServer(port);
  return begin(checkWiFi);
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::stop() {
  _lock();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i]) _releaseSlot(i, true);
  }
  _unlock();
  _server.end();
}

// -------------------------------------------------------------------------
// Connection actions
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::disconnectClient() {
  _lock();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i]) _releaseSlot(i, true);
  }
  _unlock();
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::disconnectClient(uint8_t index, bool triggerEvent) {
  _lock();
  if (index < MAX_CLIENTS && this->_clientActive[index]) _releaseSlot(index, triggerEvent);
  _unlock();
}

// -------------------------------------------------------------------------
// New connection / slot management
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_handleNewClient(AsyncClient* c) {
  _lock();

  // Find a free slot.
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) { _attachClient(i, c); _unlock(); return; }
  }

  // All slots full — record attempt, fire callback.
  char ip[SIMPLETELNET_IP_LEN];
  this->_extractIP(c->remoteIP(), ip, sizeof(ip));
  strncpy(this->_attemptIp, ip, SIMPLETELNET_IP_LEN);
  this->_attemptIp[SIMPLETELNET_IP_LEN - 1] = '\0';
  if (this->_onConnectionAttempt) this->_onConnectionAttempt(this->_attemptIp);

  // Reconnect rotation (MAX_CLIENTS==1, same IP) — mirror SimpleTelnet.
  if (MAX_CLIENTS == 1 && this->_clientActive[0] &&
      strncmp(this->_attemptIp, this->_ip[0], SIMPLETELNET_IP_LEN) == 0) {
    _releaseSlot(0, false);   // evict old silently
    _attachClient(0, c);
    if (this->_onReconnect) this->_onReconnect(this->_ip[0]);
    _unlock();
    return;
  }

  _unlock();
  // Rejected: we own the client handle; closing + deleting drops the connection.
  c->close(true);
  delete c;
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_attachClient(uint8_t idx, AsyncClient* c) {
  _clientPtr[idx] = c;
  _rx[idx].clear();
  _tx[idx].clear();
  this->_extractIP(c->remoteIP(), this->_ip[idx], sizeof(this->_ip[idx]));
  this->_clientActive[idx] = true;
  this->_connectedCount++;
  this->_resetNegotiation(idx);   // fresh IAC parser state before data arrives

  c->setNoDelay(true);
  // Map keep-alive interval (ms) to AsyncTCP RX timeout (seconds, rounded up).
  uint32_t secs = (this->_keepAliveInterval + 999u) / 1000u;
  if (secs == 0) secs = 1;
  c->setRxTimeout(secs);

  void* arg = &_ctx[idx];
  c->onData([](void* a, AsyncClient*, void* data, size_t len) {
    Ctx* ctx = static_cast<Ctx*>(a);
    ctx->self->_onData(ctx->idx, static_cast<const uint8_t*>(data), len);
  }, arg);
  c->onDisconnect([](void* a, AsyncClient*) {
    Ctx* ctx = static_cast<Ctx*>(a);
    ctx->self->_onClientDisconnect(ctx->idx);
  }, arg);
  c->onError([](void* a, AsyncClient*, int8_t) {
    Ctx* ctx = static_cast<Ctx*>(a);
    ctx->self->_onClientDisconnect(ctx->idx);
  }, arg);
  c->onTimeout([](void* a, AsyncClient* cl, uint32_t) {
    cl->close();   // RX timeout -> close; onDisconnect will clean the slot
    (void)a;
  }, arg);
  c->onAck([](void* a, AsyncClient*, size_t, uint32_t) {
    Ctx* ctx = static_cast<Ctx*>(a);
    ctx->self->_lock();
    ctx->self->_flushTx(ctx->idx);   // send more queued bytes
    ctx->self->_unlock();
  }, arg);

  if (this->_onConnect) this->_onConnect(this->_ip[idx]);
  this->_startNegotiation(idx);   // proactive ECHO/SGA (NEG_CHAR_ECHO); via _tx
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_onClientDisconnect(uint8_t idx) {
  _lock();
  bool wasActive = this->_clientActive[idx];
  // The AsyncClient is being torn down by AsyncTCP; drop our reference first
  // so _releaseSlot does not try to close/delete it again.
  _clientPtr[idx] = nullptr;
  if (wasActive) _releaseSlot(idx, true);
  _unlock();
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_releaseSlot(uint8_t idx, bool triggerEvent) {
  if (!this->_clientActive[idx]) return;
  AsyncClient* c = _clientPtr[idx];
  _clientPtr[idx] = nullptr;

  if (triggerEvent && this->_onDisconnect) this->_onDisconnect(this->_ip[idx]);
  this->_ip[idx][0] = '\0';
  this->_clientActive[idx] = false;
  _rx[idx].clear();
  _tx[idx].clear();
  this->_resetNegotiation(idx);
  if (this->_connectedCount > 0) this->_connectedCount--;

  // If we still own a live handle (explicit disconnect, not an onDisconnect
  // event), close and delete it. On an event-driven disconnect the pointer was
  // already cleared by _onClientDisconnect and AsyncTCP frees the client.
  if (c) { c->close(true); delete c; }
}

// -------------------------------------------------------------------------
// Incoming data
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_onData(uint8_t idx, const uint8_t* data, size_t len) {
  _lock();
  if (!this->_clientActive[idx]) { _unlock(); return; }
  if (this->_onInput) {
    // Callback mode: filter IAC + parse immediately (line/char), no buffering.
    this->_feed(idx, data, len);
  } else {
    // Pull mode: filter IAC, stash NVT data for read()/available()/peek().
    for (size_t i = 0; i < len; i++) {
      int d = this->_filterByte(idx, data[i]);
      if (d >= 0) { uint8_t db = (uint8_t)d; _rx[idx].push(&db, 1); }
    }
  }
  _unlock();
}

// -------------------------------------------------------------------------
// Stream — read side (from per-client RX rings)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
int AsyncSimpleTelnet<MAX_CLIENTS>::available() {
  _lock();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i]) {
      uint16_t n = _rx[i].size();
      if (n > 0) { _unlock(); return n; }
    }
  }
  _unlock();
  return 0;
}

template<uint8_t MAX_CLIENTS>
int AsyncSimpleTelnet<MAX_CLIENTS>::read() {
  _lock();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i] && !_rx[i].empty()) { int v = _rx[i].pop(); _unlock(); return v; }
  }
  _unlock();
  return -1;
}

template<uint8_t MAX_CLIENTS>
int AsyncSimpleTelnet<MAX_CLIENTS>::peek() {
  _lock();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i] && !_rx[i].empty()) { int v = _rx[i].peek(); _unlock(); return v; }
  }
  _unlock();
  return -1;
}

// -------------------------------------------------------------------------
// Stream — write side (broadcast, non-blocking with TX ring + onAck drain)
// -------------------------------------------------------------------------
template<uint8_t MAX_CLIENTS>
size_t AsyncSimpleTelnet<MAX_CLIENTS>::write(uint8_t val) {
  return write(&val, 1);
}

template<uint8_t MAX_CLIENTS>
size_t AsyncSimpleTelnet<MAX_CLIENTS>::write(const uint8_t* buf, size_t size) {
  if (size == 0) return 0;
  _lock();
  if (this->_connectedCount == 0) { _unlock(); return 0; }
  bool anyOk = false;
  uint16_t n16 = (uint16_t)(size > 0xFFFF ? 0xFFFF : size);
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (!this->_clientActive[i]) continue;
    uint16_t pushed = _pushEscaped(i, buf, n16);   // 0xFF -> IAC IAC when negotiating
    if (pushed > 0) anyOk = true;
    _flushTx(i);
  }
  _unlock();
  // Stream contract: report the logical byte count if any client accepted data.
  return anyOk ? size : 0;
}

// Push data to the TX ring, doubling 0xFF -> IAC IAC (RFC 854) unless NEG_OFF.
template<uint8_t MAX_CLIENTS>
uint16_t AsyncSimpleTelnet<MAX_CLIENTS>::_pushEscaped(uint8_t idx, const uint8_t* buf, uint16_t len) {
  if (this->_negMode == SimpleTelnetCore<MAX_CLIENTS>::NEG_OFF) {
    return _tx[idx].push(buf, len);
  }
  uint16_t pushed = 0;
  for (uint16_t i = 0; i < len; i++) {
    if (buf[i] == 0xFF) {
      static const uint8_t ff2[2] = { 0xFF, 0xFF };
      pushed += _tx[idx].push(ff2, 2);
    } else {
      pushed += _tx[idx].push(&buf[i], 1);
    }
  }
  return pushed;
}

// Raw write for negotiation replies (already valid IAC bytes — never escaped).
// Callers hold the recursive mutex.
template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_sendToClient(uint8_t idx, const uint8_t* buf, size_t len) {
  if (!this->_clientActive[idx]) return;
  _tx[idx].push(buf, (uint16_t)len);
  _flushTx(idx);
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::_flushTx(uint8_t idx) {
  AsyncClient* c = _clientPtr[idx];
  if (!c || !c->connected()) return;
  bool sentAny = false;
  uint8_t tmp[SIMPLETELNET_TX_CHUNK_LEN];
  while (!_tx[idx].empty()) {
    size_t sp = c->space();
    if (sp == 0) break;                       // send buffer full — wait for onAck
    uint16_t chunk = _tx[idx].size();
    if (chunk > sp) chunk = (uint16_t)sp;
    if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
    uint16_t got = _tx[idx].peekN(tmp, chunk);
    size_t added = c->add((const char*)tmp, got, ASYNC_WRITE_FLAG_COPY);
    if (added == 0) break;
    _tx[idx].discard((uint16_t)added);
    sentAny = true;
    if (added < got) break;                   // couldn't take it all this round
  }
  if (sentAny) c->send();
}

template<uint8_t MAX_CLIENTS>
void AsyncSimpleTelnet<MAX_CLIENTS>::flush() {
  _lock();
  for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
    if (this->_clientActive[i]) _flushTx(i);
  }
  _unlock();
}

#endif // AsyncSimpleTelnet_h
