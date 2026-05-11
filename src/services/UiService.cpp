#include "UiService.h"
#include "../config/AppConfig.h"
#include <esp_heap_caps.h>
#include "ui/ui.h"
#include "ui/screens.h"

extern "C" void eez_flow_set_screen(int16_t screenId, lv_scr_load_anim_t animType, uint32_t speed, uint32_t delay);
extern int16_t g_currentScreen;
namespace eez { namespace flow { extern void (*stopScriptHook)(); } }

static void stopScriptNoop() {}

UiService* UiService::activeInstance_ = nullptr;

MCUFRIEND_kbv& UiService::tft() { return tft_; }
bool UiService::ready() const { return uiReady_; }

uint32_t UiService::lvglTickCb() { return millis(); }

void UiService::lvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* pxMap) {
  if (activeInstance_) activeInstance_->flush(area, pxMap);
  lv_display_flush_ready(disp);
}

void UiService::flush(const lv_area_t* area, uint8_t* pxMap) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  uint32_t len = w * h;
  uint16_t* colorP = (uint16_t*)pxMap;

  tft_.setAddrWindow(area->x1, area->y1, area->x2, area->y2);
  uint32_t sent = 0;
  bool first = true;
  while (sent < len) {
    int16_t chunk = (int16_t)min<uint32_t>(len - sent, 16384);
    tft_.pushColors(colorP + sent, chunk, first);
    sent += chunk;
    first = false;
  }
}

bool UiService::allocateLvglBuffer(uint16_t width) {
  const uint16_t candidates[] = {320, 160, 80, 40, 20};
  for (uint8_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    uint16_t lines = candidates[i];
    uint32_t pixels = width * lines;
    uint32_t bytes = pixels * sizeof(lv_color_t);
    lvglBuffer_ = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lvglBuffer_) lvglBuffer_ = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (lvglBuffer_) {
      lvglBufferLines_ = lines;
      Serial.printf("[LVGL] Buffer OK: %u lines, %lu bytes\n", lines, (unsigned long)bytes);
      return true;
    }
  }
  return false;
}

void UiService::initLvglForMcufriend() {
  lv_init();
  lv_tick_set_cb(UiService::lvglTickCb);
  uint16_t width = tft_.width();
  uint16_t height = tft_.height();
  if (!allocateLvglBuffer(width)) {
    Serial.println("[LVGL] Semua percobaan buffer gagal");
    while (true) delay(1000);
  }
  uint32_t bufferPixels = width * lvglBufferLines_;
  lvglDisplay_ = lv_display_create(width, height);
  lv_display_set_color_format(lvglDisplay_, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(lvglDisplay_, UiService::lvglFlushCb);
  lv_display_set_buffers(lvglDisplay_, lvglBuffer_, nullptr, bufferPixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
  Serial.printf("[LVGL] Display registered: %ux%u\n", width, height);
}

void UiService::initEezUi() {
  eez::flow::stopScriptHook = stopScriptNoop;
  ui_init();
  uiReady_ = true;
  tick();
  delayWithUi(20);
}

void UiService::begin() {
  activeInstance_ = this;
  lcdId_ = AppConfig::LCD_FORCE_ID;
  tft_.begin(lcdId_);
  tft_.setRotation(1);
  tft_.fillScreen(0xFFFF);
  Serial.print("LCD ID: 0x");
  Serial.println(lcdId_, HEX);

  initLvglForMcufriend();
  initEezUi();
  runBootAnimation();
  delayWithUi(100);
  showPreparePanel();
  delayWithUi(200);
}

void UiService::tick() {
  if (!uiReady_) return;
  lv_timer_handler();
  if (!standbyModeForUiTick_) ui_tick();
}

void UiService::delayWithUi(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    tick();
    delay(5);
  }
}

void UiService::forceScreenRedraw() {
  lv_obj_t* active = lv_scr_act();
  if (!active) return;
  lv_obj_invalidate(active);
  lv_refr_now(lv_display_get_default());
  lv_timer_handler();
  lv_refr_now(lv_display_get_default());
}

void UiService::switchScreenClean(int16_t screenId) {
  if ((int16_t)(g_currentScreen + 1) == screenId) {
    forceScreenRedraw();
    return;
  }
  tft_.fillScreen(0xFFFF);
  eez_flow_set_screen(screenId, (lv_scr_load_anim_t)0, 0, 0);
  forceScreenRedraw();
}

void UiService::show(lv_obj_t* obj) { if (obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN); }
void UiService::hide(lv_obj_t* obj) { if (obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }

bool UiService::statusIsConnected(const char* text) {
  return text && (strcmp(text, "Connected") == 0 || strcmp(text, "Ready") == 0);
}

void UiService::setLoadingStatus(lv_obj_t* label, lv_obj_t* spinner, const char* text) {
  if (!label || !text) return;
  bool connected = statusIsConnected(text);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, connected ? lv_color_hex(0x22c55e) : lv_color_hex(0x008cff), LV_PART_MAIN | LV_STATE_DEFAULT);
  if (spinner) connected ? hide(spinner) : show(spinner);
}

void UiService::configureStatusLabel(lv_obj_t* label, int16_t x) {
  if (!label) return;
  lv_obj_set_pos(label, x, 261);
  lv_obj_set_width(label, 148);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
}

void UiService::findLoadingSpinners() {
  loadingSpinners_[0] = nullptr;
  loadingSpinners_[1] = nullptr;
  loadingSpinners_[2] = nullptr;
  if (!objects.loading_screen) return;

  uint32_t count = lv_obj_get_child_count(objects.loading_screen);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t* child = lv_obj_get_child(objects.loading_screen, i);
    if (!child) continue;
    int32_t x = lv_obj_get_x(child);
    int32_t y = lv_obj_get_y(child);
    int32_t w = lv_obj_get_width(child);
    int32_t h = lv_obj_get_height(child);
    if (w == 48 && h == 48 && y >= 185 && y <= 205) {
      if (x < 150) loadingSpinners_[0] = child;
      else if (x < 300) loadingSpinners_[1] = child;
      else loadingSpinners_[2] = child;
    }
  }
}

void UiService::configureLoadingLabels() {
  if (!uiReady_) return;
  configureStatusLabel(objects.obj2, 10);
  configureStatusLabel(objects.obj3, 166);
  configureStatusLabel(objects.obj4, 323);
  findLoadingSpinners();
}

void UiService::setPrepareStatus(const char* wifi, const char* mqttText, const char* sensor, int progress) {
  if (uiReady_) {
    setLoadingStatus(objects.obj2, loadingSpinners_[0], wifi);
    setLoadingStatus(objects.obj3, loadingSpinners_[1], mqttText);
    setLoadingStatus(objects.obj4, loadingSpinners_[2], sensor);
  }
  (void)progress;
  forceScreenRedraw();
}

void UiService::showBootPanel() {
  Serial.println("[UI] -> BOOTING SCREEN");
  standbyModeForUiTick_ = false;
  tft_.fillScreen(0xFFFF);
  if (objects.booting_screen) {
    g_currentScreen = SCREEN_ID_BOOTING_SCREEN - 1;
    lv_scr_load(objects.booting_screen);
  } else {
    eez_flow_set_screen(SCREEN_ID_BOOTING_SCREEN, (lv_scr_load_anim_t)0, 0, 0);
  }
  if (standbyLabel_) hide(standbyLabel_);
  applyBootFrame(0.0f);
  forceScreenRedraw();
}

void UiService::applyBootFrame(float seconds) {
  if (!uiReady_) return;

  float introT = seconds / 3.0f;
  if (introT < 0.0f) introT = 0.0f;
  if (introT > 1.0f) introT = 1.0f;

  uint8_t logoOpa = (uint8_t)(introT * 255.0f);
  int16_t logoX = (int16_t)(80 + (10.0f * introT));

  if (objects.logo_unram) {
    show(objects.logo_unram);
    lv_obj_set_pos(objects.logo_unram, logoX, 113);
    lv_obj_set_style_image_opa(objects.logo_unram, logoOpa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_opa(objects.logo_unram, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
  }

  float unramOpaF = introT;
  if (seconds >= 3.0f) {
    unramOpaF = 1.0f - ((seconds - 3.0f) / 1.5f);
    if (unramOpaF < 0.0f) unramOpaF = 0.0f;
  }
  uint8_t unramOpa = (uint8_t)(unramOpaF * 255.0f);
  int16_t unramX = (int16_t)(230 + (10.0f * introT));

  if (objects.unram_label) {
    show(objects.unram_label);
    lv_obj_set_pos(objects.unram_label, unramX, 131);
    lv_obj_set_width(objects.unram_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(objects.unram_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(objects.unram_label, unramOpa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_opa(objects.unram_label, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(objects.unram_label, "Universitas\nMataram");
  }

  float boot2OpaF = 0.0f;
  if (seconds >= 4.7f) {
    boot2OpaF = (seconds - 4.7f) / 1.8f;
    if (boot2OpaF > 1.0f) boot2OpaF = 1.0f;
  }
  uint8_t boot2Opa = (uint8_t)(boot2OpaF * 255.0f);

  if (objects.boot_2) {
    show(objects.boot_2);
    lv_obj_set_pos(objects.boot_2, 240, 118);
    lv_obj_set_style_text_opa(objects.boot_2, boot2Opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_opa(objects.boot_2, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(objects.boot_2, "Biometric\nAttendance\nSystem");
  }
}

void UiService::runBootAnimation() {
  Serial.println("[BOOT] Animation started");
  showBootPanel();
  uint32_t start = millis();
  while (millis() - start < AppConfig::EEZ_BOOT_DURATION_MS) {
    float seconds = (millis() - start) / 1000.0f;
    applyBootFrame(seconds);
    lv_obj_invalidate(objects.booting_screen);
    lv_timer_handler();
    delay(5);
  }
  Serial.printf("[BOOT] Animation finished after %lu ms\n", (unsigned long)(millis() - start));
}

void UiService::showStandbyPanel() {
  Serial.println("[UI] -> STANDBY SCREEN");
  standbyModeForUiTick_ = true;
  switchScreenClean(SCREEN_ID_BOOTING_SCREEN);
  if (uiReady_) {
    if (objects.logo_unram) {
      show(objects.logo_unram);
      lv_obj_set_pos(objects.logo_unram, 90, 113);
    }
    if (objects.unram_label) {
      show(objects.unram_label);
      lv_obj_set_pos(objects.unram_label, 220, 142);
      lv_obj_set_width(objects.unram_label, 240);
      lv_obj_set_style_text_opa(objects.unram_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.unram_label, "Standing By");
      lv_obj_move_foreground(objects.unram_label);
    }
    if (objects.boot_2) hide(objects.boot_2);
  }
  forceScreenRedraw();
}

void UiService::showPreparePanel() {
  Serial.println("[UI] -> LOADING SCREEN");
  standbyModeForUiTick_ = false;
  if (standbyLabel_) hide(standbyLabel_);
  switchScreenClean(SCREEN_ID_LOADING_SCREEN);
  tick();
  delayWithUi(50);
  configureLoadingLabels();
  if (uiReady_ && objects.obj7) lv_label_set_text(objects.obj7, "Sensor");
  setPrepareStatus("Waiting...", "Waiting...", "Waiting...", 0);
}

void UiService::wifiConnecting() { setPrepareStatus("Connecting...", "Waiting...", "Waiting...", 20); }
void UiService::wifiConnected() { setPrepareStatus("Connected", "Waiting...", "Waiting...", 40); }
void UiService::mqttConnecting() { setPrepareStatus("Connected", "Connecting...", "Waiting...", 55); }
void UiService::mqttConnected() { setPrepareStatus("Connected", "Connected", "Waiting...", 70); }
void UiService::sensorChecking() { setPrepareStatus("Connected", "Connected", "Checking...", 85); }
void UiService::sensorReady() { setPrepareStatus("Connected", "Connected", "Connected", 100); }

void UiService::showSyncOnlySensorPanel(const char* sensorText) {
  showPreparePanel();
  if (uiReady_ && objects.obj7) lv_label_set_text(objects.obj7, "Sensor");
  setPrepareStatus("Connected", "Connected", sensorText, 85);
}

void UiService::showReadyPanel(const String& activeClassName, const char* line2) {
  switchScreenClean(SCREEN_ID_SCAN);
  if (uiReady_ && objects.obj9) {
    lv_label_set_text(objects.obj9, activeClassName.length() > 0 ? activeClassName.c_str() : "Biometric System");
  }
  (void)line2;
}

void UiService::showScanning(const char* text) {
  switchScreenClean(SCREEN_ID_SCAN);
  if (uiReady_ && objects.obj9 && text) lv_label_set_text(objects.obj9, text);
}

void UiService::showRegisterPanel(const char* nama, const char* nim, const char* status) {
  switchScreenClean(SCREEN_ID_REGISTER);
  if (uiReady_) {
    if (objects.register_name) lv_label_set_text(objects.register_name, nama && nama[0] ? nama : "-");
    if (objects.register_nim) lv_label_set_text(objects.register_nim, nim && nim[0] ? nim : "-");
    if (objects.register_status) {
      lv_obj_set_pos(objects.register_status, 192, 220);
      lv_obj_set_size(objects.register_status, 265, 76);
      lv_label_set_long_mode(objects.register_status, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_font(objects.register_status, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_obj_set_style_text_line_space(objects.register_status, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_text(objects.register_status, status && status[0] ? status : "Tempelkan jari ke sensor untuk mendaftar");
    }
  }
  forceScreenRedraw();
}

void UiService::showSuccess(const char* nama, const char* nim, const char* detail) {
  switchScreenClean(SCREEN_ID_RESULT);
  if (uiReady_) {
    if (nama && objects.result_name) {
      lv_label_set_text(objects.result_name, nama);
    }
    if (nim && objects.result_nim) {
      lv_label_set_text(objects.result_nim, nim);
    }
    if (objects.obj13) lv_label_set_text(objects.obj13, "PRESENT");
  }
  (void)detail;
  forceScreenRedraw();
}

void UiService::showScanFailed() {
  Serial.println("[UI] -> FAILED RESULT SCREEN");
  standbyModeForUiTick_ = false;
  tft_.fillScreen(0xFFFF);
  if (uiReady_ && objects.failed_result) {
    lv_scr_load(objects.failed_result);
    g_currentScreen = -1;
    if (objects.obj20) lv_label_set_text(objects.obj20, "Failed");
  }
  forceScreenRedraw();
}

void UiService::showFail(const char* status, const char* detail) {
  switchScreenClean(SCREEN_ID_RESULT);
  if (uiReady_) {
    if (objects.result_name) {
      lv_label_set_text(objects.result_name, status ? status : "UNKNOWN");
    }
    if (objects.result_nim) {
      lv_label_set_text(objects.result_nim, detail ? detail : "DENIED");
    }
    if (objects.obj13) lv_label_set_text(objects.obj13, "FAILED");
  }
  forceScreenRedraw();
}
