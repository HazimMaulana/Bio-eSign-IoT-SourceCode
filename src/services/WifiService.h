#pragma once

#include <Arduino.h>
#include <WiFi.h>
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
  String ipString() const;

private:
  DNSServer dnsServer_;
  Preferences preferences_;
  WebServer webServer_;
  bool routesConfigured_ = false;
  bool webServerStarted_ = false;

  bool loadCredentials(String& ssid, String& password);
  bool connectWithCredentials(const String& ssid, const String& password, UiService* ui);
  void startDashboardServer();
  void startSetupPortal(UiService* ui);
  void configurePortalRoutes();
  String portalPage(const String& message = "");
};
