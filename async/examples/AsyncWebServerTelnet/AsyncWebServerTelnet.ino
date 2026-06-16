/*
 * AsyncWebServerTelnet — ESP32 example
 *
 * Runs an ESPAsyncWebServer and an AsyncSimpleTelnet side by side. Both are
 * event-driven on AsyncTCP: there is NO telnet.loop() polling in loop().
 *
 * The web server serves a tiny status page; the telnet CLI (port 23) accepts
 * single-key commands and can also be reached as a broadcast log channel.
 *
 * Requires: ESP32Async/AsyncTCP + ESP32Async/ESPAsyncWebServer.
 */
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncSimpleTelnet.h>

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-pass";

AsyncWebServer    web(80);
AsyncSimpleTelnet<3> telnet(23);   // up to 3 simultaneous telnet clients

void onTelnetConnect(const char* ip)   { telnet.printf("Welcome %s — press 'h' for help\r\n", ip); }
void onTelnetInput(const char* s) {
  switch (s[0]) {
    case 'h': telnet.println("[h]elp  [u]ptime  [r]am  [q]uit"); break;
    case 'u': telnet.printf("uptime: %lu s\r\n", millis() / 1000UL); break;
    case 'r': telnet.printf("free heap: %u B\r\n", (unsigned)ESP.getFreeHeap()); break;
    case 'q': telnet.println("bye"); telnet.disconnectClient(); break;
    default : telnet.printf("unknown: '%c'\r\n", s[0]); break;
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  // ---- async web server ----
  web.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "SimpleTelnet async demo — telnet to port 23");
  });
  web.begin();

  // ---- async telnet (char/CLI mode) ----
  telnet.setLineMode(false);            // single-key commands, no Enter needed
  telnet.onConnect(onTelnetConnect);
  telnet.onInputReceived(onTelnetInput);
  telnet.begin();                       // event-driven; no loop() needed
}

uint32_t last = 0;
void loop() {
  // Note: NO telnet.loop() required — AsyncTCP delivers events on its own task.
  // Just do your normal work; broadcast a heartbeat to all telnet clients.
  if (millis() - last >= 5000UL) {
    last = millis();
    if (telnet.isConnected())
      telnet.printf("[hb] %lu s, heap %u\r\n", millis() / 1000UL, (unsigned)ESP.getFreeHeap());
  }
}
