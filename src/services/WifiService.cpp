#include "WifiService.h"
#include "UiService.h"
#include "../config/AppConfig.h"

static const byte DNS_PORT = 53;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const char* WIFI_PREF_NAMESPACE = "bioesign_wifi";
static const char* WIFI_PREF_SSID = "ssid";
static const char* WIFI_PREF_PASS = "pass";

WifiService::WifiService() : webServer_(80) {}

void WifiService::begin(UiService* ui) {
  String ssid;
  String password;

  if (digitalRead(AppConfig::BTN_PIN) == LOW) {
    Serial.println("[WiFi] Setup portal forced by boot button.");
    startSetupPortal(ui);
    return;
  }

  if (!loadCredentials(ssid, password)) {
    Serial.println("[WiFi] No saved credentials. Starting setup portal.");
    startSetupPortal(ui);
    return;
  }

  if (connectWithCredentials(ssid, password, ui)) {
    startDashboardServer();
    return;
  }

  Serial.println("[WiFi] Saved credentials failed. Starting setup portal.");
  startSetupPortal(ui);
}

bool WifiService::loadCredentials(String& ssid, String& password) {
  preferences_.begin(WIFI_PREF_NAMESPACE, true);
  ssid = preferences_.getString(WIFI_PREF_SSID, "");
  password = preferences_.getString(WIFI_PREF_PASS, "");
  preferences_.end();

  ssid.trim();
  return ssid.length() > 0;
}

bool WifiService::connectWithCredentials(const String& ssid, const String& password, UiService* ui) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("WiFi connecting to ");
  Serial.print(ssid);
  if (ui) ui->wifiConnecting();

  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print(".");
    if (ui) ui->delayWithUi(250);
    else delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connect failed");
    return false;
  }

  Serial.println("\nWiFi OK");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());
  if (ui) ui->wifiConnected();
  return true;
}

void WifiService::startDashboardServer() {
  configurePortalRoutes();

  if (!webServerStarted_) {
    webServer_.begin();
    webServerStarted_ = true;
  }

  Serial.print("[WiFi] Dashboard URL: http://");
  Serial.println(WiFi.localIP());
}

void WifiService::startSetupPortal(UiService* ui) {
  WiFi.mode(WIFI_AP);

  String apSsid = "BioESign-" + AppConfig::macAddressCompact();
  bool apOk = WiFi.softAP(apSsid.c_str());
  IPAddress apIp = WiFi.softAPIP();

  Serial.println(apOk ? "[WiFi] Setup AP started" : "[WiFi] Setup AP failed");
  Serial.print("[WiFi] AP SSID: ");
  Serial.println(apSsid);
  Serial.print("[WiFi] Setup URL: http://");
  Serial.println(apIp);

  if (ui) {
    ui->showFail("WiFi Setup", "Connect to setup AP");
  }

  dnsServer_.start(DNS_PORT, "*", apIp);
  configurePortalRoutes();
  if (!webServerStarted_) {
    webServer_.begin();
    webServerStarted_ = true;
  }

  while (true) {
    dnsServer_.processNextRequest();
    webServer_.handleClient();
    if (ui) ui->delayWithUi(10);
    else delay(10);
  }
}

void WifiService::configurePortalRoutes() {
  if (routesConfigured_) return;
  routesConfigured_ = true;

  webServer_.on("/", HTTP_GET, [this]() {
    webServer_.send(200, "text/html", portalPage());
  });

  webServer_.on("/save", HTTP_POST, [this]() {
    String ssid = webServer_.arg("ssid");
    String password = webServer_.arg("password");
    ssid.trim();

    if (ssid.length() == 0) {
      webServer_.send(400, "text/html", portalPage("SSID wajib diisi."));
      return;
    }

    preferences_.begin(WIFI_PREF_NAMESPACE, false);
    preferences_.putString(WIFI_PREF_SSID, ssid);
    preferences_.putString(WIFI_PREF_PASS, password);
    preferences_.end();

    webServer_.send(200, "text/html", portalPage("WiFi disimpan. ESP restart..."));
    delay(800);
    ESP.restart();
  });

  webServer_.on("/clear", HTTP_POST, [this]() {
    preferences_.begin(WIFI_PREF_NAMESPACE, false);
    preferences_.clear();
    preferences_.end();

    webServer_.send(200, "text/html", portalPage("Konfigurasi WiFi dihapus. ESP restart..."));
    delay(800);
    ESP.restart();
  });

  webServer_.onNotFound([this]() {
    webServer_.sendHeader("Location", "/", true);
    webServer_.send(302, "text/plain", "");
  });
}

String WifiService::portalPage(const String& message) {
  String html;
  html.reserve(2600);
  html += F("<!doctype html><html lang=\"id\"><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>Bio E-Sign WiFi Setup</title>");
  html += F("<style>");
  html += F("body{margin:0;font-family:Arial,sans-serif;background:#f6f8fb;color:#172033}");
  html += F("main{max-width:420px;margin:0 auto;padding:28px 18px}");
  html += F("h1{font-size:24px;margin:0 0 8px}p{line-height:1.45;color:#526071}");
  html += F("form{background:#fff;border:1px solid #d9e1ec;border-radius:8px;padding:18px;margin-top:16px}");
  html += F("label{display:block;font-size:13px;font-weight:700;margin:14px 0 6px}");
  html += F("input{width:100%;box-sizing:border-box;border:1px solid #b9c4d0;border-radius:6px;padding:11px;font-size:16px}");
  html += F("button{width:100%;border:0;border-radius:6px;background:#1167b1;color:#fff;font-size:16px;font-weight:700;padding:12px;margin-top:18px}");
  html += F(".muted{font-size:13px}.msg{padding:10px;border-radius:6px;background:#e8f4ff;color:#0f4c81}");
  html += F(".danger{background:#fff;border:1px solid #d9e1ec;color:#ad1f1f}");
  html += F("</style></head><body><main>");
  html += F("<h1>Bio E-Sign WiFi Setup</h1>");
  html += F("<p class=\"muted\">Device ID: ");
  html += AppConfig::deviceId();
  html += F("</p>");
  if (message.length() > 0) {
    html += F("<p class=\"msg\">");
    html += message;
    html += F("</p>");
  }
  html += F("<form method=\"post\" action=\"/save\">");
  html += F("<label for=\"ssid\">Nama WiFi</label>");
  html += F("<input id=\"ssid\" name=\"ssid\" autocomplete=\"off\" required>");
  html += F("<label for=\"password\">Password WiFi</label>");
  html += F("<input id=\"password\" name=\"password\" type=\"password\">");
  html += F("<button type=\"submit\">Simpan dan Restart</button>");
  html += F("</form>");
  html += F("<form method=\"post\" action=\"/clear\">");
  html += F("<button class=\"danger\" type=\"submit\">Hapus Konfigurasi</button>");
  html += F("</form>");
  html += F("<p class=\"muted\">Jika memakai AP setup, buka 192.168.4.1. Jika device sudah online, buka IP device dari dashboard/status MQTT.</p>");
  html += F("</main></body></html>");
  return html;
}

void WifiService::ensureConnected(UiService* ui) {
  if (WiFi.status() != WL_CONNECTED) {
    begin(ui);
    return;
  }

  if (!webServerStarted_) startDashboardServer();
  webServer_.handleClient();
}

bool WifiService::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String WifiService::ipString() const {
  return WiFi.localIP().toString();
}
