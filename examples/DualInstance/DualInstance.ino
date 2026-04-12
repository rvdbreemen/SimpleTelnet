/*
 * SimpleTelnet — DualInstance example
 *
 * PURPOSE: Two completely independent SimpleTelnet instances on different ports
 *          demonstrating that they share no state and require no coordination.
 *
 * Instance 1 — stream (port 25238, MAX_CLIENTS = 4)
 *   Streaming proxy: forwards Serial↔TCP in both directions.
 *   Multiple clients can tap the same data simultaneously.
 *   Each new stream client is announced on the CLI channel.
 *
 * Instance 2 — cli (port 23, MAX_CLIENTS = 1)
 *   Interactive debug terminal with single-key command dispatch.
 *   Used to inspect device state and observe stream client events.
 *
 * Key design point:
 *   Each SimpleTelnet instance owns its own WiFiServer object and its own
 *   client slot array. They do NOT share state, buffers, or sockets.
 *   Independence is total — you can have as many instances as RAM permits.
 *
 * This mirrors the OTGW-firmware architecture:
 *   OTGWstream  — raw OT message stream on port 25238 (up to 4 clients)
 *   debugTelnet — human-readable debug log on port 23 (single client)
 *
 * Compile target: ESP8266 or ESP32.
 * Connect with:   telnet <device-ip> 23      (CLI terminal)
 *                 telnet <device-ip> 25238   (data stream)
 */

// ── Includes ─────────────────────────────────────────────────────────────────

#include <SimpleTelnet.h>

// ── Configuration ─────────────────────────────────────────────────────────────

const char* ssid     = "your-ssid";
const char* password = "your-password";

// ── SimpleTelnet instances ────────────────────────────────────────────────────

// Streaming instance — up to 4 simultaneous TCP clients.
// Every write() is broadcast to all connected clients.
// Port 25238 matches the OTGW-firmware OT stream port.
SimpleTelnet<4> stream(25238);

// CLI instance — single interactive debug terminal.
// MAX_CLIENTS = 1 means only one human session at a time.
// Port 23 is the standard telnet port.
SimpleTelnet<1> cli(23);

// ── Forward declarations ───────────────────────────────────────────────────────

void onStreamConnect(const char* ip);
void onStreamDisconnect(const char* ip);
void onCliConnect(const char* ip);
void onCliDisconnect(const char* ip);
void onCliInput(const char* line);
void cliPrintHelp();

// ── setup() ──────────────────────────────────────────────────────────────────

void setup() {

  // ── Serial ───────────────────────────────────────────────────────────────
  Serial.begin(115200);
  delay(100);

  // ── WiFi Setup ───────────────────────────────────────────────────────────
  Serial.print(F("Connecting to WiFi"));
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("WiFi connected. IP: "));
  Serial.println(WiFi.localIP());

  // ── Stream instance setup ─────────────────────────────────────────────────

  // Streaming mode is the default — no setLineMode() call needed.
  // Callbacks are registered before begin() so they fire for the first client.
  stream.onConnect(onStreamConnect);
  stream.onDisconnect(onStreamDisconnect);

  // begin(false): bind the TCP socket unconditionally, without re-checking
  // WiFi state. This mirrors the OTGWstream use case where the server must be
  // ready to accept connections the instant the IP address is assigned —
  // calling begin(false) early in setup() ensures no client arrives before
  // the socket is open even on a slow join.
  stream.begin(false);

  Serial.println(F("Stream server started on port 25238."));

  // ── CLI instance setup ───────────────────────────────────────────────────

  // char-by-char dispatch: each byte fires onCliInput immediately.
  // The operator presses a single key — no Enter required.
  cli.setLineMode(false);

  cli.onConnect(onCliConnect);
  cli.onDisconnect(onCliDisconnect);
  cli.onInputReceived(onCliInput);

  // begin(true) — default: only starts if WiFi is up.
  // Since we confirmed WiFi above this will always succeed here.
  cli.begin();

  Serial.println(F("CLI server started on port 23."));
  Serial.println(F("Both servers are now independent and fully active."));
}

// ── loop() ───────────────────────────────────────────────────────────────────

void loop() {

  // IMPORTANT: BOTH instances must call .loop() every iteration.
  // Each call processes that instance's own WiFiServer and client sockets.
  // There is no shared polling mechanism — skipping one call freezes that
  // instance entirely (no new connections, no keep-alive, no input).
  stream.loop();
  cli.loop();

  // ── Serial → stream (and echo to CLI) ────────────────────────────────────

  // Forward hardware Serial bytes to all stream clients.
  // This is the bidirectional proxy: device data flows out over TCP.
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    stream.write(b);  // broadcast to all 4 possible stream clients
  }

  // ── Stream → Serial ──────────────────────────────────────────────────────

  // Forward bytes typed in any stream session back to the hardware UART.
  while (stream.available()) {
    Serial.write((uint8_t)stream.read());
  }

  // ── Periodic data on stream ───────────────────────────────────────────────

  // Simulate a sensor reading arriving every 2 seconds so stream clients see
  // live data without needing real hardware attached.
  static uint32_t lastStreamMs = 0;
  if (millis() - lastStreamMs >= 2000UL) {
    lastStreamMs = millis();
    if (stream.isConnected()) {
      // printf() on the stream instance: only stream clients see this output.
      // cli clients see nothing — the two instances are completely independent.
#if defined(ARDUINO_ARCH_ESP8266)
      stream.printf_P(PSTR("T: %lu  heap: %u\r\n"), millis() / 1000UL, ESP.getFreeHeap());
#else
      stream.printf("T: %lu  heap: %u\r\n", millis() / 1000UL, (unsigned)ESP.getFreeHeap());
#endif
    }
  }
}

// ── Stream callbacks ──────────────────────────────────────────────────────────

void onStreamConnect(const char* ip) {
  // Log the stream connection event on the CLI channel so the operator can
  // see it in real time. This is the key cross-instance communication pattern:
  // one instance's callback writes to the OTHER instance's output. This works
  // because both instances are independent — there is no deadlock risk.
#if defined(ARDUINO_ARCH_ESP8266)
  cli.printf_P(PSTR("[stream] client connected: %s  (%u/4 active)\r\n"),
               ip, stream.connectedCount());
#else
  cli.printf("[stream] client connected: %s  (%u/4 active)\r\n",
             ip, stream.connectedCount());
#endif

  Serial.print(F("Stream connect: "));
  Serial.println(ip);
}

void onStreamDisconnect(const char* ip) {
#if defined(ARDUINO_ARCH_ESP8266)
  cli.printf_P(PSTR("[stream] client disconnected: %s  (%u/4 active)\r\n"),
               ip, stream.connectedCount());
#else
  cli.printf("[stream] client disconnected: %s  (%u/4 active)\r\n",
             ip, stream.connectedCount());
#endif

  Serial.print(F("Stream disconnect: "));
  Serial.println(ip);
}

// ── CLI callbacks ─────────────────────────────────────────────────────────────

void onCliConnect(const char* ip) {
  cli.println(F("┌──────────────────────────────────────────┐"));
  cli.println(F("│  SimpleTelnet DualInstance example        │"));
  cli.println(F("│  Stream: port 25238  |  CLI: port 23      │"));
  cli.println(F("│  Press 'h' for help                       │"));
  cli.println(F("└──────────────────────────────────────────┘"));

#if defined(ARDUINO_ARCH_ESP8266)
  cli.printf_P(PSTR("Your IP: %s\r\n"), ip);
#else
  cli.printf("Your IP: %s\r\n", ip);
#endif

  Serial.print(F("CLI connect: "));
  Serial.println(ip);
}

void onCliDisconnect(const char* ip) {
  Serial.print(F("CLI disconnect: "));
  Serial.println(ip);
}

void onCliInput(const char* line) {
  // Each invocation carries exactly one character (setLineMode(false)).
  switch ((unsigned char)line[0]) {

    case 'h':
    case 'H':
    case '?':
      cliPrintHelp();
      break;

    case 's':
    case 'S': {
      // Report status of both instances. This demonstrates how each instance
      // independently tracks its own connection count.
#if defined(ARDUINO_ARCH_ESP8266)
      cli.printf_P(PSTR("Stream clients : %u / 4\r\n"), stream.connectedCount());
      cli.printf_P(PSTR("CLI clients    : %u / 1\r\n"), cli.connectedCount());
      cli.printf_P(PSTR("Free heap      : %u bytes\r\n"), ESP.getFreeHeap());
      cli.printf_P(PSTR("Uptime         : %lu s\r\n"), millis() / 1000UL);
#else
      cli.printf("Stream clients : %u / 4\r\n", stream.connectedCount());
      cli.printf("CLI clients    : %u / 1\r\n", cli.connectedCount());
      cli.printf("Free heap      : %u bytes\r\n", (unsigned)ESP.getFreeHeap());
      cli.printf("Uptime         : %lu s\r\n", millis() / 1000UL);
#endif
      break;
    }

    case 'k':
    case 'K':
      // Kick all stream clients (useful for testing reconnect behaviour).
      if (stream.isConnected()) {
        cli.println(F("Kicking all stream clients..."));
        stream.disconnectClient();  // no index = disconnect all
      } else {
        cli.println(F("No stream clients connected."));
      }
      break;

    case 'q':
    case 'Q':
      cli.println(F("Goodbye!"));
      cli.disconnectClient();
      break;

    case '\r':
    case '\n':
      // Silently ignore bare newline/carriage-return characters that telnet
      // clients send as part of their line-ending sequences.
      break;

    default:
#if defined(ARDUINO_ARCH_ESP8266)
      cli.printf_P(PSTR("Unknown: '%c'  (press 'h' for help)\r\n"), line[0]);
#else
      cli.printf("Unknown: '%c'  (press 'h' for help)\r\n", line[0]);
#endif
      break;
  }
}

// ── Helper ────────────────────────────────────────────────────────────────────

void cliPrintHelp() {
  cli.println(F("Commands (single key):"));
  cli.println(F("  h / ?  — this help text"));
  cli.println(F("  s      — status (both instances)"));
  cli.println(F("  k      — kick all stream clients"));
  cli.println(F("  q      — disconnect CLI session"));
}
