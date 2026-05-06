#pragma once

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include <lvgl.h>

class UiService {
public:
  void begin();
  void tick();
  void delayWithUi(uint32_t ms);

  void showBootPanel();
  void runBootAnimation();
  void showStandbyPanel();
  void showPreparePanel();
  void setPrepareStatus(const char* wifi, const char* mqttText, const char* sensor, int progress);

  void wifiConnecting();
  void wifiConnected();
  void mqttConnecting();
  void mqttConnected();
  void sensorChecking();
  void sensorReady();
  void showSyncOnlySensorPanel(const char* sensorText = "Syncing...");

  void showReadyPanel(const String& activeClassName, const char* line2 = "Place your finger on the sensor");
  void showScanning(const char* text = "Verifying fingerprint...");
  void showRegisterPanel(const char* nama, const char* nim, const char* status);
  void showSuccess(const char* nama, const char* nim, const char* detail);
  void showFail(const char* status, const char* detail);

  MCUFRIEND_kbv& tft();
  bool ready() const;

private:
  MCUFRIEND_kbv tft_;
  uint16_t lcdId_ = 0;
  lv_display_t* lvglDisplay_ = nullptr;
  lv_color_t* lvglBuffer_ = nullptr;
  uint16_t lvglBufferLines_ = 0;
  bool uiReady_ = false;
  bool standbyModeForUiTick_ = false;
  lv_obj_t* loadingSpinners_[3] = {nullptr, nullptr, nullptr};
  lv_obj_t* standbyLabel_ = nullptr;

  static UiService* activeInstance_;
  static uint32_t lvglTickCb();
  static void lvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* pxMap);

  void initLvglForMcufriend();
  bool allocateLvglBuffer(uint16_t width);
  void flush(const lv_area_t* area, uint8_t* pxMap);
  void initEezUi();
  void forceScreenRedraw();
  void applyBootFrame(float seconds);
  void switchScreenClean(int16_t screenId);
  void configureLoadingLabels();
  void findLoadingSpinners();
  void configureStatusLabel(lv_obj_t* label, int16_t x);
  void setLoadingStatus(lv_obj_t* label, lv_obj_t* spinner, const char* text);
  bool statusIsConnected(const char* text);
  void hide(lv_obj_t* obj);
  void show(lv_obj_t* obj);
};
