#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>

#define MGE_RX_PIN D5
#define MGE_TX_PIN D6

static const char *AP_PASSWORD = "mgeups123";
static const char *CONFIG_FILE = "/wifi.cfg";

ESP8266WebServer server(80);
SoftwareSerial mgeSerial(MGE_RX_PIN, MGE_TX_PIN);

String wifiSsid;
String wifiPassword;
bool apMode = false;

// -----------------------------------------------------------------------------
// RX / Parser
// -----------------------------------------------------------------------------

String lastRxHex;
unsigned long lastRxMillis = 0;
unsigned long totalRxBytes = 0;

String lastType33Hex;
String lastType44Hex;

String upsStatus = "Unbekannt";
String upsPower = "Unbekannt";
String upsOperation = "Unbekannt";

bool lastType33ChecksumOk = false;
bool lastType44ChecksumOk = false;

unsigned long type33Count = 0;
unsigned long type44Count = 0;

uint8_t rxFrame[32];
uint8_t rxFrameLen = 0;
bool rxInFrame = false;

// -----------------------------------------------------------------------------
// HTML
// -----------------------------------------------------------------------------

String htmlPage(const String &title, const String &body)
{
  String html;
  html.reserve(8000);

  html += F("<!doctype html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<meta http-equiv='refresh' content='5'>");
  html += F("<title>");
  html += title;
  html += F("</title>");

  html += F(
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:820px;margin:30px auto;padding:0 18px;background:#f4f4f4;color:#222}"
    ".card{background:white;padding:20px;margin-bottom:18px;border-radius:10px;box-shadow:0 1px 5px #bbb}"
    "input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
    "button{padding:10px 18px;cursor:pointer}"
    "a{color:#06c}"
    ".ok{color:green}"
    ".warn{color:#b66a00}"
    ".bad{color:#b00020}"
    ".value{font-size:1.3em;font-weight:bold}"
    "pre{background:#111;color:#ddd;padding:14px;border-radius:6px;overflow:auto;white-space:pre-wrap;word-break:break-all}"
    "table{width:100%;border-collapse:collapse}"
    "td{padding:7px;border-bottom:1px solid #ddd}"
    "</style></head><body>"
  );

  html += body;
  html += F("</body></html>");

  return html;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

String hexByte(uint8_t b)
{
  String s;

  if (b < 0x10)
    s += "0";

  s += String(b, HEX);
  s.toUpperCase();

  return s;
}

String frameToHex(const uint8_t *data, uint8_t len)
{
  String result;

  for (uint8_t i = 0; i < len; i++)
  {
    if (i > 0)
      result += " ";

    result += hexByte(data[i]);
  }

  return result;
}

bool checkChecksum(const uint8_t *data, uint8_t len)
{
  if (len < 3)
    return false;

  uint8_t checksum = 0;

  // XOR aller Bytes zwischen TYPE und CHECKSUM
  // Beispiel:
  // 85 33 02 23 00 21
  //       02 ^ 23 ^ 00 = 21
  for (uint8_t i = 2; i < len - 1; i++)
  {
    checksum ^= data[i];
  }

  return checksum == data[len - 1];
}

// -----------------------------------------------------------------------------
// WLAN configuration
// -----------------------------------------------------------------------------

void saveConfig()
{
  File file = LittleFS.open(CONFIG_FILE, "w");

  if (!file)
    return;

  file.println(wifiSsid);
  file.println(wifiPassword);
  file.close();
}

bool loadConfig()
{
  if (!LittleFS.exists(CONFIG_FILE))
    return false;

  File file = LittleFS.open(CONFIG_FILE, "r");

  if (!file)
    return false;

  wifiSsid = file.readStringUntil('\n');
  wifiPassword = file.readStringUntil('\n');

  wifiSsid.trim();
  wifiPassword.trim();

  file.close();

  return wifiSsid.length() > 0;
}

void startAccessPoint()
{
  apMode = true;

  String hostname = "MGE-USV-" + String(ESP.getChipId(), HEX);
  hostname.toUpperCase();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(hostname.c_str(), AP_PASSWORD);

  Serial.println();
  Serial.println("Fallback AP started");
  Serial.print("SSID: ");
  Serial.println(hostname);
  Serial.print("Password: ");
  Serial.println(AP_PASSWORD);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
}

bool connectWifi()
{
  if (!loadConfig())
    return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  Serial.print("Connecting to WiFi");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000)
  {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    apMode = false;

    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    return true;
  }

  Serial.println("WiFi connection failed");

  return false;
}

// -----------------------------------------------------------------------------
// MGE parser
// -----------------------------------------------------------------------------

void processType33(const uint8_t *frame, uint8_t len)
{
  lastType33Hex = frameToHex(frame, len);
  lastType33ChecksumOk = checkChecksum(frame, len);
  type33Count++;

  if (!lastType33ChecksumOk)
  {
    upsStatus = "Checksum Fehler";
    upsPower = "Unbekannt";
    upsOperation = "Unbekannt";

    return;
  }

  // Bekannte TYPE-33 Telegramme

  if (len == 6 &&
      frame[0] == 0x85 &&
      frame[1] == 0x33 &&
      frame[2] == 0x02 &&
      frame[3] == 0x23 &&
      frame[4] == 0x00 &&
      frame[5] == 0x21)
  {
    upsStatus = "EIN";
    upsPower = "Netz vorhanden";
    upsOperation = "ONLINE";
  }
  else if (len == 6 &&
           frame[0] == 0x85 &&
           frame[1] == 0x33 &&
           frame[2] == 0x02 &&
           frame[3] == 0x24 &&
           frame[4] == 0x00 &&
           frame[5] == 0x26)
  {
    upsStatus = "EIN";
    upsPower = "Netz fehlt";
    upsOperation = "BATTERIE";
  }
  else if (len == 6 &&
           frame[0] == 0x85 &&
           frame[1] == 0x33 &&
           frame[2] == 0x02 &&
           frame[3] == 0x03 &&
           frame[4] == 0x00 &&
           frame[5] == 0x01)
  {
    upsStatus = "AUS";
    upsPower = "Netz vorhanden";
    upsOperation = "OFFLINE";
  }
  else if (len == 6 &&
           frame[0] == 0x85 &&
           frame[1] == 0x33 &&
           frame[2] == 0x02 &&
           frame[3] == 0x00 &&
           frame[4] == 0x00 &&
           frame[5] == 0x02)
  {
    upsStatus = "AUS";
    upsPower = "Netz fehlt";
    upsOperation = "OFFLINE / NETZAUSFALL";
  }
  else
  {
    // Unbekanntes TYPE-33 Telegramm:
    // Telegramm trotzdem anzeigen, aber nichts hineininterpretieren.
    upsStatus = "Unbekannter Status";
    upsPower = "Unbekannt";
    upsOperation = "Unbekannt";
  }
}

void processType44(const uint8_t *frame, uint8_t len)
{
  lastType44Hex = frameToHex(frame, len);
  lastType44ChecksumOk = checkChecksum(frame, len);
  type44Count++;
}

void processFrame(const uint8_t *frame, uint8_t len)
{
  if (len < 2)
    return;

  if (frame[0] != 0x85)
    return;

  switch (frame[1])
  {
    case 0x33:
      if (len == 6)
        processType33(frame, len);
      break;

    case 0x44:
      if (len == 7)
        processType44(frame, len);
      break;

    default:
      break;
  }
}

void feedMgeByte(uint8_t b)
{
  // Telegrammstart
  if (!rxInFrame)
  {
    if (b == 0x85)
    {
      rxInFrame = true;
      rxFrameLen = 0;
      rxFrame[rxFrameLen++] = b;
    }

    return;
  }

  // Buffer overflow protection
  if (rxFrameLen >= sizeof(rxFrame))
  {
    rxInFrame = false;
    rxFrameLen = 0;
    return;
  }

  rxFrame[rxFrameLen++] = b;

  // TYPE 33 = 6 Bytes
  if (rxFrameLen == 2 && rxFrame[1] == 0x33)
    return;

  if (rxFrameLen == 6 && rxFrame[1] == 0x33)
  {
    processFrame(rxFrame, rxFrameLen);

    rxInFrame = false;
    rxFrameLen = 0;

    return;
  }

  // TYPE 44 = 7 Bytes
  if (rxFrameLen == 2 && rxFrame[1] == 0x44)
    return;

  if (rxFrameLen == 7 && rxFrame[1] == 0x44)
  {
    processFrame(rxFrame, rxFrameLen);

    rxInFrame = false;
    rxFrameLen = 0;

    return;
  }

  // Unbekannter TYPE
  if (rxFrameLen == 2 &&
      rxFrame[1] != 0x33 &&
      rxFrame[1] != 0x44)
  {
    rxInFrame = false;
    rxFrameLen = 0;
  }
}

void readMgeSerial()
{
  bool received = false;

  while (mgeSerial.available())
  {
    uint8_t b = mgeSerial.read();

    if (lastRxHex.length() > 4000)
      lastRxHex.remove(0, 2000);

    if (lastRxHex.length() > 0)
      lastRxHex += " ";

    lastRxHex += hexByte(b);

    totalRxBytes++;
    lastRxMillis = millis();
    received = true;

    // Parser
    feedMgeByte(b);

    // USB-Debug
    Serial.printf("%02X ", b);
  }

  if (received)
    Serial.println();
}

// -----------------------------------------------------------------------------
// Web pages
// -----------------------------------------------------------------------------

void handleRoot()
{
  String body;

  body += F("<div class='card'><h1>MGE UPS Controller</h1>");

  if (apMode)
  {
    body += F("<p class='warn'><b>Fallback AP active</b></p>");
  }
  else
  {
    body += F("<p class='ok'><b>WLAN connected</b></p>");

    body += F("<p>IP address: <b>");
    body += WiFi.localIP().toString();
    body += F("</b></p>");
  }

  body += F("<p>ESP8266 chip ID: ");
  body += String(ESP.getChipId(), HEX);
  body += F("</p></div>");

  body += F(
    "<div class='card'>"
    "<h2>USV</h2>"
    "<p>Interface: 2400 Baud / 8N1</p>"
    "<p>RX: D5 &nbsp;&nbsp; TX: D6</p>"
    "<p><a href='/ups'>USV Monitor</a></p>"
    "</div>"
  );

  body += F(
    "<div class='card'>"
    "<a href='/config'>WLAN configuration</a>"
    "</div>"
  );

  server.send(
    200,
    "text/html",
    htmlPage("MGE UPS Controller", body)
  );
}

void handleUps()
{
  String body;

  body += F("<div class='card'>");
  body += F("<h1>USV Status</h1>");

  body += F("<table>");

  body += F("<tr><td>USV</td><td class='value'>");
  body += upsStatus;
  body += F("</td></tr>");

  body += F("<tr><td>Netz</td><td>");
  body += upsPower;
  body += F("</td></tr>");

  body += F("<tr><td>Betrieb</td><td>");
  body += upsOperation;
  body += F("</td></tr>");

  body += F("</table>");
  body += F("</div>");

  body += F("<div class='card'>");
  body += F("<h2>Kommunikation</h2>");

  body += F("<table>");

  body += F("<tr><td>Interface</td><td>2400 8N1</td></tr>");
  body += F("<tr><td>RX</td><td>D5</td></tr>");
  body += F("<tr><td>TX</td><td>D6</td></tr>");

  body += F("<tr><td>Total RX Bytes</td><td>");
  body += String(totalRxBytes);
  body += F("</td></tr>");

  body += F("<tr><td>TYPE 33 Telegramme</td><td>");
  body += String(type33Count);
  body += F("</td></tr>");

  body += F("<tr><td>TYPE 44 Telegramme</td><td>");
  body += String(type44Count);
  body += F("</td></tr>");

  body += F("<tr><td>Letzter RX</td><td>");

  if (lastRxMillis == 0)
  {
    body += F("noch keiner");
  }
  else
  {
    body += String((millis() - lastRxMillis) / 1000);
    body += F(" Sekunden");
  }

  body += F("</td></tr>");

  body += F("</table>");
  body += F("</div>");

  body += F("<div class='card'>");
  body += F("<h2>TYPE 33 – Status</h2>");

  if (lastType33Hex.length() == 0)
  {
    body += F("<p>Noch kein TYPE-33-Telegramm empfangen.</p>");
  }
  else
  {
    body += F("<pre>");
    body += lastType33Hex;
    body += F("</pre>");

    if (lastType33ChecksumOk)
      body += F("<p class='ok'>Checksum: OK</p>");
    else
      body += F("<p class='bad'>Checksum: FEHLER</p>");
  }

  body += F("</div>");

  body += F("<div class='card'>");
  body += F("<h2>TYPE 44 – Rohdaten</h2>");

  if (lastType44Hex.length() == 0)
  {
    body += F("<p>Noch kein TYPE-44-Telegramm empfangen.</p>");
  }
  else
  {
    body += F("<pre>");
    body += lastType44Hex;
    body += F("</pre>");

    if (lastType44ChecksumOk)
      body += F("<p class='ok'>Checksum: OK</p>");
    else
      body += F("<p class='bad'>Checksum: FEHLER</p>");
  }

  body += F("</div>");

  body += F("<div class='card'>");
  body += F("<h2>Raw RX</h2>");

  if (lastRxHex.length() == 0)
  {
    body += F("<pre>No data received yet.</pre>");
  }
  else
  {
    body += F("<pre>");
    body += lastRxHex;
    body += F("</pre>");
  }

  body += F("</div>");

  body += F(
    "<div class='card'>"
    "<p><a href='/ups'>Refresh</a></p>"
    "<p><a href='/'>Back</a></p>"
    "</div>"
  );

  server.send(
    200,
    "text/html",
    htmlPage("USV Monitor", body)
  );
}

void handleConfig()
{
  String body;

  body += F("<div class='card'><h1>WLAN configuration</h1>");

  body += F("<form method='POST' action='/save'>");

  body += F("<label>SSID</label>");
  body += F("<input name='ssid' value='");
  body += wifiSsid;
  body += F("' required>");

  body += F("<label>Password</label>");
  body += F("<input type='password' name='password' value='");
  body += wifiPassword;
  body += F("'>");

  body += F("<button type='submit'>Save and restart</button>");

  body += F("</form>");

  body += F("<p><a href='/'>Back</a></p>");

  body += F("</div>");

  server.send(
    200,
    "text/html",
    htmlPage("WLAN configuration", body)
  );
}

void handleSave()
{
  if (!server.hasArg("ssid"))
  {
    server.send(
      400,
      "text/plain",
      "SSID missing"
    );

    return;
  }

  wifiSsid = server.arg("ssid");
  wifiPassword = server.arg("password");

  saveConfig();

  server.send(
    200,
    "text/html",
    htmlPage(
      "Saved",
      "<div class='card'><h1>Saved</h1>"
      "<p>Configuration saved. Restarting...</p></div>"
    )
  );

  delay(1000);

  ESP.restart();
}

// -----------------------------------------------------------------------------
// Setup / Loop
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  delay(300);

  Serial.println();
  Serial.println("MGE UPS Controller v0.3");
  Serial.println("----------------------");

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount failed");
  }

  mgeSerial.begin(2400);

  Serial.println("MGE serial initialized: 2400 8N1");
  Serial.println("RX=D5 TX=D6");

  if (!connectWifi())
  {
    startAccessPoint();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/ups", HTTP_GET, handleUps);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);

  server.begin();

  Serial.println("HTTP server started");
}

void loop()
{
  server.handleClient();
  readMgeSerial();
}