#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <time.h>
#include <stdarg.h>

#define MGE_RX_PIN D5
#define MGE_TX_PIN D6

static const char *AP_PASSWORD = "mgeups123";
static const char *APP_VERSION = "0.7.2";
static const char *CONFIG_FILE = "/wifi.cfg";
static const char *LOG_FILE = "/system.log";
static const size_t LOG_MAX_BYTES = 128 * 1024;

String logBuffer;
bool logReady = false;

ESP8266WebServer server(80);
SoftwareSerial mgeSerial(MGE_RX_PIN, MGE_TX_PIN);

String wifiSsid;
String wifiPassword;
bool apMode = false;

// -----------------------------------------------------------------------------
// Persistent logger
// -----------------------------------------------------------------------------

String logTimestamp()
{
  time_t now = time(nullptr);

  if (now > 1700000000)
  {
    struct tm tmNow;
    localtime_r(&now, &tmNow);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmNow);
    return String(buffer);
  }

  return String("+") + String(millis() / 1000.0, 3) + "s";
}

void writeLogLine(const String &line)
{
  if (!logReady)
    return;

  if (LittleFS.exists(LOG_FILE))
  {
    File existing = LittleFS.open(LOG_FILE, "r");
    if (existing)
    {
      size_t currentSize = existing.size();
      existing.close();

      if (currentSize >= LOG_MAX_BYTES)
        LittleFS.remove(LOG_FILE);
    }
  }

  File file = LittleFS.open(LOG_FILE, "a");

  if (!file)
    return;

  file.print("[");
  file.print(logTimestamp());
  file.print("] ");
  file.println(line);
  file.close();
}

void appendLogText(const String &text)
{
  for (size_t i = 0; i < text.length(); i++)
  {
    char c = text[i];

    if (c == '\n')
    {
      if (logBuffer.endsWith("\r"))
        logBuffer.remove(logBuffer.length() - 1);

      writeLogLine(logBuffer);
      logBuffer = "";
    }
    else
    {
      logBuffer += c;

      if (logBuffer.length() > 1024)
      {
        writeLogLine(logBuffer);
        logBuffer = "";
      }
    }
  }
}

template <typename T>
void logPrint(const T &value)
{
  Serial.print(value);
  appendLogText(String(value));
}

void logPrintln()
{
  Serial.println();
  writeLogLine(logBuffer);
  logBuffer = "";
}

void logPrintln(const IPAddress &value)
{
  Serial.println(value);
  appendLogText(value.toString());
  writeLogLine(logBuffer);
  logBuffer = "";
}

template <typename T>
void logPrintln(const T &value)
{
  Serial.println(value);
  appendLogText(String(value));
  writeLogLine(logBuffer);
  logBuffer = "";
}

void logPrintf(const char *format, ...)
{
  char buffer[512];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  Serial.print(buffer);
  appendLogText(String(buffer));
}

void initLogger()
{
  // Keep the persistent log in the LittleFS root. Create it explicitly first
  // because some ESP8266 LittleFS builds are picky about append mode.
  File file;

  if (LittleFS.exists(LOG_FILE))
    file = LittleFS.open(LOG_FILE, "a");
  else
    file = LittleFS.open(LOG_FILE, "w");

  if (file)
  {
    file.close();
    logReady = true;
    Serial.println("Persistent logger initialized");
  }
  else
  {
    Serial.println("ERROR: cannot create/open persistent log file");
  }
}

void syncClock()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  // Austria: CET/CEST with automatic DST transition.
  configTime("CET-1CEST,M3.5.0,M10.5.0",
             "pool.ntp.org",
             "time.nist.gov");

  unsigned long start = millis();

  while (time(nullptr) < 1700000000 &&
         millis() - start < 5000)
  {
    delay(100);
  }

  if (time(nullptr) >= 1700000000)
    logPrintln("NTP time synchronized");
  else
    logPrintln("NTP synchronization unavailable; using uptime timestamps");
}

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
uint8_t shutVoltage = 0;
uint8_t shutLoadPercent = 0;
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

  logPrintln();
  logPrintln("Fallback AP started");
  logPrint("SSID: ");
  logPrintln(hostname);
  logPrint("Password: ");
  logPrintln(AP_PASSWORD);
  logPrint("IP: ");
  logPrintln(WiFi.softAPIP());
}

bool connectWifi()
{
  if (!loadConfig())
    return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  logPrint("Connecting to WiFi");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000)
  {
    delay(250);
    logPrint(".");
  }

  logPrintln();

  if (WiFi.status() == WL_CONNECTED)
  {
    apMode = false;

    logPrintln("WiFi connected");
    logPrint("IP: ");
    logPrintln(WiFi.localIP());

    return true;
  }

  logPrintln("WiFi connection failed");

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
  logPrint(prefix);
  for (size_t i = 0; i < len; i++)
  {
    if (i > 0) logPrint(" ");
    logPrintf("%02X", data[i]);
  }
  logPrintln();
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



bool readShutByte(uint8_t &b, uint32_t timeoutMs)
{
  unsigned long start = millis();

  while (millis() - start < timeoutMs)
  {
    if (mgeSerial.available())
    {
      b = mgeSerial.read();
      return true;
    }

    yield();
  }

  return false;
}

bool consumeAsyncMgeFrame(uint8_t firstByte)
{
  // Spontaneous MGE status/telemetry frames use the normal 0x85 frame
  // header and fixed lengths:
  //   85 33 ... = 6 bytes
  //   85 44 ... = 7 bytes
  // They can arrive while a SHUT GET_REPORT response is pending.
  if (firstByte != 0x85)
    return false;

  uint8_t frame[7];
  frame[0] = firstByte;

  if (!readShutByte(frame[1], 1000))
  {
    logPrintln("Async MGE frame: timeout waiting for type");
    return true;
  }

  uint8_t expectedLen = 0;

  if (frame[1] == 0x33)
    expectedLen = 6;
  else if (frame[1] == 0x44)
    expectedLen = 7;
  else
  {
    logPrintf("Async MGE frame: unknown type %02X\n", frame[1]);
    return true;
  }

  for (uint8_t i = 2; i < expectedLen; i++)
  {
    if (!readShutByte(frame[i], 1000))
    {
      logPrintf("Async MGE frame: timeout at byte %u\n", i);
      return true;
    }
  }

  if (!checkChecksum(frame, expectedLen))
  {
    logPrintln("Async MGE frame: checksum BAD");
    return true;
  }

  logPrintf("Async MGE frame: %02X ", frame[1]);

  for (uint8_t i = 0; i < expectedLen; i++)
  {
    if (i > 0)
      logPrint(" ");

    logPrintf("%02X", frame[i]);
  }

  logPrintln();

  processFrame(frame, expectedLen);
  return true;
}

bool receiveShutResponse(uint8_t *out, size_t outMax, size_t &outLen, uint32_t timeoutMs)
{
  outLen = 0;
  unsigned long deadline = millis() + timeoutMs;

  while ((long)(millis() - deadline) < 0)
  {
    uint8_t type;

    if (!readShutByte(type, 50))
      continue;

    // The UPS can send spontaneous TYPE-33/TYPE-44 frames while we are
    // waiting for a GET_REPORT response. Consume them as complete frames
    // instead of treating 0x85 as a SHUT packet type.
    if (type == 0x85)
    {
      consumeAsyncMgeFrame(type);
      continue;
    }

    // Standalone protocol tokens can appear before a SHUT packet.
    if (type == 0x06 || type == 0x15 ||
        type == 0x16 || type == 0x17 || type == 0x18)
    {
      logPrintf("SHUT token: %02X\n", type);
      continue;
    }

    uint8_t lenByte;

    if (!readShutByte(lenByte, 500))
    {
      logPrintln("Timeout waiting for SHUT length byte");
      continue;
    }

    if ((lenByte >> 4) != (lenByte & 0x0F))
    {
      logPrintf("Invalid SHUT length byte: %02X\n", lenByte);
      continue;
    }

    uint8_t len = lenByte & 0x0F;

    if (len > 8)
    {
      logPrintf("Invalid SHUT payload length: %u\n", len);
      mgeSerial.write(0x15);
      mgeSerial.flush();
      continue;
    }

    uint8_t frame[8];
    uint8_t checksum = 0;

    for (uint8_t i = 0; i < len; i++)
    {
      if (!readShutByte(frame[i], 1000))
      {
        logPrintln("Timeout while receiving SHUT payload");
        return outLen > 0;
      }

      checksum ^= frame[i];
    }

    uint8_t receivedChecksum;

    if (!readShutByte(receivedChecksum, 1000))
    {
      logPrintln("Timeout waiting for SHUT checksum");
      return outLen > 0;
    }

    logPrintf("SHUT RX packet: type=%02X len=%u chk=%02X/%02X\n",
              type, len, receivedChecksum, checksum);

    if (receivedChecksum == checksum)
    {
      logPrint("SHUT RX payload: ");

      for (uint8_t i = 0; i < len; i++)
      {
        if (i > 0)
          logPrint(" ");

        logPrintf("%02X", frame[i]);
      }

      logPrintln();
    }

    if (receivedChecksum != checksum)
    {
      logPrintln("SHUT checksum: BAD");
      mgeSerial.write(0x15);
      mgeSerial.flush();
      continue;
    }

    logPrintln("SHUT checksum: OK");

    for (uint8_t i = 0; i < len; i++)
    {
      if (outLen < outMax)
        out[outLen++] = frame[i];
    }

    mgeSerial.write(0x06);
    mgeSerial.flush();

    if (type & 0x80)
    {
      logPrintln("SHUT RX: LAST packet");
      return true;
    }

    logPrintln("SHUT RX: more fragments");
  }

  logPrintln("SHUT RX: timeout");
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
    // Report 0x0E contains two independent 8-bit fields:
    // byte 1 = Voltage, byte 2 = PercentLoad.
    shutVoltage = response[1];
    shutLoadPercent = response[2];
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

    logPrintf("%02X ", b);
  }

  if (received)
    logPrintln();
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
    "<p><a href='/logs'>System Logs</a></p>"
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
  body += F("<tr><td>Spannung</td><td>");
  if (shutVoltageValid) {
    body += String(shutVoltage); body += F(" V");
  } else body += F("Unbekannt");
  body += F("</td></tr>");
  body += F("<tr><td>Last</td><td>");
  if (shutVoltageValid) {
    body += String(shutLoadPercent); body += F(" %");
  } else body += F("Unbekannt");
  body += F("</td></tr></table>");
  body += F("<p><small>SHUT-Werte werden aktiv per GET REPORT abgefragt. Report 0x0E: Spannung + Last.</small></p></div>");

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

void handleLogs()
{
  File file = LittleFS.open(LOG_FILE, "r");

  if (!file)
  {
    server.send(200, "text/plain; charset=utf-8", "No log entries.");
    return;
  }

  String body;
  body.reserve(16000);

  size_t size = file.size();

  // Keep the web page responsive; full log remains available via download.
  if (size > 16000)
  {
    file.seek(size - 16000, SeekSet);
    body = "[... log gekürzt; vollständiger Log über Download ...]\\n";
  }

  while (file.available())
    body += file.readStringUntil('\n') + "\n";

  file.close();

  String page;
  page.reserve(body.length() + 1500);
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>MGE Logs</title>";
  page += "<style>body{font-family:Arial;max-width:1000px;margin:20px auto;padding:0 15px}";
  page += "pre{background:#111;color:#ddd;padding:15px;white-space:pre-wrap;overflow:auto}";
  page += "a,button{display:inline-block;padding:9px 14px;margin:4px}</style></head><body>";
  page += "<h1>System Log</h1>";
  page += "<p>Firmware: v";
  page += APP_VERSION;
  page += " | Datei: ";
  page += String(size);
  page += " Bytes</p>";
  page += "<p><a href='/logs/download'>Log herunterladen</a>";
  page += "<a href='/logs/clear' onclick=\"return confirm('Log wirklich löschen?')\">Log löschen</a>";
  page += "<a href='/'>Zurück</a></p>";
  page += "<pre>";
  page += body;
  page += "</pre></body></html>";

  server.send(200, "text/html; charset=utf-8", page);
}

void handleLogDownload()
{
  File file = LittleFS.open(LOG_FILE, "r");

  if (!file)
  {
    server.send(404, "text/plain", "Log file not found");
    return;
  }

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=mge-ups-system.log"
  );

  server.streamFile(file, "text/plain; charset=utf-8");
  file.close();
}

void handleLogClear()
{
  LittleFS.remove(LOG_FILE);

  File file = LittleFS.open(LOG_FILE, "a");
  if (file)
    file.close();

  server.send(
    200,
    "text/html; charset=utf-8",
    "<html><body><h1>Log gelöscht</h1>"
    "<p><a href='/logs'>Zurück zum Log</a></p></body></html>"
  );

  logPrintln("System log cleared");
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

  logPrintln();
  logPrint("MGE UPS Controller v");
  logPrintln(APP_VERSION);
  logPrintln("----------------------");

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount failed");
  }
  else
  {
    initLogger();
  }

  mgeSerial.begin(2400);

  logPrintln("MGE serial initialized: 2400 8N1");
  logPrintln("RX=D5 TX=D6");

  if (!connectWifi())
  {
    startAccessPoint();
  }
  else
  {
    syncClock();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/ups", HTTP_GET, handleUps);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/logs/download", HTTP_GET, handleLogDownload);
  server.on("/logs/clear", HTTP_GET, handleLogClear);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);

  server.begin();

  logPrintln("HTTP server started");
}

void loop()
{
  server.handleClient();
  readMgeSerial();
  pollShutTelemetry();
}