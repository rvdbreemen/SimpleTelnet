/*
 * SimpleTelnet — StreamingMode example
 *
 * PURPOSE: Serial-to-TCP proxy, equivalent to TelnetStream / OTGWstream.
 *
 * This example shows how to use SimpleTelnet as a transparent bridge between
 * the hardware Serial port and one or more TCP clients. Everything written to
 * Serial by the firmware shows up on every connected telnet session, and every
 * keystroke from a telnet session is forwarded to Serial.read().
 *
 * This is the pattern used by OTGW-firmware's OTGWstream: the OT message log
 * flows out on TCP port 25238 so that multiple OpenTherm Monitor clients can
 * tap the stream simultaneously, without any of them "stealing" data from the
 * others.
 *
 * Compile target: ESP8266 or ESP32 (no platform-specific code in this sketch).
 * Connect with: telnet <device-ip> 25238
 *
 * Wiring: none beyond WiFi. All output goes to the telnet stream.
 */

// ── Includes ─────────────────────────────────────────────────────────────────

#include <SimpleTelnet.h>

// ── Configuration ─────────────────────────────────────────────────────────────

// Change to your network credentials before uploading.
const char* ssid     = "your-ssid";
const char* password = "your-password";

// ── SimpleTelnet instance ─────────────────────────────────────────────────────

// MAX_CLIENTS = 4 means up to four simultaneous TCP connections are accepted.
// Each connected client receives an identical copy of every write() — this is
// the "broadcast" semantics of streaming mode.
//
// Port 25238 is used instead of 23 to avoid conflicts with the debug telnet
// terminal that typically runs on port 23. Use a separate port for each
// independent stream.
SimpleTelnet<4> telnet(25238);

// ── Forward declarations ───────────────────────────────────────────────────────

void onTelnetConnect(const char* ip);
void onTelnetDisconnect(const char* ip);

// ── setup() ──────────────────────────────────────────────────────────────────

void setup() {

  // ── Serial ───────────────────────────────────────────────────────────────
  Serial.begin(115200);
  delay(100);

  // ── WiFi Setup ───────────────────────────────────────────────────────────
  Serial.print(F("Connecting to WiFi"));
  WiFi.begin(ssid, password);

  // Poll until connected — this works on both ESP8266 and ESP32.
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("Connected. IP: "));
  Serial.println(WiFi.localIP());

  // ── SimpleTelnet Setup ───────────────────────────────────────────────────

  // Register callbacks BEFORE begin() so they fire for the very first client.

  // onConnect fires for each client that successfully completes the TCP
  // handshake. The ip argument is a null-terminated IPv4 string ("192.168.1.x").
  // connectedCount() already includes the new client when this fires.
  telnet.onConnect(onTelnetConnect);

  // onDisconnect fires when a client drops (TCP close, timeout, or
  // disconnectClient() call). connectedCount() has already been decremented.
  telnet.onDisconnect(onTelnetDisconnect);

  // begin(false): the 'false' tells SimpleTelnet to bind the server immediately
  // without waiting for WiFi to be fully up. In this example WiFi is already
  // connected, so true/false makes no difference here. The parameter exists for
  // the OTGWstream use case where the server must be ready before WiFi joins —
  // passing false lets you call begin() early in setup() and still have clients
  // connect the moment the IP is assigned.
  telnet.begin(false);

  Serial.println(F("SimpleTelnet streaming server started on port 25238."));
}

// ── loop() ───────────────────────────────────────────────────────────────────

void loop() {

  // telnet.loop() MUST be called every iteration. It:
  //   - accepts new TCP connections (up to MAX_CLIENTS)
  //   - runs the keep-alive check and evicts dead clients
  //   - reads incoming bytes from all clients (available()/read())
  // Without this call the server is effectively frozen.
  telnet.loop();

  // ── Serial → Telnet ──────────────────────────────────────────────────────

  // Forward every byte arriving on the hardware Serial RX pin to all connected
  // telnet clients. On ESP8266 this is the PIC OTGW RX line; on a dev board it
  // would be whatever device is attached to the hardware UART.
  while (Serial.available()) {
    // write() broadcasts to every currently connected client in one call.
    telnet.write((uint8_t)Serial.read());
  }

  // ── Telnet → Serial ──────────────────────────────────────────────────────

  // Forward every byte typed by a telnet client back to the hardware Serial TX.
  // In streaming mode, read() returns one byte from the first client that has
  // data pending. Each byte is consumed once (no broadcast on the read path).
  while (telnet.available()) {
    Serial.write((uint8_t)telnet.read());
  }
}

// ── Callback implementations ──────────────────────────────────────────────────

void onTelnetConnect(const char* ip) {
  // printf_P uses a PROGMEM format string (ESP8266) or normal RAM string (ESP32).
  // SimpleTelnet handles the #ifdef internally — just use printf_P with PSTR()
  // for flash-safe strings on ESP8266 and it will work on ESP32 too.
#if defined(ARDUINO_ARCH_ESP8266)
  telnet.printf_P(PSTR("Stream connected from %s (%u/%u clients active)\r\n"),
                  ip, telnet.connectedCount(), 4);
#else
  telnet.printf("Stream connected from %s (%u/4 clients active)\r\n",
                ip, telnet.connectedCount());
#endif

  // Also log to Serial so you can see connection events without a telnet client.
  Serial.print(F("Telnet client connected: "));
  Serial.println(ip);
}

void onTelnetDisconnect(const char* ip) {
  // ip is valid during the callback but must not be stored — it points into
  // SimpleTelnet's internal buffer which will be reused for the next client.
  Serial.print(F("Telnet client disconnected: "));
  Serial.println(ip);
}
