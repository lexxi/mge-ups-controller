#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>

#define MGE_RX_PIN D5
#define MGE_TX_PIN D6

static const char *AP_PASSWORD = "mgeups123";
static const char *APP_VERSION = "0.6";
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

// Last 30 TYPE-44 values, kept in RAM for live analysis.
static const uint8_t TYPE44_HISTORY_SIZE = 30;
struct Type44HistoryEntry
{
  unsigned long millisAt;
  uint8_t p0;
  uint8_t p1;
  uint16_t p2p3;
};

Type44HistoryEntry type44History[TYPE44_HISTORY_SIZE];
uint8_t type44HistoryCount = 0;
uint8_t type44HistoryNext = 0;

// TYPE 44 decoded fields – deliberately no physical interpretation
uint8_t type44P0 = 0;
uint8_t type44P1 = 0;
uint16_t type44P2P3 = 0;

// Confirmed SHUT GET REPORT values (read-only).
bool shutTelemetryValid = false;
uint8_t shutCapacity = 0;
uint16_t shutRuntimeSeconds = 0;
uint16_t shutVoltageRaw = 0;
bool shutVoltageValid = false;
unsigned long shutLastPollMillis = 0;
static const unsigned long SHUT_POLL_INTERVAL_MS = 15000;

uint8_t rxFrame[32];
uint8_t rxFrameLen = 0;
bool rxInFrame = false;

// -----------------------------------------------------------------------------
// HTML
// -----------------------------------------------------------------------------

String htmlPage(const String &title, const String &body)
{
  String html;
  html.reserve(9000);

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
    "table{width:100%;border-collapse:collapse}"
    "td{padding:7px;border-bottom:1px solid #ddd}"
    "pre{background:#111;color:#ddd;padding:14px;border-radius:6px;overflow:auto;white-space:pre-wrap;word-break:break-all}"
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

  // XOR von TYPE/Payload ausgeschlossen:
  // bestätigt: frame[2] bis frame[len-2]
  for (uint8_t i = 2; i < len - 1; i++)
    checksum ^= data[i];

  return checksum == data[len - 1];
}

// -----------------------------------------------------------------------------
// WLAN
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
uint8_t shutChecksum(const uint8_t *data, uint8_t len)
{
  uint8_t checksum = 0;

  for (uint8_t i = 0; i < len; i++)
    checksum ^= data[i];

  return checksum;
}

void clearShutRx()
{
  while (mgeSerial.available()) mgeSerial.read();
}

void printHexLine(const char *prefix, const uint8_t *data, size_t len)
{
  Serial.print(prefix);
  for (size_t i = 0; i < len; i++)
  {
    if (i > 0) Serial.print(" ");
    Serial.printf("%02X", data[i]);
  }
  Serial.println();
}

void sendShutGetReport(uint8_t reportId)
{
  // HID GET_REPORT request, report type FEATURE (0x03),
  // interface 0, report size 8 bytes (larger than actual report is OK).
  uint8_t hidData[8] = {
    0xA1,       // bmRequestType: GET_REPORT / interface
    0x01,       // bRequest: GET_REPORT
    reportId,   // report ID
    0x03,       // report type: FEATURE
    0x00,       // interface / wValue high byte
    0x00,       // reserved / wIndex low byte
    0x08,       // requested report size
    0x00        // report size high byte
  };

  uint8_t packet[11];
  packet[0] = 0x81;                  // REQUEST + LAST packet
  packet[1] = 0x88;                  // 8 data bytes

  for (uint8_t i = 0; i < 8; i++)
    packet[2 + i] = hidData[i];

  packet[10] = shutChecksum(hidData, 8);

  printHexLine("SHUT GET REPORT TX: ", packet, sizeof(packet));

  mgeSerial.write(packet, sizeof(packet));
  mgeSerial.flush();
}



bool receiveShutResponse(uint8_t *out, size_t outMax, size_t &outLen, uint32_t timeoutMs)
{
  outLen = 0;
  unsigned long deadline = millis() + timeoutMs;

  while ((long)(millis() - deadline) < 0)
  {
    if (!mgeSerial.available())
    {
      yield();
      continue;
    }

    uint8_t type = mgeSerial.read();

    // Standalone protocol tokens can appear before a SHUT packet.
    // 0x06 is an ACK and must be consumed before parsing the response.
    if (type == 0x06 || type == 0x15 ||
        type == 0x16 || type == 0x17 || type == 0x18)
    {
      Serial.printf("SHUT token: %02X\n", type);
      continue;
    }

    // The length byte may arrive a few milliseconds after the type byte.
    // Do not discard the type and restart parsing if the byte is not
    // immediately available.
    unsigned long lenWaitStart = millis();
    while (!mgeSerial.available() && millis() - lenWaitStart < 500)
      yield();

    if (!mgeSerial.available())
    {
      Serial.println("Timeout waiting for SHUT length byte");
      continue;
    }

    uint8_t lenByte = mgeSerial.read();

    if ((lenByte >> 4) != (lenByte & 0x0F))
    {
      Serial.printf("Invalid SHUT length byte: %02X\n", lenByte);
      continue;
    }

    uint8_t len = lenByte & 0x0F;

    if (len > 8)
    {
      Serial.printf("Invalid SHUT payload length: %u\n", len);
      mgeSerial.write(0x15); // NACK
      mgeSerial.flush();
      continue;
    }

    uint8_t frame[8];
    uint8_t checksum = 0;

    for (uint8_t i = 0; i < len; i++)
    {
      unsigned long waitStart = millis();
      while (!mgeSerial.available() && millis() - waitStart < 1000)
        yield();

      if (!mgeSerial.available())
      {
        Serial.println("Timeout while receiving SHUT payload");
        return outLen > 0;
      }

      frame[i] = mgeSerial.read();
      checksum ^= frame[i];
    }

    unsigned long waitStart = millis();
    while (!mgeSerial.available() && millis() - waitStart < 1000)
      yield();

    if (!mgeSerial.available())
    {
      Serial.println("Timeout waiting for SHUT checksum");
      return outLen > 0;
    }

    uint8_t receivedChecksum = mgeSerial.read();

    Serial.printf("SHUT RX packet: type=%02X len=%u chk=%02X/%02X\n",
                  type, len, receivedChecksum, checksum);

    if (receivedChecksum != checksum)
    {
      Serial.println("SHUT checksum: BAD");
      mgeSerial.write(0x15); // NACK
      mgeSerial.flush();
      continue;
    }

    Serial.println("SHUT checksum: OK");

    for (uint8_t i = 0; i < len; i++)
    {
      if (outLen < outMax)
        out[outLen++] = frame[i];
    }

    // ACK every valid packet. The UPS waits for this before sending
    // the next fragment.
    mgeSerial.write(0x06);
    mgeSerial.flush();

    // LAST flag is in the high bit of bType, not in bLength.
    if (type & 0x80)
    {
      Serial.println("SHUT RX: LAST packet");
      return true;
    }

    Serial.println("SHUT RX: more fragments");
  }

  Serial.println("SHUT RX: timeout");
  return outLen > 0;
}


void pollShutTelemetry()
{
  if (millis() - shutLastPollMillis < SHUT_POLL_INTERVAL_MS)
    return;

  shutLastPollMillis = millis();

  uint8_t response[16];
  size_t responseLen = 0;

  auto getReport = [&](uint8_t reportId) -> bool {
    responseLen = 0;
    clearShutRx();
    mgeSerial.write(0x16);
    mgeSerial.flush();
    unsigned long start = millis();
    bool syncOk = false;
    while (millis() - start < 1200) {
      if (mgeSerial.available()) {
        uint8_t b = mgeSerial.read();
        if (b == 0x16) { syncOk = true; break; }
      }
      yield();
    }
    if (!syncOk) return false;
    clearShutRx();
    sendShutGetReport(reportId);
    return receiveShutResponse(response, sizeof(response), responseLen, 2500);
  };

  if (getReport(0x0E) && responseLen >= 3 && response[0] == 0x0E)
  {
    shutVoltageRaw = (uint16_t)response[1] |
                     ((uint16_t)response[2] << 8);
    shutVoltageValid = true;
  }

  if (getReport(0x16) && responseLen >= 4 && response[0] == 0x16)
  {
    shutCapacity = response[1];
    shutRuntimeSeconds = (uint16_t)response[2] |
                         ((uint16_t)response[3] << 8);
    shutTelemetryValid = true;
  }
}

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

  if (!lastType44ChecksumOk)
    return;

  // Confirmed raw field layout:
  //
  // P0   = frame[2]
  // P1   = frame[3]
  // P2P3 = frame[4] | (frame[5] << 8)
  //
  // No physical interpretation here.

  type44P0 = frame[2];
  type44P1 = frame[3];
  type44P2P3 =
      static_cast<uint16_t>(frame[4]) |
      (static_cast<uint16_t>(frame[5]) << 8);

  // Store the latest TYPE-44 value in the circular RAM history.
  Type44HistoryEntry &entry = type44History[type44HistoryNext];
  entry.millisAt = millis();
  entry.p0 = type44P0;
  entry.p1 = type44P1;
  entry.p2p3 = type44P2P3;

  type44HistoryNext =
      (type44HistoryNext + 1) % TYPE44_HISTORY_SIZE;

  if (type44HistoryCount < TYPE44_HISTORY_SIZE)
    type44HistoryCount++;
}

void processFrame(const uint8_t *frame, uint8_t len)
{
  if (len < 2 || frame[0] != 0x85)
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

  if (rxFrameLen >= sizeof(rxFrame))
  {
    rxInFrame = false;
    rxFrameLen = 0;
    return;
  }

  rxFrame[rxFrameLen++] = b;

  if (rxFrameLen == 2 &&
      rxFrame[1] != 0x33 &&
      rxFrame[1] != 0x44)
  {
    rxInFrame = false;
    rxFrameLen = 0;
    return;
  }

  if (rxFrameLen == 6 && rxFrame[1] == 0x33)
  {
    processFrame(rxFrame, rxFrameLen);

    rxInFrame = false;
    rxFrameLen = 0;

    return;
  }

  if (rxFrameLen == 7 && rxFrame[1] == 0x44)
  {
    processFrame(rxFrame, rxFrameLen);

    rxInFrame = false;
    rxFrameLen = 0;

    return;
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

    feedMgeByte(b);

    Serial.printf("%02X ", b);
  }

  if (received)
    Serial.println();
}

// -----------------------------------------------------------------------------
// Web
// -----------------------------------------------------------------------------

void handleRoot()
{
  String body;

  body += F("<div class='card'><h1>MGE UPS Controller</h1>");
  body += F("<p>Firmware <b>v");
  body += APP_VERSION;
  body += F("</b></p>");

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

  // ---------------------------------------------------------------------------
  // Communication
  // ---------------------------------------------------------------------------

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

  body += F("</table>");
  body += F("</div>");

  // ---------------------------------------------------------------------------
  // TYPE 33
  // ---------------------------------------------------------------------------

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

  // ---------------------------------------------------------------------------
  // SHUT telemetry
  body += F("<div class='card'><h2>SHUT – Telemetrie</h2><table>");
  body += F("<tr><td>Batterie</td><td>");
  if (shutTelemetryValid) { body += String(shutCapacity); body += F(" %"); }
  else body += F("Unbekannt");
  body += F("</td></tr>");
  body += F("<tr><td>Restlaufzeit</td><td>");
  if (shutTelemetryValid) {
    unsigned int minutes = shutRuntimeSeconds / 60;
    unsigned int seconds = shutRuntimeSeconds % 60;
    body += String(minutes); body += F(":");
    if (seconds < 10) body += F("0");
    body += String(seconds); body += F(" min");
  } else body += F("Unbekannt");
  body += F("</td></tr>");
  body += F("<tr><td>Report 0x0E Rohwert</td><td>");
  if (shutVoltageValid) {
    body += String(shutVoltageRaw); body += F(" (0x");
    String voltageHex = String(shutVoltageRaw, HEX); voltageHex.toUpperCase();
    body += voltageHex; body += F(")");
  } else body += F("Unbekannt");
  body += F("</td></tr></table>");
  body += F("<p><small>SHUT-Werte werden aktiv per GET REPORT abgefragt. 0x0E wird noch nicht skaliert.</small></p></div>");

  // TYPE 44
  // ---------------------------------------------------------------------------

  body += F("<div class='card'>");
  body += F("<h2>TYPE 44 – Telemetrie</h2>");

  if (lastType44Hex.length() == 0)
  {
    body += F("<p>Noch kein TYPE-44-Telegramm empfangen.</p>");
  }
  else
  {
    body += F("<table>");

    body += F("<tr><td>Telegramm</td><td><code>");
    body += lastType44Hex;
    body += F("</code></td></tr>");

    body += F("<tr><td>Checksum</td><td>");

    if (lastType44ChecksumOk)
      body += F("<span class='ok'>OK</span>");
    else
      body += F("<span class='bad'>FEHLER</span>");

    body += F("</td></tr>");

    body += F("<tr><td>P0</td><td>0x");
    body += hexByte(type44P0);
    body += F(" (");
    body += String(type44P0);
    body += F(")</td></tr>");

    body += F("<tr><td>P1</td><td>0x");
    body += hexByte(type44P1);
    body += F(" (");
    body += String(type44P1);
    body += F(")</td></tr>");

    body += F("<tr><td>P2P3</td><td>0x");

    String p2p3Hex;
    if (type44P2P3 < 0x1000)
      p2p3Hex += "0";

    if (type44P2P3 < 0x100)
      p2p3Hex += "0";

    if (type44P2P3 < 0x10)
      p2p3Hex += "0";

    p2p3Hex += String(type44P2P3, HEX);
    p2p3Hex.toUpperCase();

    body += p2p3Hex;

    body += F(" (");
    body += String(type44P2P3);
    body += F(")</td></tr>");

    body += F("</table>");

    body += F(
      "<p><small>"
      "Die Felder werden absichtlich nicht als Spannung, "
      "Last, Laufzeit oder Prozentwert interpretiert."
      "</small></p>"
    );
  }

  body += F("</div>");

  // ---------------------------------------------------------------------------
  // TYPE 44 history
  // ---------------------------------------------------------------------------

  body += F("<div class='card'>");
  body += F("<h2>TYPE 44 – Verlauf</h2>");

  if (type44HistoryCount == 0)
  {
    body += F("<p>Noch kein TYPE-44-Verlauf vorhanden.</p>");
  }
  else
  {
    body += F("<table><tr><th>Zeit</th><th>P1</th><th>P2P3</th></tr>");

    uint8_t oldestIndex;

    if (type44HistoryCount < TYPE44_HISTORY_SIZE)
      oldestIndex = 0;
    else
      oldestIndex = type44HistoryNext;

    for (uint8_t i = 0; i < type44HistoryCount; i++)
    {
      uint8_t index =
          (oldestIndex + i) % TYPE44_HISTORY_SIZE;

      const Type44HistoryEntry &entry = type44History[index];

      unsigned long ageSeconds =
          (millis() - entry.millisAt) / 1000;

      String p2p3Hex;

      if (entry.p2p3 < 0x1000) p2p3Hex += "0";
      if (entry.p2p3 < 0x100)  p2p3Hex += "0";
      if (entry.p2p3 < 0x10)   p2p3Hex += "0";

      p2p3Hex += String(entry.p2p3, HEX);
      p2p3Hex.toUpperCase();

      body += F("<tr><td>vor " );
      body += String(ageSeconds);
      body += F(" s</td><td>0x");
      body += hexByte(entry.p1);
      body += F(" (");
      body += String(entry.p1);
      body += F(")</td><td>0x");
      body += p2p3Hex;
      body += F(" (");
      body += String(entry.p2p3);
      body += F(")</td></tr>");
    }

    body += F("</table>");
  }

  body += F("<p><small>Letzte 30 gültigen TYPE-44-Telegramme. Verlauf liegt nur im RAM und wird beim Neustart gelöscht.</small></p>");
  body += F("</div>");

  // ---------------------------------------------------------------------------
  // Raw RX
  // ---------------------------------------------------------------------------

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
  Serial.print("MGE UPS Controller v");
  Serial.println(APP_VERSION);
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
  pollShutTelemetry();
}