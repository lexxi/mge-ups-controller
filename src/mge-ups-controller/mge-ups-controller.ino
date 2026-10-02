#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>

static const char *AP_PASSWORD = "mgeups123";
static const char *CONFIG_FILE = "/wifi.cfg";

ESP8266WebServer server(80);

String wifiSsid;
String wifiPassword;
bool apMode = false;

String htmlPage(const String &title, const String &body)
{
  String html;
  html.reserve(5000);
  html += F("<!doctype html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>");
  html += title;
  html += F("</title>");
  html += F("<style>body{font-family:Arial,sans-serif;max-width:760px;margin:30px auto;padding:0 18px;background:#f4f4f4;color:#222}.card{background:white;padding:20px;margin-bottom:18px;border-radius:10px;box-shadow:0 1px 5px #bbb}input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}button{padding:10px 18px;cursor:pointer}a{color:#06c}.ok{color:green}.warn{color:#b66a00}</style></head><body>");
  html += body;
  html += F("</body></html>");
  return html;
}

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

  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
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

void handleRoot()
{
  String body;
  body += F("<div class='card'><h1>MGE UPS Controller</h1>");

  if (apMode)
  {
    body += F("<p class='warn'><b>Fallback AP active</b></p>");
    body += F("<p>Connect to the controller access point and configure WLAN.</p>");
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

  body += F("<div class='card'><h2>UPS</h2><p>UPS monitoring is not implemented yet.</p><p>Serial interface planned: 2400 8N1, RX D5, TX D6.</p></div>");
  body += F("<div class='card'><a href='/config'>WLAN configuration</a></div>");

  server.send(200, "text/html", htmlPage("MGE UPS Controller", body));
}

void handleConfig()
{
  String body;
  body += F("<div class='card'><h1>WLAN configuration</h1>");
  body += F("<form method='POST' action='/save'>");
  body += F("<label>SSID</label><input name='ssid' value='");
  body += wifiSsid;
  body += F("' required>");
  body += F("<label>Password</label><input type='password' name='password' value='");
  body += wifiPassword;
  body += F("'>");
  body += F("<button type='submit'>Save and restart</button>");
  body += F("</form><p><a href='/'>Back</a></p></div>");

  server.send(200, "text/html", htmlPage("WLAN configuration", body));
}

void handleSave()
{
  if (!server.hasArg("ssid"))
  {
    server.send(400, "text/plain", "SSID missing");
    return;
  }

  wifiSsid = server.arg("ssid");
  wifiPassword = server.arg("password");
  saveConfig();

  server.send(200, "text/html", htmlPage("Saved", "<div class='card'><h1>Saved</h1><p>Configuration saved. Restarting...</p></div>"));

  delay(1000);
  ESP.restart();
}

void setup()
{
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("MGE UPS Controller v0.1");
  Serial.println("----------------------");

  if (!LittleFS.begin())
    Serial.println("LittleFS mount failed");

  if (!connectWifi())
    startAccessPoint();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);

  server.begin();
  Serial.println("HTTP server started");
}

void loop()
{
  server.handleClient();
}
