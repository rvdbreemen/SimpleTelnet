/*
 * AsyncCLIMode — interactive async telnet CLI on ESP32 (AsyncTCP)
 *
 * The async counterpart of the CLIMode example: a single-client, character-at-
 * a-time debug terminal — but fully event-driven. There is NO telnet.loop():
 * connect/data/disconnect arrive as AsyncTCP events.
 *
 * Character-at-a-time + server echo are enabled via RFC 854 negotiation
 * (NEG_CHAR_ECHO): the server sends WILL ECHO + WILL SUPPRESS-GO-AHEAD and
 * echoes your keystrokes, so single keys work without pressing Enter and the
 * client does not need any local configuration.
 *
 * Requires: ESP32 + ESP32Async/AsyncTCP.
 * Connect with:  telnet <device-ip>
 */
#include <WiFi.h>
#include <AsyncTCP.h>
#include <AsyncSimpleTelnet.h>

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-pass";

AsyncSimpleTelnet<1> telnet(23);   // single client, standard telnet port

static void printHelp() {
  telnet.println(F("\r\nCommands:"));
  telnet.println(F("  h  this help"));
  telnet.println(F("  i  chip info"));
  telnet.println(F("  u  uptime"));
  telnet.println(F("  q  quit"));
}

void onTelnetConnect(const char* ip) {
  telnet.printf("Welcome %s — async CLI. Press 'h' for help.\r\n", ip);
}

void onTelnetDisconnect(const char* ip) {
  Serial.printf("[telnet] %s disconnected\n", ip);
}

// Char mode: fires once per keystroke with a 1-char string.
void onTelnetInput(const char* key) {
  switch (key[0]) {
    case 'h': case 'H': case '?': printHelp(); break;
    case 'i': case 'I':
      telnet.printf("\r\nchip: %s  cores: %u  flash: %u KB\r\n",
                    ESP.getChipModel(), ESP.getChipCores(),
                    (unsigned)(ESP.getFlashChipSize() / 1024));
      break;
    case 'u': case 'U':
      telnet.printf("\r\nuptime: %lu s\r\n", millis() / 1000UL);
      break;
    case 'q': case 'Q':
      telnet.println(F("\r\nbye"));
      telnet.disconnectClient();
      break;
    default:
      // printable keys are echoed by the server (NEG_CHAR_ECHO); ignore here
      break;
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.printf("\nIP: %s — telnet to port 23\n", WiFi.localIP().toString().c_str());

  telnet.setLineMode(false);                                  // per-keystroke
  telnet.setTelnetNegotiation(AsyncSimpleTelnet<1>::NEG_CHAR_ECHO);  // echo + SGA
  telnet.onConnect(onTelnetConnect);
  telnet.onDisconnect(onTelnetDisconnect);
  telnet.onInputReceived(onTelnetInput);
  telnet.begin();                                             // event-driven
}

void loop() {
  // Nothing to poll — AsyncTCP drives everything. Do your real work here.
  delay(10);
}
