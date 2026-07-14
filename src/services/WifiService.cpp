#include "WifiService.h"
#include "UiService.h"
#include "../config/AppConfig.h"

static const byte DNS_PORT = 53;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
static const uint32_t WIFI_INTERNET_PROBE_INTERVAL_MS = 12000;
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
    cachedSsid_.clear();
    cachedPassword_.clear();
    startSetupPortal(ui);
    return;
  }

  cachedSsid_ = ssid;
  cachedPassword_ = password;
  startStationAttempt(cachedSsid_, cachedPassword_, ui);
}

bool WifiService::loadCredentials(String& ssid, String& password) {
  preferences_.begin(WIFI_PREF_NAMESPACE, true);
  ssid = preferences_.getString(WIFI_PREF_SSID, "");
  password = preferences_.getString(WIFI_PREF_PASS, "");
  preferences_.end();

  ssid.trim();
  return ssid.length() > 0;
}

void WifiService::startStationAttempt(const String& ssid, const String& password, UiService* ui) {
  if (ssid.length() == 0) return;

  if (portalActive_) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }

  WiFi.setHostname(AppConfig::deviceId().c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  connectionStartedAtMs_ = millis();
  lastRetryAtMs_ = connectionStartedAtMs_;
  lastInternetProbeAtMs_ = 0;
  state_ = ConnectionState::Connecting;

  Serial.print("WiFi connecting to ");
  Serial.println(ssid);
  if (ui) ui->wifiConnecting();
}

bool WifiService::pollStationAttempt(UiService* ui) {
  if (cachedSsid_.length() == 0) return false;

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return true;
  }

  if (status == WL_NO_SSID_AVAIL) {
    state_ = ConnectionState::SsidMissing;
    if (ui) ui->wifiSsidMissing();
    if (!portalActive_) {
      startSetupPortal(ui);
    }
    return false;
  }

  uint32_t elapsed = millis() - connectionStartedAtMs_;
  if (elapsed < WIFI_CONNECT_TIMEOUT_MS) {
    return false;
  }

  if (!isSsidVisible(cachedSsid_)) {
    state_ = ConnectionState::SsidMissing;
    if (ui) ui->wifiSsidMissing();
    if (!portalActive_) {
      startSetupPortal(ui);
    }
    return false;
  }

  state_ = ConnectionState::SetupAvailable;
  if (ui) ui->wifiSetupAvailable();
  if (!portalActive_) {
    startSetupPortal(ui);
  }
  return false;
}

bool WifiService::isSsidVisible(const String& ssid) {
  if (ssid.length() == 0) return false;

  int networkCount = WiFi.scanNetworks(false, true);
  bool visible = false;
  for (int i = 0; i < networkCount; ++i) {
    if (WiFi.SSID(i) == ssid) {
      visible = true;
      break;
    }
  }
  WiFi.scanDelete();
  return visible;
}

bool WifiService::probeBackendReachable() {
  WiFiClient client;
  client.setTimeout(1500);
  bool ok = client.connect(AppConfig::MQTT_HOST, AppConfig::MQTT_PORT);
  client.stop();
  return ok;
}

void WifiService::startOtaService() {
  if (otaStarted_) return;

  ArduinoOTA.setHostname(AppConfig::deviceId().c_str());
  ArduinoOTA.setPassword(AppConfig::OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Update started");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Update finished");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100U) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]\n", error);
  });

  ArduinoOTA.begin();
  otaStarted_ = true;
  Serial.print("[OTA] Ready on host: ");
  Serial.println(AppConfig::deviceId());
}

void WifiService::startDashboardServer() {
  configurePortalRoutes();

  if (!webServerStarted_) {
    webServer_.begin();
    webServerStarted_ = true;
  }

  startOtaService();

  Serial.print("[WiFi] Dashboard URL: http://");
  Serial.println(WiFi.localIP());
}

void WifiService::startSetupPortal(UiService* ui) {
  portalActive_ = true;
  if (cachedSsid_.length() > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(cachedSsid_.c_str(), cachedPassword_.c_str());
      connectionStartedAtMs_ = millis();
      lastRetryAtMs_ = connectionStartedAtMs_;
      state_ = ConnectionState::Connecting;
    }
  } else {
    WiFi.mode(WIFI_AP);
  }

  String apSsid = "BioESign-" + AppConfig::macAddressCompact();
  bool apOk = WiFi.softAP(apSsid.c_str());
  IPAddress apIp = WiFi.softAPIP();

  Serial.println(apOk ? "[WiFi] Setup AP started" : "[WiFi] Setup AP failed");
  Serial.print("[WiFi] AP SSID: ");
  Serial.println(apSsid);
  Serial.print("[WiFi] Setup URL: http://");
  Serial.println(apIp);

  if (ui) {
    if (state_ == ConnectionState::SsidMissing) {
      ui->wifiSsidMissing();
    } else if (state_ == ConnectionState::NoInternet) {
      ui->wifiNoInternet();
    } else {
      ui->wifiSetupAvailable();
    }
  }

  dnsServer_.start(DNS_PORT, "*", apIp);
  configurePortalRoutes();
  if (!webServerStarted_) {
    webServer_.begin();
    webServerStarted_ = true;
  }
  startOtaService();
  state_ = ConnectionState::SetupAvailable;
}

void WifiService::servicePortal() {
  if (!webServerStarted_) return;
  dnsServer_.processNextRequest();
  webServer_.handleClient();
  if (otaStarted_) ArduinoOTA.handle();
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
  servicePortal();

  if (WiFi.status() == WL_CONNECTED) {
    if (state_ != ConnectionState::Online) {
      if (millis() - lastInternetProbeAtMs_ >= WIFI_INTERNET_PROBE_INTERVAL_MS) {
        lastInternetProbeAtMs_ = millis();
        if (probeBackendReachable()) {
          state_ = ConnectionState::Online;
          if (ui) ui->wifiConnected();
          if (!webServerStarted_) startDashboardServer();
          startOtaService();
          Serial.println("[WiFi] Backend reachable");
        } else {
          state_ = ConnectionState::NoInternet;
          if (ui) ui->wifiNoInternet();
          if (!portalActive_) startSetupPortal(ui);
          Serial.println("[WiFi] Connected, but backend is unreachable");
        }
      }
    }

    return;
  }

  if (cachedSsid_.length() == 0) {
    if (!portalActive_) startSetupPortal(ui);
    return;
  }

  if (!portalActive_ && state_ != ConnectionState::Connecting) {
    startSetupPortal(ui);
  }

  if (state_ == ConnectionState::Connecting) {
    if (pollStationAttempt(ui)) {
      lastInternetProbeAtMs_ = 0;
    }
    return;
  }

  if (millis() - lastRetryAtMs_ < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastRetryAtMs_ = millis();
  state_ = ConnectionState::Connecting;
  startStationAttempt(cachedSsid_, cachedPassword_, ui);
}

bool WifiService::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool WifiService::isOnline() const {
  return state_ == ConnectionState::Online;
}

String WifiService::ipString() const {
  return WiFi.localIP().toString();
}
