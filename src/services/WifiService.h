#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>

class UiService;

class WifiService {
public:
  WifiService();

  void begin(UiService* ui = nullptr);
  void ensureConnected(UiService* ui = nullptr);
  bool isConnected() const;
  bool isOnline() const;
  String ipString() const;

private:
  enum class ConnectionState : uint8_t {
    Idle,
    Connecting,
    Online,
    SsidMissing,
    NoInternet,
    SetupAvailable,
  };

  DNSServer dnsServer_;
  Preferences preferences_;
  WebServer webServer_;
  bool routesConfigured_ = false;
  bool webServerStarted_ = false;
  bool portalActive_ = false;
  bool otaStarted_ = false;
  String cachedSsid_;
  String cachedPassword_;
  ConnectionState state_ = ConnectionState::Idle;
  uint32_t connectionStartedAtMs_ = 0;
  uint32_t lastRetryAtMs_ = 0;
  uint32_t lastInternetProbeAtMs_ = 0;

  bool loadCredentials(String& ssid, String& password);
  void startStationAttempt(const String& ssid, const String& password, UiService* ui);
  bool pollStationAttempt(UiService* ui);
  bool isSsidVisible(const String& ssid);
  bool probeBackendReachable();
  void startOtaService();
  void startDashboardServer();
  void startSetupPortal(UiService* ui);
  void servicePortal();
  void configurePortalRoutes();
  String portalPage(const String& message = "");
  String updatePage(const String& message = "");
  bool requireOtaAuth();
};
