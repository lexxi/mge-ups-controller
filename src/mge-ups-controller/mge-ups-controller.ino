#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <time.h>
#include <stdarg.h>

#define MGE_RX_PIN D6
#define MGE_TX_PIN D5

static const char *AP_PASSWORD = "mgeups123";
static const char *APP_VERSION = "0.12.1";
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

String formatUptime()
{
  unsigned long seconds = millis() / 1000UL;
  unsigned long days = seconds / 86400UL;
  seconds %= 86400UL;
  unsigned long hours = seconds / 3600UL;
  seconds %= 3600UL;
  unsigned long minutes = seconds / 60UL;
  seconds %= 60UL;

  char buffer[48];

  if (days > 0)
    snprintf(buffer, sizeof(buffer), "%lu d %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  else
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu", hours, minutes, seconds);

  return String(buffer);
}

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

String lastPresentStatusHex;
String lastBatteryReportHex;

String upsStatus = "Unbekannt";
String upsPower = "Unbekannt";
String upsOperation = "Unbekannt";

bool presentStatusChecksumOk = false;
bool batteryReportChecksumOk = false;

unsigned long presentStatusCount = 0;
unsigned long batteryReportCount = 0;

// HID Report 0x02 / UPS.PowerSummary.PresentStatus
uint16_t presentStatusBits = 0;
bool statusAcPresent = false;
bool statusCharging = false;
bool statusDischarging = false;
bool statusBelowCapacityLimit = false;
bool statusNeedReplacement = false;
bool statusGood = false;
bool statusShutdownImminent = false;
bool statusOverload = false;
bool statusInternalFailure = false;

// Last 30 HID Report 0x16 values, kept in RAM for live analysis.
static const uint8_t BATTERY_HISTORY_SIZE = 30;
struct BatteryHistoryEntry
{
  unsigned long millisAt;
  uint8_t capacity;
  uint16_t runtimeSeconds;
};

BatteryHistoryEntry batteryHistory[BATTERY_HISTORY_SIZE];
uint8_t batteryHistoryCount = 0;
uint8_t batteryHistoryNext = 0;

// Confirmed SHUT/HID GET_REPORT values.
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



void recordRxByte(uint8_t b)
{
  if (lastRxHex.length() > 4000)
    lastRxHex.remove(0, 2000);

  if (lastRxHex.length() > 0)
    lastRxHex += " ";

  lastRxHex += hexByte(b);
  totalRxBytes++;
  lastRxMillis = millis();
}

void sendShutGetDescriptor(uint8_t descriptorType, uint16_t requestedLength)
{
  // USB/HID GET_DESCRIPTOR over SHUT.
  // For HID and report descriptors bmRequestType must be 0x81.
  uint8_t hidData[8] = {
    0x81,                                   // bmRequestType: HID/interface
    0x06,                                   // bRequest: GET_DESCRIPTOR
    0x00,                                   // wValue LSB: descriptor index
    descriptorType,                         // wValue MSB: descriptor type
    0x00,                                   // wIndex LSB
    0x00,                                   // wIndex MSB
    (uint8_t)(requestedLength & 0xFF),       // wLength LSB
    (uint8_t)(requestedLength >> 8)          // wLength MSB
  };

  uint8_t packet[11];
  packet[0] = 0x81;
  packet[1] = 0x88;

  for (uint8_t i = 0; i < 8; i++)
    packet[2 + i] = hidData[i];

  packet[10] = shutChecksum(hidData, 8);

  printHexLine("SHUT GET DESCRIPTOR TX: ", packet, sizeof(packet));
  mgeSerial.write(packet, sizeof(packet));
  mgeSerial.flush();
}

bool syncShut(uint32_t timeoutMs = 1200)
{
  clearShutRx();
  mgeSerial.write(0x16);
  mgeSerial.flush();

  unsigned long start = millis();

  while (millis() - start < timeoutMs)
  {
    if (mgeSerial.available())
    {
      uint8_t b = mgeSerial.read();
      recordRxByte(b);

      if (b == 0x16)
        return true;
    }

    yield();
  }

  return false;
}

bool getShutDescriptor(uint8_t descriptorType,
                       uint8_t *out,
                       size_t outMax,
                       size_t &outLen,
                       uint16_t requestedLength)
{
  outLen = 0;

  if (!syncShut())
  {
    logPrintln("SHUT descriptor: sync failed");
    return false;
  }

  clearShutRx();
  sendShutGetDescriptor(descriptorType, requestedLength);

  if (requestedLength > 64)
    return receiveShutResponseExact(out, outMax, outLen, requestedLength, 30000);

  return receiveShutResponse(out, outMax, outLen, 5000);
}

bool readShutByte(uint8_t &b, uint32_t timeoutMs)
{
  unsigned long start = millis();

  while (millis() - start < timeoutMs)
  {
    if (mgeSerial.available())
    {
      b = mgeSerial.read();
      recordRxByte(b);
      return true;
    }

    yield();
  }

  return false;
}

bool processShutNotifyFrame(const uint8_t *frame, uint8_t len);

bool consumeAsyncShutNotify(uint8_t firstByte)
{
  // SHUT NOTIFY packet:
  //   byte 0 = packet type (0x85 = NOTIFY + LAST)
  //   byte 1 = mirrored payload length (e.g. 0x33 => 3 bytes)
  //   payload = HID report ID + report data
  //   final byte = XOR checksum over payload only
  if (firstByte != 0x85)
    return false;

  uint8_t frame[12];
  frame[0] = firstByte;

  if (!readShutByte(frame[1], 1000))
  {
    logPrintln("SHUT NOTIFY: timeout waiting for length");
    return true;
  }

  uint8_t lenByte = frame[1];

  if ((lenByte >> 4) != (lenByte & 0x0F))
  {
    logPrintf("SHUT NOTIFY: invalid length byte %02X\n", lenByte);
    return true;
  }

  uint8_t payloadLen = lenByte & 0x0F;

  if (payloadLen == 0 || payloadLen > 8)
  {
    logPrintf("SHUT NOTIFY: invalid payload length %u\n", payloadLen);
    return true;
  }

  uint8_t expectedLen = payloadLen + 3;

  for (uint8_t i = 2; i < expectedLen; i++)
  {
    if (!readShutByte(frame[i], 1000))
    {
      logPrintf("SHUT NOTIFY: timeout at byte %u\n", i);
      return true;
    }
  }

  printHexLine("SHUT NOTIFY RX: ", frame, expectedLen);

  if (!checkChecksum(frame, expectedLen))
  {
    logPrintln("SHUT NOTIFY checksum: BAD");
    return true;
  }

  logPrintln("SHUT NOTIFY checksum: OK");
  processShutNotifyFrame(frame, expectedLen);
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

    // The UPS can send spontaneous SHUT NOTIFY packets while a
    // GET_REPORT response is pending. Consume the complete packet first.
    if (type == 0x85)
    {
      consumeAsyncShutNotify(type);
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

bool receiveShutResponseExact(uint8_t *out,
                              size_t outMax,
                              size_t &outLen,
                              size_t expectedLen,
                              uint32_t timeoutMs)
{
  outLen = 0;
  unsigned long deadline = millis() + timeoutMs;

  while ((long)(millis() - deadline) < 0 && outLen < expectedLen)
  {
    uint8_t type;

    if (!readShutByte(type, 50))
      continue;

    if (type == 0x85)
    {
      consumeAsyncShutNotify(type);
      continue;
    }

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
        return outLen == expectedLen;
      }

      checksum ^= frame[i];
    }

    uint8_t receivedChecksum;

    if (!readShutByte(receivedChecksum, 1000))
    {
      logPrintln("Timeout waiting for SHUT checksum");
      return outLen == expectedLen;
    }

    // Keep bulk descriptor reads quiet. Persistent logging for every
    // 8-byte SHUT fragment can delay SoftwareSerial enough to lose data.

    if (receivedChecksum != checksum)
    {
      logPrintln("SHUT checksum: BAD");
      mgeSerial.write(0x15);
      mgeSerial.flush();
      continue;
    }

    for (uint8_t i = 0; i < len; i++)
    {
      if (outLen < outMax && outLen < expectedLen)
        out[outLen++] = frame[i];
    }

    mgeSerial.write(0x06);
    mgeSerial.flush();

    if (outLen >= expectedLen)
    {
      logPrintf("SHUT RX exact: complete %u bytes\n", (unsigned int)outLen);
      return true;
    }

    if ((type & 0x80) != 0 && (outLen % 64 == 0))
    {
      logPrintf("SHUT RX exact: progress %u/%u bytes\n",
                (unsigned int)outLen,
                (unsigned int)expectedLen);
    }
  }

  logPrintf("SHUT RX exact: incomplete %u/%u bytes\n",
            (unsigned int)outLen,
            (unsigned int)expectedLen);

  return outLen == expectedLen;
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
    if (!syncShut())
      return false;

    clearShutRx();
    sendShutGetReport(reportId);
    return receiveShutResponse(response, sizeof(response), responseLen, 2500);
  };

  if (getReport(0x02) && responseLen >= 3 && response[0] == 0x02)
  {
    lastPresentStatusHex = frameToHex(response, responseLen);
    presentStatusChecksumOk = true;
    presentStatusCount++;
    applyPresentStatusPayload(response, responseLen);
    updateUpsStateFromPresentStatus();
  }

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
    lastBatteryReportHex = frameToHex(response, responseLen);
    batteryReportChecksumOk = true;
    batteryReportCount++;
    storeBatteryPayload(response, responseLen);
  }
}

// SHUT/HID notification parser
// -----------------------------------------------------------------------------

void applyPresentStatusPayload(const uint8_t *payload, size_t len)
{
  if (len < 3 || payload[0] != 0x02)
    return;

  presentStatusBits =
      static_cast<uint16_t>(payload[1]) |
      (static_cast<uint16_t>(payload[2]) << 8);

  statusAcPresent          = presentStatusBits & (1u << 0);
  statusCharging           = presentStatusBits & (1u << 1);
  statusDischarging        = presentStatusBits & (1u << 2);
  statusBelowCapacityLimit = presentStatusBits & (1u << 3);
  statusNeedReplacement    = presentStatusBits & (1u << 4);
  statusGood               = presentStatusBits & (1u << 5);
  statusShutdownImminent   = presentStatusBits & (1u << 6);
  statusOverload           = presentStatusBits & (1u << 7);
  statusInternalFailure    = presentStatusBits & (1u << 8);
}

void storeBatteryPayload(const uint8_t *payload, size_t len)
{
  if (len < 4 || payload[0] != 0x16)
    return;

  shutCapacity = payload[1];
  shutRuntimeSeconds =
      static_cast<uint16_t>(payload[2]) |
      (static_cast<uint16_t>(payload[3]) << 8);

  shutTelemetryValid = true;

  BatteryHistoryEntry &entry = batteryHistory[batteryHistoryNext];
  entry.millisAt = millis();
  entry.capacity = shutCapacity;
  entry.runtimeSeconds = shutRuntimeSeconds;

  batteryHistoryNext =
      (batteryHistoryNext + 1) % BATTERY_HISTORY_SIZE;

  if (batteryHistoryCount < BATTERY_HISTORY_SIZE)
    batteryHistoryCount++;
}

void updateUpsStateFromPresentStatus()
{
  if (!statusGood)
  {
    upsStatus = "AUS";
    upsPower = statusAcPresent ? "Netz vorhanden" : "Netz fehlt";
    upsOperation = statusAcPresent ? "OFFLINE" : "OFFLINE / NETZAUSFALL";
    return;
  }

  upsStatus = "EIN";
  upsPower = statusAcPresent ? "Netz vorhanden" : "Netz fehlt";

  if (statusAcPresent)
    upsOperation = statusCharging ? "ONLINE / LADEN" : "ONLINE";
  else
    upsOperation = statusDischarging ? "BATTERIE" : "NETZAUSFALL";

  if (statusShutdownImminent)
    upsOperation += " / SHUTDOWN IMMINENT";
  if (statusBelowCapacityLimit)
    upsOperation += " / BATTERIE NIEDRIG";
  if (statusOverload)
    upsOperation += " / OVERLOAD";
  if (statusNeedReplacement)
    upsOperation += " / BATTERIE TAUSCHEN";
  if (statusInternalFailure)
    upsOperation += " / INTERNER FEHLER";
}

void processPresentStatusReport(const uint8_t *frame, uint8_t len)
{
  lastPresentStatusHex = frameToHex(frame, len);
  presentStatusChecksumOk = checkChecksum(frame, len);
  presentStatusCount++;

  if (!presentStatusChecksumOk || len < 6 || frame[2] != 0x02)
    return;

  applyPresentStatusPayload(&frame[2], len - 3);
  updateUpsStateFromPresentStatus();
}

void processBatteryReport(const uint8_t *frame, uint8_t len)
{
  lastBatteryReportHex = frameToHex(frame, len);
  batteryReportChecksumOk = checkChecksum(frame, len);
  batteryReportCount++;

  if (!batteryReportChecksumOk || len < 7 || frame[2] != 0x16)
    return;

  storeBatteryPayload(&frame[2], len - 3);
}

bool processShutNotifyFrame(const uint8_t *frame, uint8_t len)
{
  if (len < 4 || frame[0] != 0x85)
    return false;

  uint8_t payloadLen = frame[1] & 0x0F;

  if ((frame[1] >> 4) != payloadLen ||
      len != payloadLen + 3 ||
      !checkChecksum(frame, len))
    return false;

  switch (frame[2])
  {
    case 0x02:
      processPresentStatusReport(frame, len);
      return true;

    case 0x16:
      processBatteryReport(frame, len);
      return true;

    default:
      logPrintf("SHUT NOTIFY: unhandled HID report 0x%02X\n", frame[2]);
      return false;
  }
}

void feedMgeByte(uint8_t b)
{
  static uint8_t expectedFrameLen = 0;

  if (!rxInFrame)
  {
    if (b == 0x85)
    {
      rxInFrame = true;
      rxFrameLen = 0;
      expectedFrameLen = 0;
      rxFrame[rxFrameLen++] = b;
    }
    return;
  }

  if (rxFrameLen >= sizeof(rxFrame))
  {
    rxInFrame = false;
    rxFrameLen = 0;
    expectedFrameLen = 0;
    return;
  }

  rxFrame[rxFrameLen++] = b;

  if (rxFrameLen == 2)
  {
    uint8_t lenByte = rxFrame[1];

    if ((lenByte >> 4) != (lenByte & 0x0F))
    {
      rxInFrame = false;
      rxFrameLen = 0;
      expectedFrameLen = 0;
      return;
    }

    uint8_t payloadLen = lenByte & 0x0F;

    if (payloadLen == 0 || payloadLen > 8)
    {
      rxInFrame = false;
      rxFrameLen = 0;
      expectedFrameLen = 0;
      return;
    }

    expectedFrameLen = payloadLen + 3;
  }

  if (expectedFrameLen > 0 && rxFrameLen == expectedFrameLen)
  {
    processShutNotifyFrame(rxFrame, rxFrameLen);
    rxInFrame = false;
    rxFrameLen = 0;
    expectedFrameLen = 0;
  }
}

void readMgeSerial()
{
  bool received = false;

  while (mgeSerial.available())
  {
    uint8_t b = mgeSerial.read();

    recordRxByte(b);
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

String jsonBool(bool value)
{
  return value ? "true" : "false";
}

String jsonEscape(const String &value)
{
  String out;
  out.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); i++)
  {
    char c = value[i];

    switch (c)
    {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c >= 0x20)
          out += c;
        break;
    }
  }

  return out;
}

void handleApiStatus()
{
  String json;
  json.reserve(2200);

  json += F("{");
  json += F("\"version\":\"");
  json += APP_VERSION;
  json += F("\",");

  json += F("\"ups\":{");
  json += F("\"status\":\"");
  json += jsonEscape(upsStatus);
  json += F("\",");
  json += F("\"power\":\"");
  json += jsonEscape(upsPower);
  json += F("\",");
  json += F("\"operation\":\"");
  json += jsonEscape(upsOperation);
  json += F("\",");
  json += F("\"ac_present\":");
  json += jsonBool(statusAcPresent);
  json += F(",\"charging\":");
  json += jsonBool(statusCharging);
  json += F(",\"discharging\":");
  json += jsonBool(statusDischarging);
  json += F(",\"below_capacity_limit\":");
  json += jsonBool(statusBelowCapacityLimit);
  json += F(",\"need_replacement\":");
  json += jsonBool(statusNeedReplacement);
  json += F(",\"good\":");
  json += jsonBool(statusGood);
  json += F(",\"shutdown_imminent\":");
  json += jsonBool(statusShutdownImminent);
  json += F(",\"overload\":");
  json += jsonBool(statusOverload);
  json += F(",\"internal_failure\":");
  json += jsonBool(statusInternalFailure);
  json += F("},");

  json += F("\"telemetry\":{");
  json += F("\"valid\":");
  json += jsonBool(shutTelemetryValid);
  json += F(",\"battery_percent\":");
  if (shutTelemetryValid) json += String(shutCapacity); else json += F("null");
  json += F(",\"runtime_seconds\":");
  if (shutTelemetryValid) json += String(shutRuntimeSeconds); else json += F("null");
  json += F(",\"input_voltage\":");
  if (shutVoltageValid) json += String(shutVoltage); else json += F("null");
  json += F(",\"load_percent\":");
  if (shutVoltageValid) json += String(shutLoadPercent); else json += F("null");
  json += F("},");

  json += F("\"communication\":{");
  json += F("\"protocol\":\"SHUT\",");
  json += F("\"baud\":2400,");
  json += F("\"rx_pin\":\"D5\",");
  json += F("\"tx_pin\":\"D6\",");
  json += F("\"rx_bytes\":");
  json += String(totalRxBytes);
  json += F(",\"present_status_reports\":");
  json += String(presentStatusCount);
  json += F(",\"battery_reports\":");
  json += String(batteryReportCount);
  json += F(",\"last_rx_age_ms\":");
  if (lastRxMillis > 0)
    json += String(millis() - lastRxMillis);
  else
    json += F("null");
  json += F("},");

  json += F("\"system\":{");
  json += F("\"uptime_ms\":");
  json += String(millis());
  json += F(",\"wifi_connected\":");
  json += jsonBool(WiFi.status() == WL_CONNECTED);
  json += F(",\"ap_mode\":");
  json += jsonBool(apMode);
  json += F("}");

  json += F("}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}


bool parseHexByteString(const String &input, uint8_t *out, size_t outMax, size_t &outLen)
{
  outLen = 0;
  String token;

  for (size_t i = 0; i <= input.length(); i++)
  {
    char c = (i < input.length()) ? input[i] : ' ';

    if (c == ' ' || c == ':' || c == ',' || c == ';' || c == '-')
    {
      if (token.length() == 0)
        continue;

      if (token.length() > 2 || outLen >= outMax)
        return false;

      char *endptr = nullptr;
      long value = strtol(token.c_str(), &endptr, 16);

      if (*endptr != '\0' || value < 0 || value > 255)
        return false;

      out[outLen++] = (uint8_t)value;
      token = "";
    }
    else
    {
      if (!isxdigit((unsigned char)c))
        return false;

      token += c;
    }
  }

  return outLen > 0;
}

bool sendShutPacket(uint8_t type, const uint8_t *payload, size_t payloadLen)
{
  if (payloadLen == 0 || payloadLen > 8)
    return false;

  uint8_t lenByte = ((uint8_t)payloadLen << 4) | (uint8_t)payloadLen;
  uint8_t checksum = shutChecksum(payload, payloadLen);

  mgeSerial.write(type);
  mgeSerial.write(lenByte);
  mgeSerial.write(payload, payloadLen);
  mgeSerial.write(checksum);
  mgeSerial.flush();

  return true;
}

bool waitForShutAck(uint32_t timeoutMs)
{
  unsigned long start = millis();

  while (millis() - start < timeoutMs)
  {
    uint8_t b;

    if (!readShutByte(b, 50))
      continue;

    if (b == 0x06)
      return true;

    if (b == 0x15)
      return false;

    if (b == 0x85)
    {
      consumeAsyncShutNotify(b);
      continue;
    }
  }

  return false;
}

bool setShutReport(uint8_t reportId,
                   const uint8_t *report,
                   size_t reportLen)
{
  if (reportLen == 0 || reportLen > 8)
    return false;

  if (!syncShut())
  {
    logPrintln("SHUT SET REPORT: sync failed");
    return false;
  }

  clearShutRx();

  uint8_t setup[8] = {
    0x21,                              // bmRequestType: SET_REPORT
    0x09,                              // bRequest: SET_REPORT
    reportId,                          // wValue LSB: report ID
    0x03,                              // wValue MSB: FEATURE report
    0x00, 0x00,                        // wIndex
    (uint8_t)(reportLen & 0xFF),
    (uint8_t)(reportLen >> 8)
  };

  // First SHUT packet: request, not LAST.
  if (!sendShutPacket(0x01, setup, sizeof(setup)))
    return false;

  if (!waitForShutAck(1500))
  {
    logPrintln("SHUT SET REPORT: setup not acknowledged");
    return false;
  }

  // Second SHUT packet: actual report data, LAST request packet.
  if (!sendShutPacket(0x81, report, reportLen))
    return false;

  if (!waitForShutAck(1500))
  {
    logPrintln("SHUT SET REPORT: data not acknowledged");
    return false;
  }

  logPrintf("SHUT SET REPORT: report=0x%02X len=%u OK\n",
            reportId, (unsigned int)reportLen);

  return true;
}

bool parseHexArgByte(const String &value, uint8_t &out)
{
  if (value.length() == 0 || value.length() > 4)
    return false;

  String v = value;

  if (v.startsWith("0x") || v.startsWith("0X"))
    v = v.substring(2);

  if (v.length() == 0 || v.length() > 2)
    return false;

  for (size_t i = 0; i < v.length(); i++)
    if (!isxdigit((unsigned char)v[i]))
      return false;

  out = (uint8_t)strtoul(v.c_str(), nullptr, 16);
  return true;
}

void handleApiHidGetReport()
{
  if (!server.hasArg("id"))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing id\"}");
    return;
  }

  uint8_t reportId;

  if (!parseHexArgByte(server.arg("id"), reportId))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }

  size_t requestedLen = 8;

  if (server.hasArg("len"))
  {
    long n = server.arg("len").toInt();

    if (n < 1 || n > 8)
    {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"len must be 1..8\"}");
      return;
    }

    requestedLen = (size_t)n;
  }

  uint8_t response[8];
  size_t responseLen = 0;

  bool ok = false;

  if (syncShut())
  {
    clearShutRx();
    sendShutGetReport(reportId);
    ok = receiveShutResponse(response, sizeof(response),
                             responseLen, 3000);
  }

  String json;
  json.reserve(256);
  json += F("{\"ok\":");
  json += jsonBool(ok);
  json += F(",\"report_id\":\"0x");
  json += hexByte(reportId);
  json += F("\",\"requested_length\":");
  json += String(requestedLen);
  json += F(",\"received_length\":");
  json += String(responseLen);
  json += F(",\"hex\":\"");

  for (size_t i = 0; i < responseLen; i++)
  {
    if (i > 0) json += " ";
    json += hexByte(response[i]);
  }

  json += F("\"}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(ok ? 200 : 503, "application/json; charset=utf-8", json);
}

void handleApiHidSetReport()
{
  if (!server.hasArg("confirm") || server.arg("confirm") != "YES")
  {
    server.send(403, "application/json",
                "{\"ok\":false,\"error\":\"write requires confirm=YES\"}");
    return;
  }

  if (!server.hasArg("id") || !server.hasArg("data"))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing id or data\"}");
    return;
  }

  uint8_t reportId;

  if (!parseHexArgByte(server.arg("id"), reportId))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }

  uint8_t payload[8];
  size_t payloadLen = 0;

  if (!parseHexByteString(server.arg("data"),
                          payload, sizeof(payload), payloadLen))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid hex data; max 8 bytes\"}");
    return;
  }

  bool ok = setShutReport(reportId, payload, payloadLen);

  String json;
  json.reserve(256);
  json += F("{\"ok\":");
  json += jsonBool(ok);
  json += F(",\"report_id\":\"0x");
  json += hexByte(reportId);
  json += F("\",\"length\":");
  json += String(payloadLen);
  json += F(",\"hex\":\"");

  for (size_t i = 0; i < payloadLen; i++)
  {
    if (i > 0) json += " ";
    json += hexByte(payload[i]);
  }

  json += F("\"}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(ok ? 200 : 503, "application/json; charset=utf-8", json);
}


bool getShutFeatureReport(uint8_t reportId,
                          uint8_t *response,
                          size_t responseMax,
                          size_t &responseLen,
                          size_t requestedLen)
{
  responseLen = 0;

  if (!syncShut())
    return false;

  clearShutRx();
  sendShutGetReport(reportId);

  return receiveShutResponse(response,
                             responseMax,
                             responseLen,
                             3000);
}

bool setDelayReport(uint8_t reportId, int32_t rawValue)
{
  uint8_t payload[4];
  payload[0] = reportId;
  payload[1] = (uint8_t)(rawValue & 0xFF);
  payload[2] = (uint8_t)((rawValue >> 8) & 0xFF);
  payload[3] = (uint8_t)((rawValue >> 16) & 0xFF);

  return setShutReport(reportId, payload, sizeof(payload));
}

int32_t decodeSigned24(const uint8_t *p)
{
  int32_t value =
      (int32_t)p[0] |
      ((int32_t)p[1] << 8) |
      ((int32_t)p[2] << 16);

  if (value & 0x00800000)
    value |= 0xFF000000;

  return value;
}

void sendControlResult(bool ok,
                       const char *action,
                       long requestedSeconds,
                       int32_t rawValue)
{
  String json;
  json.reserve(320);

  json += F("{\"ok\":");
  json += jsonBool(ok);
  json += F(",\"action\":\"");
  json += action;
  json += F("\",\"requested_seconds\":");
  json += String(requestedSeconds);
  json += F(",\"raw_value\":");
  json += String(rawValue);
  json += F("}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(ok ? 200 : 503,
              "application/json; charset=utf-8",
              json);
}

void handleApiControlShutdown()
{
  if (!server.hasArg("delay"))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing delay\"}");
    return;
  }

  long seconds = server.arg("delay").toInt();

  if (seconds < 0 || seconds > 8388607L)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"delay out of range\"}");
    return;
  }

  bool ok = setDelayReport(0x0F, (int32_t)seconds);
  sendControlResult(ok, "shutdown", seconds, (int32_t)seconds);
}

void handleApiControlStartup()
{
  if (!server.hasArg("delay"))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing delay\"}");
    return;
  }

  long seconds = server.arg("delay").toInt();

  if (seconds < 0 || seconds > 83886070L)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"delay out of range\"}");
    return;
  }

  // Report 0x11 has HID Unit Exponent +1: one raw unit = 10 seconds.
  long raw = (seconds + 9L) / 10L;

  if (raw > 8388607L)
    raw = 8388607L;

  bool ok = setDelayReport(0x11, (int32_t)raw);
  sendControlResult(ok, "startup", seconds, (int32_t)raw);
}

void handleApiControlShutdownCancel()
{
  bool ok = setDelayReport(0x0F, -1);
  sendControlResult(ok, "shutdown_cancel", -1, -1);
}

void handleApiControlStartupCancel()
{
  bool ok = setDelayReport(0x11, -1);
  sendControlResult(ok, "startup_cancel", -1, -1);
}

void handleApiControlStatus()
{
  uint8_t shutdownReport[8];
  uint8_t startupReport[8];
  size_t shutdownLen = 0;
  size_t startupLen = 0;

  bool shutdownOk =
      getShutFeatureReport(0x0F,
                           shutdownReport,
                           sizeof(shutdownReport),
                           shutdownLen,
                           4);

  bool startupOk =
      getShutFeatureReport(0x11,
                           startupReport,
                           sizeof(startupReport),
                           startupLen,
                           4);

  int32_t shutdownRaw = -1;
  int32_t startupRaw = -1;

  if (shutdownOk && shutdownLen >= 4 && shutdownReport[0] == 0x0F)
    shutdownRaw = decodeSigned24(&shutdownReport[1]);

  if (startupOk && startupLen >= 4 && startupReport[0] == 0x11)
    startupRaw = decodeSigned24(&startupReport[1]);

  String json;
  json.reserve(420);

  json += F("{\"ok\":");
  json += jsonBool(shutdownOk && startupOk);

  json += F(",\"shutdown\":{\"raw\":");
  json += String(shutdownRaw);
  json += F(",\"seconds\":");
  if (shutdownRaw >= 0)
    json += String(shutdownRaw);
  else
    json += F("null");
  json += F("}");

  json += F(",\"startup\":{\"raw\":");
  json += String(startupRaw);
  json += F(",\"seconds\":");
  if (startupRaw >= 0)
    json += String((long)startupRaw * 10L);
  else
    json += F("null");
  json += F("}");

  json += F("}");

  server.sendHeader("Cache-Control", "no-store");
  server.send((shutdownOk && startupOk) ? 200 : 503,
              "application/json; charset=utf-8",
              json);
}

void handleApiHidReportDescriptor()
{
  uint8_t hidDescriptor[9];
  size_t hidDescriptorLen = 0;

  bool hidOk = getShutDescriptor(
      0x21,                 // HID descriptor
      hidDescriptor,
      sizeof(hidDescriptor),
      hidDescriptorLen,
      sizeof(hidDescriptor));

  uint16_t reportedReportLength = 0;

  if (hidOk && hidDescriptorLen >= 9)
  {
    reportedReportLength =
        static_cast<uint16_t>(hidDescriptor[7]) |
        (static_cast<uint16_t>(hidDescriptor[8]) << 8);
  }

  static const size_t MAX_REPORT_DESCRIPTOR = 768;
  uint8_t reportDescriptor[MAX_REPORT_DESCRIPTOR];
  size_t reportDescriptorLen = 0;

  bool reportOk = false;

  if (hidOk &&
      reportedReportLength > 0 &&
      reportedReportLength <= MAX_REPORT_DESCRIPTOR)
  {
    reportOk = getShutDescriptor(
        0x22,                 // HID report descriptor
        reportDescriptor,
        sizeof(reportDescriptor),
        reportDescriptorLen,
        reportedReportLength);
  }

  String json;
  json.reserve(reportDescriptorLen * 3 + 512);

  json += F("{\"ok\":");
  json += jsonBool(hidOk && reportOk);
  json += F(",\"hid_descriptor_length\":");
  json += String(hidDescriptorLen);
  json += F(",\"reported_report_descriptor_length\":");
  if (reportedReportLength > 0)
    json += String(reportedReportLength);
  else
    json += F("null");

  json += F(",\"received_report_descriptor_length\":");
  json += String(reportDescriptorLen);

  json += F(",\"hid_descriptor_hex\":\"");
  for (size_t i = 0; i < hidDescriptorLen; i++)
  {
    if (i > 0)
      json += " ";
    json += hexByte(hidDescriptor[i]);
  }
  json += F("\"");

  json += F(",\"report_descriptor_hex\":\"");
  for (size_t i = 0; i < reportDescriptorLen; i++)
  {
    if (i > 0)
      json += " ";
    json += hexByte(reportDescriptor[i]);
  }
  json += F("\"}");

  server.sendHeader("Cache-Control", "no-store");
  server.send((hidOk && reportOk) ? 200 : 503,
              "application/json; charset=utf-8",
              json);
}

void handleApiHealth()
{
  bool commOk = shutTelemetryValid || presentStatusCount > 0;

  String json;
  json.reserve(256);

  json += F("{\"ok\":");
  json += jsonBool(commOk);
  json += F(",\"version\":\"");
  json += APP_VERSION;
  json += F("\",\"uptime_ms\":");
  json += String(millis());
  json += F(",\"last_rx_age_ms\":");
  if (lastRxMillis > 0)
    json += String(millis() - lastRxMillis);
  else
    json += F("null");
  json += F("}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(commOk ? 200 : 503, "application/json; charset=utf-8", json);
}

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
    "<p><a href='/api/status'>API Status (JSON)</a></p>"
    "<p><a href='/api/health'>API Health (JSON)</a></p>"
    "<p><a href='/api/hid/report-descriptor'>HID Report Descriptor (JSON)</a></p>"
    "<p>Engineering API: /api/hid/get?id=0E&amp;len=3</p>"
    "<p>Write API: /api/hid/set?id=XX&amp;data=..&amp;confirm=YES</p>"
    "<p>Control API: /api/control/status</p>"
    "<p>POST /api/control/shutdown?delay=30</p>"
    "<p>POST /api/control/startup?delay=300</p>"
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

  body += F("<tr><td>HID Report 0x02 / PresentStatus</td><td>");
  body += String(presentStatusCount);
  body += F("</td></tr>");

  body += F("<tr><td>HID Report 0x16 / Batterie</td><td>");
  body += String(batteryReportCount);
  body += F("</td></tr>");

  body += F("</table>");
  body += F("</div>");

  // ---------------------------------------------------------------------------
  // HID Report 0x02 / PresentStatus
  // ---------------------------------------------------------------------------

  body += F("<div class='card'>");
  body += F("<h2>SHUT/HID 0x02 – PresentStatus</h2>");

  if (lastPresentStatusHex.length() == 0)
  {
    body += F("<p>Noch kein PresentStatus-Report empfangen.</p>");
  }
  else
  {
    body += F("<pre>");
    body += lastPresentStatusHex;
    body += F("</pre>");

    if (presentStatusChecksumOk)
      body += F("<p class='ok'>Checksum: OK</p>");
    else
      body += F("<p class='bad'>Checksum: FEHLER</p>");

    String statusHex = String(presentStatusBits, HEX);
    statusHex.toUpperCase();
    while (statusHex.length() < 4)
      statusHex = "0" + statusHex;

    body += F("<table>");
    body += F("<tr><td>Statusbits</td><td>0x");
    body += statusHex;
    body += F("</td></tr>");
    body += F("<tr><td>AC Present</td><td>"); body += statusAcPresent ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Charging</td><td>"); body += statusCharging ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Discharging</td><td>"); body += statusDischarging ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Below Capacity Limit</td><td>"); body += statusBelowCapacityLimit ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Need Replacement</td><td>"); body += statusNeedReplacement ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Good</td><td>"); body += statusGood ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Shutdown Imminent</td><td>"); body += statusShutdownImminent ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Overload</td><td>"); body += statusOverload ? "1" : "0"; body += F("</td></tr>");
    body += F("<tr><td>Internal Failure</td><td>"); body += statusInternalFailure ? "1" : "0"; body += F("</td></tr>");
    body += F("</table>");
  }

  body += F("</div>");

  // ---------------------------------------------------------------------------
  // SHUT telemetry
  body += F("<div class='card'><h2>SHUT – Telemetrie</h2>");
  body += F("<p><small>Firmware v");
  body += APP_VERSION;
  body += F(" | Uptime ");
  body += formatUptime();
  body += F("</small></p><table>");
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
  body += F("<p><small>SHUT-Werte werden aktiv per GET REPORT abgefragt. Report 0x02: Status; Report 0x0E: Spannung + Last; Report 0x16: Batterie + Restlaufzeit.</small></p></div>");

  // ---------------------------------------------------------------------------
  // HID Report 0x16 / Battery
  // ---------------------------------------------------------------------------

  body += F("<div class='card'>");
  body += F("<h2>SHUT/HID 0x16 – Batterie</h2>");

  if (lastBatteryReportHex.length() == 0)
  {
    body += F("<p>Noch kein Batterie-Report 0x16 empfangen.</p>");
  }
  else
  {
    body += F("<pre>");
    body += lastBatteryReportHex;
    body += F("</pre>");

    if (batteryReportChecksumOk)
      body += F("<p class='ok'>Checksum: OK</p>");
    else
      body += F("<p class='bad'>Checksum: FEHLER</p>");

    body += F("<table><tr><td>Remaining Capacity</td><td>");
    body += String(shutCapacity);
    body += F(" %</td></tr><tr><td>Run Time To Empty</td><td>");
    body += String(shutRuntimeSeconds / 60);
    body += F(":");
    if ((shutRuntimeSeconds % 60) < 10)
      body += F("0");
    body += String(shutRuntimeSeconds % 60);
    body += F(" min</td></tr></table>");
  }

  body += F("</div>");

  // ---------------------------------------------------------------------------
  // Battery history
  // ---------------------------------------------------------------------------

  body += F("<div class='card'>");
  body += F("<h2>Batterie – Verlauf</h2>");

  if (batteryHistoryCount == 0)
  {
    body += F("<p>Noch kein Verlauf vorhanden.</p>");
  }
  else
  {
    body += F("<table><tr><th>Zeit</th><th>Kapazität</th><th>Restlaufzeit</th></tr>");

    uint8_t oldestIndex =
        (batteryHistoryCount < BATTERY_HISTORY_SIZE) ? 0 : batteryHistoryNext;

    for (uint8_t i = 0; i < batteryHistoryCount; i++)
    {
      uint8_t index =
          (oldestIndex + i) % BATTERY_HISTORY_SIZE;

      const BatteryHistoryEntry &entry = batteryHistory[index];
      unsigned long ageSeconds =
          (millis() - entry.millisAt) / 1000;

      body += F("<tr><td>vor ");
      body += String(ageSeconds);
      body += F(" s</td><td>");
      body += String(entry.capacity);
      body += F(" %</td><td>");
      body += String(entry.runtimeSeconds / 60);
      body += F(":");
      if ((entry.runtimeSeconds % 60) < 10)
        body += F("0");
      body += String(entry.runtimeSeconds % 60);
      body += F(" min</td></tr>");
    }

    body += F("</table>");
  }

  body += F("<p><small>Letzte 30 gültigen HID-Reports 0x16. Verlauf liegt nur im RAM und wird beim Neustart gelöscht.</small></p>");
  body += F("</div>");

  // ---------------------------------------------------------------------------
  // Control
  // ---------------------------------------------------------------------------

  body += F("<div class='card'>");
  body += F("<h2>Steuerung</h2>");
  body += F("<p><b>Shutdown-Timer:</b> <span id='shutdownTimer'>-</span><br>");
  body += F("<b>Startup-Timer:</b> <span id='startupTimer'>-</span></p>");

  body += F("<p>");
  body += F("<button onclick=\"controlPost('/api/control/shutdown?delay=30')\">Shutdown in 30 s</button> ");
  body += F("<button onclick=\"controlPost('/api/control/shutdown/cancel')\">Shutdown abbrechen</button>");
  body += F("</p>");

  body += F("<p>");
  body += F("<button onclick=\"controlPost('/api/control/startup?delay=300')\">Startup in 300 s</button> ");
  body += F("<button onclick=\"controlPost('/api/control/startup/cancel')\">Startup abbrechen</button>");
  body += F("</p>");

  body += F("<p id='controlResult'><small>Timerstatus wird alle 5 Sekunden aktualisiert.</small></p>");

  body += F("<script>"
            "async function refreshControl(){"
            "try{"
            "const r=await fetch('/api/control/status',{cache:'no-store'});"
            "const j=await r.json();"
            "document.getElementById('shutdownTimer').textContent="
            "j.shutdown.seconds===null?'inaktiv':j.shutdown.seconds+' s';"
            "document.getElementById('startupTimer').textContent="
            "j.startup.seconds===null?'inaktiv':j.startup.seconds+' s';"
            "}catch(e){"
            "document.getElementById('controlResult').textContent='Control-Status konnte nicht geladen werden';"
            "}"
            "}"
            "async function controlPost(url){"
            "try{"
            "const r=await fetch(url,{method:'POST'});"
            "const j=await r.json();"
            "document.getElementById('controlResult').textContent="
            "j.ok?'Befehl angenommen':'Befehl fehlgeschlagen';"
            "await refreshControl();"
            "}catch(e){"
            "document.getElementById('controlResult').textContent='Fehler beim Senden';"
            "}"
            "}"
            "refreshControl();"
            "setInterval(refreshControl,5000);"
            "</script>");

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
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  File file = LittleFS.open(LOG_FILE, "r");

  if (!file)
  {
    server.send(200, "text/plain; charset=utf-8", "No log entries.");
    return;
  }

  size_t size = file.size();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");

  String header;
  header.reserve(1200);
  header += F("<!doctype html><html><head><meta charset='utf-8'>");
  header += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  header += F("<title>MGE Logs</title>");
  header += F("<style>body{font-family:Arial;max-width:1000px;margin:20px auto;padding:0 15px}");
  header += F("pre{background:#111;color:#ddd;padding:15px;white-space:pre-wrap;overflow:auto}");
  header += F("a,button{display:inline-block;padding:9px 14px;margin:4px}</style></head><body>");
  header += F("<h1>System Log</h1><p>Firmware: v");
  header += APP_VERSION;
  header += F(" | Datei: ");
  header += String(size);
  header += F(" Bytes</p>");
  header += F("<p><a href='/logs/download'>Log herunterladen</a>");
  header += F("<a href='/logs/clear' onclick=\"return confirm('Log wirklich löschen?')\">Log löschen</a>");
  header += F("<a href='/'>Zurück</a></p><pre id='log'>");

  server.sendContent(header);

  if (size > 16000)
  {
    file.seek(size - 16000, SeekSet);
    server.sendContent(F("[... log gekürzt; vollständiger Log über Download ...]\n"));
  }

  String chunk;
  chunk.reserve(768);

  while (file.available())
  {
    char c = (char)file.read();

    if (c == '&')
      chunk += F("&amp;");
    else if (c == '<')
      chunk += F("&lt;");
    else if (c == '>')
      chunk += F("&gt;");
    else
      chunk += c;

    if (chunk.length() >= 512)
    {
      server.sendContent(chunk);
      chunk = "";
      yield();
    }
  }

  file.close();

  if (chunk.length() > 0)
    server.sendContent(chunk);

  server.sendContent(
    F("</pre>"
      "<p><button type='button' onclick='location.reload()'>Refresh</button></p>"
      "</body></html>")
  );

  server.sendContent("");
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

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount failed");
  }
  else
  {
    initLogger();
  }

  logPrintln();
  logPrint("MGE UPS Controller v");
  logPrintln(APP_VERSION);
  logPrintln("----------------------");

  mgeSerial.begin(2400);

  logPrintln("MGE serial initialized: 2400 8N1");
  logPrintln("RX=D6 TX=D5");

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
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/health", HTTP_GET, handleApiHealth);
  server.on("/api/hid/report-descriptor", HTTP_GET, handleApiHidReportDescriptor);
  server.on("/api/hid/get", HTTP_GET, handleApiHidGetReport);
  server.on("/api/hid/set", HTTP_POST, handleApiHidSetReport);
  server.on("/api/control/status", HTTP_GET, handleApiControlStatus);
  server.on("/api/control/shutdown", HTTP_POST, handleApiControlShutdown);
  server.on("/api/control/startup", HTTP_POST, handleApiControlStartup);
  server.on("/api/control/shutdown/cancel", HTTP_POST, handleApiControlShutdownCancel);
  server.on("/api/control/startup/cancel", HTTP_POST, handleApiControlStartupCancel);
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