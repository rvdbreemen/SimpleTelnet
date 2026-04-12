/*
 * SimpleTelnet — CLIMode example
 *
 * PURPOSE: Interactive single-client debug terminal with char-by-char dispatch.
 *
 * This example shows SimpleTelnet in CLI mode where each byte typed by the
 * telnet user fires the onInputReceived callback immediately (no buffering
 * until newline). This pattern suits a minimal debug REPL where single
 * keystrokes trigger commands, similar to a microcontroller monitor prompt.
 *
 * Differences from the line-mode (ESPTelnet default):
 *   setLineMode(true)  — callback fires once per line (newline terminates)
 *   setLineMode(false) — callback fires for every single character received
 *
 * This sketch uses setLineMode(false) so that pressing 'h' alone (without
 * Enter) immediately prints the help text. The experience feels like a
 * single-key menu, not a shell.
 *
 * MAX_CLIENTS = 1: only one interactive session is allowed at a time.
 * A second connection attempt while a client is active will be queued by the
 * TCP stack and accepted as soon as the first client disconnects.
 *
 * Compile target: ESP8266 or ESP32 (no platform-specific code in this sketch
 * except the printf_P / PSTR pattern which SimpleTelnet abstracts).
 * Connect with: telnet <device-ip> 23
 */

// ── Includes ─────────────────────────────────────────────────────────────────

#include <SimpleTelnet.h>

// ── Configuration ─────────────────────────────────────────────────────────────

// Change to your network credentials before uploading.
const char* ssid     = "your-ssid";
const char* password = "your-password";

// ── SimpleTelnet instance ─────────────────────────────────────────────────────

// Single-client interactive terminal on the standard telnet port.
// MAX_CLIENTS = 1 keeps RAM usage minimal — no need for per-slot input buffers
// when only one human types at a time.
SimpleTelnet<1> telnet(23);

// ── Forward declarations ───────────────────────────────────────────────────────

void onTelnetConnect(const char* ip);
void onTelnetDisconnect(const char* ip);
void onTelnetInput(const char* line);  // 'line' is one char (setLineMode false)
void printHelp();
void printInfo();

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
  Serial.print(F("Connected. IP: "));
  Serial.println(WiFi.localIP());

  // ── SimpleTelnet Setup ───────────────────────────────────────────────────

  // char-by-char dispatch: onInputReceived fires for EVERY byte received.
  // The 'line' pointer passed to the callback contains exactly one character
  // followed by a null terminator (line[0] is the key, line[1] == '\0').
  // Switch on line[0] to dispatch single-key commands.
  //
  // If you prefer whole-line commands (like a shell), call setLineMode(true)
  // instead and use strcmp() to dispatch on the full line string.
  telnet.setLineMode(false);

  // Register callbacks before begin() to catch the very first connection.
  telnet.onConnect(onTelnetConnect);
  telnet.onDisconnect(onTelnetDisconnect);
  telnet.onInputReceived(onTelnetInput);

  // begin(true) — the default: only starts the TCP server if WiFi is already
  // connected. Since we confirmed WiFi above, this is safe. The return value
  // is false if WiFi is not yet up; check it if you call begin() before WiFi.
  if (!telnet.begin()) {
    Serial.println(F("WARNING: telnet.begin() failed (WiFi not connected?)"));
  }

  Serial.println(F("SimpleTelnet CLI server started on port 23."));
}

// ── loop() ───────────────────────────────────────────────────────────────────

// Tracks the last time we sent a periodic status line to the client.
static uint32_t lastStatusMs = 0;

void loop() {

  // telnet.loop() handles new connections, keep-alive, and reads incoming bytes.
  // It fires onInputReceived synchronously for each byte as it is consumed.
  telnet.loop();

  // ── Periodic status line ─────────────────────────────────────────────────

  // Every 30 seconds, send an uptime reminder so the operator knows the
  // session is still alive even without typing. This also demonstrates that
  // you can write to telnet outside of callbacks — output and input coexist
  // on the same connection without any locking needed (cooperative scheduling).
  if (telnet.isConnected() && (millis() - lastStatusMs >= 30000UL)) {
    lastStatusMs = millis();
#if defined(ARDUINO_ARCH_ESP8266)
    telnet.printf_P(PSTR("[status] uptime %lu s  heap %u B  press 'h' for help\r\n"),
                    millis() / 1000, ESP.getFreeHeap());
#else
    telnet.printf("[status] uptime %lu s  heap %u B  press 'h' for help\r\n",
                  millis() / 1000, (unsigned)ESP.getFreeHeap());
#endif
  }
}

// ── Callback: onConnect ───────────────────────────────────────────────────────

void onTelnetConnect(const char* ip) {
  // Send a welcome banner. println(F(...)) keeps the string in flash on ESP8266.
  // The F() macro is a no-op on ESP32 but costs nothing there either.
  telnet.println(F("┌──────────────────────────────────────┐"));
  telnet.println(F("│  SimpleTelnet CLIMode example         │"));
  telnet.println(F("│  Press 'h' for help                   │"));
  telnet.println(F("└──────────────────────────────────────┘"));

  // printf_P with PSTR() for a formatted line that includes runtime values.
  // On ESP32, printf_P falls back to regular printf — PSTR() is a no-op there.
#if defined(ARDUINO_ARCH_ESP8266)
  telnet.printf_P(PSTR("Your IP: %s\r\n"), ip);
#else
  telnet.printf("Your IP: %s\r\n", ip);
#endif

  Serial.print(F("CLI client connected from: "));
  Serial.println(ip);
}

// ── Callback: onDisconnect ────────────────────────────────────────────────────

void onTelnetDisconnect(const char* ip) {
  // Do not store 'ip' — it points into SimpleTelnet's internal buffer.
  Serial.print(F("CLI client disconnected: "));
  Serial.println(ip);
}

// ── Callback: onInputReceived ─────────────────────────────────────────────────

void onTelnetInput(const char* line) {
  // In char mode (setLineMode(false)), line always contains exactly one
  // printable character. Switch on line[0] to dispatch the command.
  //
  // The cast to (unsigned char) avoids undefined behaviour when line[0]
  // is negative on platforms where char is signed.
  switch ((unsigned char)line[0]) {

    case 'h':
    case 'H':
    case '?':
      // Print the help menu.
      printHelp();
      break;

    case 'i':
    case 'I':
      // Print network / device information.
      printInfo();
      break;

    case 'u':
    case 'U':
      // Print current uptime in seconds. millis() overflows after ~49 days —
      // acceptable for a debug terminal.
#if defined(ARDUINO_ARCH_ESP8266)
      telnet.printf_P(PSTR("Uptime: %lu s\r\n"), millis() / 1000UL);
#else
      telnet.printf("Uptime: %lu s\r\n", millis() / 1000UL);
#endif
      break;

    case 'q':
    case 'Q':
      // Politely say goodbye and drop the TCP connection.
      telnet.println(F("Goodbye!"));
      // disconnectClient() with no arguments disconnects all active clients
      // (only one here since MAX_CLIENTS=1). onDisconnect will fire.
      telnet.disconnectClient();
      break;

    case '\r':
    case '\n':
      // Ignore bare newlines / carriage returns (telnet clients often send
      // these as part of line endings even when the user just pressed Enter).
      break;

    default:
      // Echo back anything unrecognised so the user gets immediate feedback.
#if defined(ARDUINO_ARCH_ESP8266)
      telnet.printf_P(PSTR("Unknown key: '%c'  (press 'h' for help)\r\n"), line[0]);
#else
      telnet.printf("Unknown key: '%c'  (press 'h' for help)\r\n", line[0]);
#endif
      break;
  }
}

// ── Helper: printHelp ─────────────────────────────────────────────────────────

void printHelp() {
  // F() strings live in flash on ESP8266. On ESP32 they are just const char*
  // in flash anyway — no overhead either way.
  telnet.println(F("Available commands (single key):"));
  telnet.println(F("  h / ?  — this help text"));
  telnet.println(F("  i      — device & network info"));
  telnet.println(F("  u      — uptime in seconds"));
  telnet.println(F("  q      — disconnect"));
}

// ── Helper: printInfo ─────────────────────────────────────────────────────────

void printInfo() {
  // getIP() returns the IP of the most recently connected client.
  // It returns a pointer to SimpleTelnet's internal buffer — use it immediately,
  // do not store it in a pointer for later use.
  const char* clientIp = telnet.getIP();

#if defined(ARDUINO_ARCH_ESP8266)
  telnet.printf_P(PSTR("Client IP  : %s\r\n"), clientIp);
  telnet.printf_P(PSTR("Device IP  : %s\r\n"), WiFi.localIP().toString().c_str());
  telnet.printf_P(PSTR("Free heap  : %u bytes\r\n"), ESP.getFreeHeap());
  telnet.printf_P(PSTR("Uptime     : %lu s\r\n"), millis() / 1000UL);
#else
  // On ESP32, toString() is also available; brief use of String here is
  // unavoidable without a platform-specific IPAddress-to-char[] helper.
  telnet.printf("Client IP  : %s\r\n", clientIp);
  telnet.printf("Device IP  : %s\r\n", WiFi.localIP().toString().c_str());
  telnet.printf("Free heap  : %u bytes\r\n", (unsigned)ESP.getFreeHeap());
  telnet.printf("Uptime     : %lu s\r\n", millis() / 1000UL);
#endif
}
