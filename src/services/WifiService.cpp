#include "WifiService.h"
#include "UiService.h"
#include "../config/AppConfig.h"

void WifiService::begin(UiService* ui) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(AppConfig::WIFI_SSID, AppConfig::WIFI_PASS);
  Serial.print("WiFi connecting");
  if (ui) ui->wifiConnecting();

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    if (ui) ui->delayWithUi(250);
    else delay(250);
  }

  Serial.println("\nWiFi OK");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());
  if (ui) ui->wifiConnected();
}

void WifiService::ensureConnected(UiService* ui) {
  if (WiFi.status() != WL_CONNECTED) begin(ui);
}

bool WifiService::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String WifiService::ipString() const {
  return WiFi.localIP().toString();
}
