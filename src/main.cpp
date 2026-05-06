#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/base64.h>
#include <MCUFRIEND_kbv.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "ui/ui.h"

// ================== WIFI & MQTT ==================
// Diambil dari kode MQTT yang kamu kirim. Ubah bagian ini saja kalau SSID/broker/topik berubah.
const char* WIFI_SSID = "JImsExt";
const char* WIFI_PASS = "zamil@02052009";

const char* MQTT_HOST = "172.235.244.139";
const uint16_t MQTT_PORT = 1883;

const char* MQTT_USERNAME = "backend_service";
const char* MQTT_PASSWORD = "passwordbe";

const char* DEVICE_ID = "ESP32S3_R302_01";

const char* TOPIC_PRESENSI          = "presence/presensi";
const char* TOPIC_MAHASISWA         = "presence/mahasiswa/catalog";
const char* TOPIC_REGISTRASI        = "presence/mahasiswa/registrasi";
const char* TOPIC_REGISTER_ACK      = "presence/mahasiswa/templates/register_ack/ESP32S3_R302_01";
const char* TOPIC_TEMPLATE_REQ      = "presence/mahasiswa/templates/request";
const char* TOPIC_TEMPLATE_MANIFEST = "presence/mahasiswa/templates/manifest";
const char* TOPIC_TEMPLATE_CHUNK    = "presence/mahasiswa/templates/chunk";
const char* TOPIC_TEMPLATE_ACK      = "presence/mahasiswa/templates/ack";

// Topik tambahan opsional untuk heartbeat/status dan command.
// Kalau backend kamu belum memakainya, biarkan saja.
const char* TOPIC_STATUS            = "presence/device/ESP32S3_R302_01/status";
const char* TOPIC_SESSION_CLEAR     = "presence/mahasiswa/session/clear";
const char* TOPIC_COMMAND           = "presence/device/ESP32S3_R302_01/command";
const char* TOPIC_ATTENDANCE        = "presence/attendance";

// ================== FINGERPRINT (R302) ==================
#define FP_RX 41  // ESP32 RX <- R302 TXD
#define FP_TX 40  // ESP32 TX -> R302 RXD
static const uint32_t FP_BAUD = 57600;

MCUFRIEND_kbv tft;

#define LCD_FORCE_ID 0x6814
uint16_t lcdID = LCD_FORCE_ID;



// ================== R302 TOUCH ==================
// Sesuai wiring terakhir yang kamu pakai: TOUCH R302 -> GPIO21.
#define TOUCH_PIN 21
// Banyak modul TOUCH output LOW saat disentuh. Kalau terbalik, ubah ke HIGH.
static const uint8_t TOUCH_ACTIVE_LEVEL = LOW;
static const uint32_t TOUCH_DEBOUNCE_MS = 30;
static const uint32_t TOUCH_COOLDOWN_MS = 1000;  // cegah spam saat jari tetap nempel

// ===== Hybrid mode: TOUCH trigger + fallback polling =====
static const uint32_t FP_FALLBACK_INTERVAL_MS = 120;   // interval polling getImage
static const uint32_t FP_SCAN_COOLDOWN_MS     = 1200;  // jeda setelah satu attempt (sukses/gagal)
static uint32_t lastAttemptMs = 0;
static uint32_t lastFallbackPollMs = 0;



// ================== BUZZER (PASSIVE) ==================
#define BUZZER_PIN 42
static const int BUZZER_CH = 0;
static const int BUZZER_RES_BITS = 10;
static const int BUZZER_BASE_FREQ = 2000;

// ================== MQTT client ==================
WiFiClient espClient;
PubSubClient mqtt(espClient);
static const uint16_t MQTT_BUFFER_SIZE = 6144;
static bool mqttSendingEnabled = true;

// ================== Fingerprint lib ==================
HardwareSerial FPSerial(1);
Adafruit_Fingerprint finger(&FPSerial);


// ================== State ==================
uint16_t nextID = 1;
bool syncDone = false;
bool syncExpected = false;
bool standbyMode = false;
bool registrationRequested = false;
bool registrationInProgress = false;
bool registrationReturnToStandby = false;
String activeClassCode = "";
String activeClassName = "";

static const size_t MAX_MAHASISWA = 80;
// BUTTON / input
#define BTN_PIN 39
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t SERIAL_INPUT_TIMEOUT_MS = 120000;
static const bool CLEAR_SENSOR_TEMPLATES_ON_BOOT = false;
static const uint8_t ENROLL_RETRY_PER_SLOT = 3;

// lowered these to reduce DRAM usage on ESP32-S3
static const size_t MAX_SYNC_TEMPLATES = 50;
static const size_t MAX_CHUNKS_PER_TEMPLATE = 16;
static const size_t MAX_TEMPLATE_BYTES = 2048;
static const size_t MAX_TEMPLATE_BASE64_CHARS = ((MAX_TEMPLATE_BYTES + 2) / 3) * 4;

struct Mahasiswa {
  String nama;
  String nim;
  int32_t fingerId[3];
};

struct TemplateSyncItem {
  bool used;
  bool restored;
  bool failed;
  String templateUid;
  uint16_t fingerprintId;
  uint16_t chunkTotal;
  uint16_t chunkReceived;
  bool gotChunk[MAX_CHUNKS_PER_TEMPLATE];
  String chunkData[MAX_CHUNKS_PER_TEMPLATE];
};

struct TemplateSyncState {
  bool inProgress;
  bool manifestReceived;
  String syncId;
  uint16_t totalTemplates;
  uint16_t successCount;
  uint16_t failedCount;
};

Mahasiswa mahasiswaList[MAX_MAHASISWA];
size_t mahasiswaCount = 0;
bool mahasiswaDataReceived = false;
Mahasiswa pendingRegistration;
uint8_t pendingRegistrationSlot = 1;
TemplateSyncItem syncItems[MAX_SYNC_TEMPLATES];
TemplateSyncState syncState = {false, false, "", 0, 0, 0};

// ================== MCUFRIEND + LVGL + EEZ ==================
// The generated UI provides screens/objects in `src/ui` (see screens.h).
// We'll operate at screen level: booting -> loading -> scan -> result.
#include "ui/screens.h"

// use public eez flow API to switch screens
// eez_flow_set_screen is declared in generated `src/ui/eez-flow.h`
// and calls the appropriate internal hook.
extern "C" void eez_flow_set_screen(int16_t screenId, lv_scr_load_anim_t animType, uint32_t speed, uint32_t delay);
extern int16_t g_currentScreen;
namespace eez {
namespace flow {
extern void (*stopScriptHook)();
}
}

static void stopScriptNoop() {
  // noop: prevent EEZ stopScript assert in embedded build
}

void forceScreenRedraw() {
  lv_obj_t *active = lv_scr_act();
  if (!active) return;

  lv_obj_invalidate(active);
  lv_refr_now(lv_display_get_default());
  lv_timer_handler();
  lv_refr_now(lv_display_get_default());
}

// Wrapper untuk transisi screen yang clean.
// MCUFRIEND + LVGL partial refresh can leave pixels from the previous screen
// after loading an already-created EEZ screen, so clear the physical LCD first.
void switchScreenClean(int16_t screenId) {
  if ((int16_t)(g_currentScreen + 1) == screenId) {
    forceScreenRedraw();
    return;
  }

  tft.fillScreen(0xFFFF);
  eez_flow_set_screen(screenId, (lv_scr_load_anim_t)0, 0, 0);
  forceScreenRedraw();
}
// (constants already defined above)

static lv_display_t *lvglDisplay = nullptr;
static lv_color_t *lvglBuffer = nullptr;
static uint16_t lvglBufferLines = 0;
static bool uiReady = false;
static lv_obj_t *loadingSpinners[3] = {nullptr, nullptr, nullptr};
static lv_obj_t *standbyLabel = nullptr;

#define LVGL_SWAP_RGB565_BYTES 0
#define EEZ_BOOT_DURATION_MS 7000

static uint32_t lvgl_tick_cb() {
  return millis();
}

#if LVGL_SWAP_RGB565_BYTES
static void swap_rgb565_bytes(uint16_t *buf, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    buf[i] = (buf[i] >> 8) | (buf[i] << 8);
  }
}
#endif

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  uint32_t len = w * h;
  uint16_t *color_p = (uint16_t *)px_map;
#if LVGL_SWAP_RGB565_BYTES
  swap_rgb565_bytes(color_p, len);
#endif
  tft.setAddrWindow(area->x1, area->y1, area->x2, area->y2);

  // MCUFRIEND_kbv pushColors() takes int16_t pixel count, so large LVGL
  // flushes must be split or the length overflows and only part of the UI draws.
  uint32_t sent = 0;
  bool first = true;
  while (sent < len) {
    int16_t chunk = (int16_t)min<uint32_t>(len - sent, 16384);
    tft.pushColors(color_p + sent, chunk, first);
    sent += chunk;
    first = false;
  }

  lv_display_flush_ready(disp);
}

bool allocateLvglBuffer(uint16_t width) {
  const uint16_t candidates[] = {320, 160, 80, 40, 20};
  for (uint8_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    uint16_t lines = candidates[i];
    uint32_t pixels = width * lines;
    uint32_t bytes = pixels * sizeof(lv_color_t);
    lvglBuffer = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lvglBuffer) lvglBuffer = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (lvglBuffer) {
      lvglBufferLines = lines;
      Serial.printf("[LVGL] Buffer OK: %u lines, %lu bytes\n", lines, (unsigned long)bytes);
      return true;
    }
  }
  return false;
}

void uiTick() {
  if (!uiReady) return;
  lv_timer_handler();
  if (!standbyMode) ui_tick();
}

void delayWithUi(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    uiTick();
    delay(5);
  }
}

void initLvglForMcufriend() {
  lv_init();
  lv_tick_set_cb(lvgl_tick_cb);
  uint16_t width = tft.width();
  uint16_t height = tft.height();
  if (!allocateLvglBuffer(width)) {
    Serial.println("[LVGL] Semua percobaan buffer gagal");
    while (true) delay(1000);
  }
  uint32_t bufferPixels = width * lvglBufferLines;
  lvglDisplay = lv_display_create(width, height);
  lv_display_set_color_format(lvglDisplay, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(lvglDisplay, lvgl_flush_cb);
  lv_display_set_buffers(lvglDisplay, lvglBuffer, nullptr, bufferPixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
  Serial.printf("[LVGL] Display registered: %ux%u\n", width, height);
}

void uiShow(lv_obj_t *obj) { lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN); }
void uiHide(lv_obj_t *obj) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }

void styleSmallSpinner(lv_obj_t *spinner) {
  lv_obj_set_style_arc_width(spinner, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(spinner, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(spinner, LV_OPA_COVER, LV_PART_INDICATOR);
}

void uiSetProgress(int value) {
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  // generated UI doesn't have a progress bar exposed; ignore for now
  (void)value;
  uiTick();
}

void uiConfigureStatusLabel(lv_obj_t *label, int16_t x) {
  if (!label) return;
  lv_obj_set_pos(label, x, 261);
  lv_obj_set_width(label, 148);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
}

void uiFindLoadingSpinners() {
  loadingSpinners[0] = nullptr;
  loadingSpinners[1] = nullptr;
  loadingSpinners[2] = nullptr;
  if (!objects.loading_screen) return;

  uint32_t count = lv_obj_get_child_count(objects.loading_screen);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *child = lv_obj_get_child(objects.loading_screen, i);
    if (!child) continue;

    int32_t x = lv_obj_get_x(child);
    int32_t y = lv_obj_get_y(child);
    int32_t w = lv_obj_get_width(child);
    int32_t h = lv_obj_get_height(child);

    if (w == 48 && h == 48 && y >= 185 && y <= 205) {
      if (x < 150) loadingSpinners[0] = child;
      else if (x < 300) loadingSpinners[1] = child;
      else loadingSpinners[2] = child;
    }
  }
}

void uiConfigureLoadingLabels() {
  if (!uiReady) return;
  uiConfigureStatusLabel(objects.obj2, 10);
  uiConfigureStatusLabel(objects.obj3, 166);
  uiConfigureStatusLabel(objects.obj4, 323);
  uiFindLoadingSpinners();
}

bool uiStatusIsConnected(const char *text) {
  return text && (strcmp(text, "Connected") == 0 || strcmp(text, "Ready") == 0);
}

void uiSetLoadingStatus(lv_obj_t *label, lv_obj_t *spinner, const char *text) {
  if (!label || !text) return;

  bool connected = uiStatusIsConnected(text);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(
    label,
    connected ? lv_color_hex(0x22c55e) : lv_color_hex(0x008cff),
    LV_PART_MAIN | LV_STATE_DEFAULT
  );

  if (spinner) {
    if (connected) uiHide(spinner);
    else uiShow(spinner);
  }
}

void uiSetPrepareStatus(const char *wifi, const char *mqttText, const char *sensor, int progress) {
  // Map to generated loading screen labels: obj2=obj wifi status, obj3=mqtt, obj4=sensor
  if (uiReady) {
    uiSetLoadingStatus(objects.obj2, loadingSpinners[0], wifi);
    uiSetLoadingStatus(objects.obj3, loadingSpinners[1], mqttText);
    uiSetLoadingStatus(objects.obj4, loadingSpinners[2], sensor);
  }
  (void)progress;
  forceScreenRedraw();
}

void uiShowBootPanel() {
  Serial.println("[UI] -> BOOTING SCREEN");
  switchScreenClean(SCREEN_ID_BOOTING_SCREEN);
  if (standbyLabel) lv_obj_add_flag(standbyLabel, LV_OBJ_FLAG_HIDDEN);
  uiTick();
}

void uiShowStandbyPanel() {
  Serial.println("[UI] -> STANDBY SCREEN");
  switchScreenClean(SCREEN_ID_BOOTING_SCREEN);

  if (uiReady) {
    if (objects.logo_unram) {
      lv_obj_clear_flag(objects.logo_unram, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(objects.logo_unram, 90, 113);
      lv_obj_set_style_image_opa(objects.logo_unram, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (objects.unram_label) {
      lv_obj_clear_flag(objects.unram_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(objects.unram_label, 220, 142);
      lv_obj_set_width(objects.unram_label, 240);
      lv_obj_set_style_text_align(objects.unram_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      lv_obj_set_style_text_color(objects.unram_label, lv_color_hex(0x111827), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_obj_set_style_text_opa(objects.unram_label, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_long_mode(objects.unram_label, LV_LABEL_LONG_CLIP);
      lv_label_set_text(objects.unram_label, "Standing By");
      lv_obj_move_foreground(objects.unram_label);
    }
    if (objects.boot_2) {
      lv_obj_add_flag(objects.boot_2, LV_OBJ_FLAG_HIDDEN);
    }
    if (objects.booting_screen) {
      if (!standbyLabel) {
        standbyLabel = lv_label_create(objects.booting_screen);
        lv_obj_set_style_text_font(standbyLabel, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
      } else {
        lv_obj_set_parent(standbyLabel, objects.booting_screen);
      }
      lv_obj_clear_flag(standbyLabel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(standbyLabel, 220, 142);
      lv_obj_set_width(standbyLabel, 240);
      lv_obj_set_style_text_align(standbyLabel, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_obj_set_style_text_color(standbyLabel, lv_color_hex(0x111827), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_obj_set_style_text_opa(standbyLabel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_label_set_long_mode(standbyLabel, LV_LABEL_LONG_CLIP);
      lv_label_set_text(standbyLabel, "Standing By");
      lv_obj_move_foreground(standbyLabel);
    }
  }

  forceScreenRedraw();
}

void uiShowPreparePanel() {
  Serial.println("[UI] -> LOADING SCREEN");
  if (standbyLabel) lv_obj_add_flag(standbyLabel, LV_OBJ_FLAG_HIDDEN);
  switchScreenClean(SCREEN_ID_LOADING_SCREEN);
  uiTick();
  delayWithUi(50);
  uiConfigureLoadingLabels();
  if (uiReady && objects.obj7) {
    lv_label_set_text(objects.obj7, "Sensor");
  }

  uiSetPrepareStatus(
    "Waiting...",
    "Waiting...",
    "Waiting...",
    0
  );
}

void uiWifiConnecting() {
  uiSetPrepareStatus("Connecting...", "Waiting...", "Waiting...", 20);
}

void uiWifiConnected() {
  uiSetPrepareStatus("Connected", "Waiting...", "Waiting...", 40);
}

void uiMqttConnecting() {
  uiSetPrepareStatus("Connected", "Connecting...", "Waiting...", 55);
}

void uiMqttConnected() {
  uiSetPrepareStatus("Connected", "Connected", "Waiting...", 70);
}

void uiSensorChecking() {
  uiSetPrepareStatus("Connected", "Connected", "Checking...", 85);
}

void uiSensorReady() {
  uiSetPrepareStatus("Connected", "Connected", "Connected", 100);
}

void uiShowSyncOnlySensorPanel(const char *sensorText = "Syncing...") {
  standbyMode = false;
  uiShowPreparePanel();
  if (uiReady && objects.obj7) {
    lv_label_set_text(objects.obj7, "Sensor");
  }
  uiSetPrepareStatus("Connected", "Connected", sensorText, 85);
}

void uiShowReadyPanel(const char *line2 = "Place your finger on the sensor") {
  switchScreenClean(SCREEN_ID_SCAN);
  if (uiReady && objects.obj9) {
    lv_label_set_text(
      objects.obj9,
      activeClassName.length() > 0 ? activeClassName.c_str() : "Biometric System"
    );
  }
}

void uiShowScanning(const char *text = "Verifying fingerprint...") {
  switchScreenClean(SCREEN_ID_SCAN);
}

void uiShowRegisterPanel(const char *nama, const char *nim, const char *status) {
  switchScreenClean(SCREEN_ID_REGISTER);
  if (uiReady) {
    if (objects.register_name) {
      lv_label_set_text(objects.register_name, nama && nama[0] ? nama : "-");
    }
    if (objects.register_nim) {
      lv_label_set_text(objects.register_nim, nim && nim[0] ? nim : "-");
    }
    if (objects.register_status) {
      lv_label_set_text(objects.register_status, status && status[0] ? status : "Scan your fingerprint to register");
    }
  }
  forceScreenRedraw();
}

void uiShowSuccess(const char *nama, const char *nim, const char *detail) {
  switchScreenClean(SCREEN_ID_RESULT);
  if (uiReady) {
    if (nama && objects.result_name) {
      char buffer[96];
      snprintf(buffer, sizeof(buffer), "Nama : %s", nama);
      lv_label_set_text(objects.result_name, buffer);
    }
    if (nim && objects.result_nim) {
      char buffer[96];
      snprintf(buffer, sizeof(buffer), "NIM : %s", nim);
      lv_label_set_text(objects.result_nim, buffer);
    }
    if (objects.obj13) {
      lv_label_set_text(objects.obj13, "PRESENT");
    }
  }
  forceScreenRedraw();
}

void uiShowFail(const char *status, const char *detail) {
  switchScreenClean(SCREEN_ID_RESULT);
  if (uiReady) {
    if (objects.result_name) {
      char buffer[96];
      snprintf(buffer, sizeof(buffer), "Status : %s", status ? status : "-");
      lv_label_set_text(objects.result_name, buffer);
    }
    if (objects.result_nim) {
      char buffer[96];
      snprintf(buffer, sizeof(buffer), "Detail : %s", detail ? detail : "-");
      lv_label_set_text(objects.result_nim, buffer);
    }
    if (objects.obj13) {
      lv_label_set_text(objects.obj13, "FAILED");
    }
  }
  forceScreenRedraw();
}

void initEezUi() {
  eez::flow::stopScriptHook = stopScriptNoop;
  ui_init();
  uiReady = true;
  uiTick();
  delayWithUi(20);
}

void runBootAnimation() {
  Serial.println("[BOOT] Animation started");
  uiShowBootPanel();

  uint32_t start = millis();
  while (millis() - start < EEZ_BOOT_DURATION_MS) {
    lv_timer_handler();
    ui_tick();
    delay(5);
  }
  Serial.printf("[BOOT] Animation finished after %lu ms\n", (unsigned long)(millis() - start));
}


// ---------- Prototypes ----------
void serviceBackground();
void connectMQTT();
void onMqttMessage(char* topic, byte* payload, unsigned int length);
void publishStatus(const char* status);

bool doEnroll(uint16_t id);
void processPendingRegistration();
void pollFingerprintSensor();
void printInfo();
bool clearAllTemplates();
void clearMahasiswaList();
void enterStandbyMode(bool clearTemplates);
const Mahasiswa* findMahasiswaByFingerId(uint16_t fingerId, uint8_t* slotOut);

bool requestTemplateSync();
bool publishTemplateAck(const char* eventName, const char* templateUid, const char* message);
void resetTemplateSyncState(const char* reason);

void handleMahasiswaPayload(const char* payload, size_t length);
void handleTemplateManifestPayload(const char* payload, size_t length);
void handleTemplateChunkPayload(const char* payload, size_t length);

bool decodeBase64ToBytes(const String& b64, uint8_t** outBuf, size_t* outLen);
bool encodeBytesToBase64(const uint8_t* bytes, size_t len, String& outB64);

uint16_t getSensorPacketPayloadSizeRaw();
uint16_t getSensorPacketPayloadSize();
uint8_t sendDownCharToBuffer1WithPayload(const uint8_t* bytes, size_t len, uint16_t payloadSize);
uint8_t sendDownCharToBuffer1(const uint8_t* bytes, size_t len);
size_t estimateEffectiveTemplateLength(const uint8_t* bytes, size_t len);
bool restoreTemplateToSensor(uint16_t fingerprintId, const uint8_t* bytes, size_t len);
bool downloadTemplateBytes(uint16_t fingerprintId, uint8_t* out, size_t outCap, size_t* outLen);

bool startTemplateUploadRaw(uint8_t* packetType, uint8_t* packetPayload, size_t payloadCap, size_t* packetPayloadLen);
bool readFingerprintPacketRaw(uint8_t* typeOut, uint8_t* payload, size_t payloadCap, size_t* payloadLen, uint16_t timeoutMs);

void processPendingTemplateFinalization();

// ---------- BUZZER ----------
void buzzerTone(uint32_t freqHz, uint32_t durMs) {
  if (freqHz == 0 || durMs == 0) return;
  ledcWriteTone(BUZZER_CH, freqHz);
  delay(durMs);
  ledcWriteTone(BUZZER_CH, 0);
}
void beepSuccess() { buzzerTone(2000, 80); delay(60); buzzerTone(2600, 110); }
void beepFail()    { buzzerTone(450, 220); delay(80); buzzerTone(450, 120); }
void beepLocked()  { buzzerTone(700, 80); delay(60); buzzerTone(700, 80); delay(60); buzzerTone(700, 80); }

// ---------- WiFi / MQTT ----------
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  uiWifiConnecting();

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delayWithUi(250);
  }

  Serial.println("\nWiFi OK");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());
  uiWifiConnected();
}

void publishStatus(const char* status) {
  if (!mqttSendingEnabled) {
    Serial.println("[MQTT] Status publish skipped (sending disabled)");
    return;
  }
  if (!mqtt.connected()) return;

  String payload = "{";
  payload += "\"deviceId\":\"";
  payload += DEVICE_ID;
  payload += "\",";
  payload += "\"status\":\"";
  payload += status;
  payload += "\",";
  payload += "\"ip\":\"";
  payload += WiFi.localIP().toString();
  payload += "\"";
  payload += "}";

  bool ok = mqtt.publish(TOPIC_STATUS, payload.c_str(), true);

  if (ok) {
    Serial.print("[MQTT] Status published: ");
    Serial.println(payload);
  } else {
    Serial.println("[MQTT] Failed to publish status");
  }
}

void publishAttendanceTest() {
  if (!mqttSendingEnabled) {
    Serial.println("[MQTT] Attendance publish skipped (sending disabled)");
    return;
  }
  if (!mqtt.connected()) return;

  String payload = "{";
  payload += "\"sessionId\":\"session-001\",";
  payload += "\"studentId\":\"mhs-001\",";
  payload += "\"deviceId\":\"";
  payload += DEVICE_ID;
  payload += "\",";
  payload += "\"status\":\"PRESENT\",";
  payload += "\"verifiedAt\":\"2026-04-27T10:05:00+07:00\"";
  payload += "}";

  bool ok = mqtt.publish(TOPIC_ATTENDANCE, payload.c_str());

  if (ok) {
    Serial.print("[MQTT] Attendance published: ");
    Serial.println(payload);
  } else {
    Serial.println("[MQTT] Failed to publish attendance");
  }
}

bool publishPresensi(int fingerId, int confidence, const Mahasiswa* m, int slot) {
  if (!mqtt.connected()) return false;

  StaticJsonDocument<384> doc;
  doc["device_id"] = DEVICE_ID;
  doc["finger_id"] = fingerId;
  doc["confidence"] = confidence;
  doc["ts_ms"] = (uint32_t)millis();
  if (activeClassCode.length() > 0) doc["class_code"] = activeClassCode;

  if (m != nullptr) {
    doc["nim"] = m->nim;
    doc["nama"] = m->nama;
    doc["name"] = m->nama;
    if (slot >= 1 && slot <= 3) doc["slot"] = slot;
  }

  char payload[320];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) {
    Serial.println("Publish presensi: FAIL (serialize)");
    return false;
  }

  bool ok = mqtt.publish(TOPIC_PRESENSI, payload);
  Serial.print("Publish presensi: ");
  Serial.println(ok ? "OK" : "FAIL");
  if (ok) Serial.println(payload);
  return ok;
}

bool clearAllTemplates() {
  uint8_t p = finger.emptyDatabase();
  if (p == FINGERPRINT_OK) {
    Serial.println("[OK] Semua template di sensor berhasil dihapus.");
    return true;
  }
  Serial.print("[FAIL] Gagal hapus database sensor: 0x");
  Serial.println(p, HEX);
  return false;
}

void resetTemplateSyncItems() {
  for (size_t i = 0; i < MAX_SYNC_TEMPLATES; i++) {
    syncItems[i].used = false;
    syncItems[i].restored = false;
    syncItems[i].failed = false;
    syncItems[i].templateUid = "";
    syncItems[i].fingerprintId = 0;
    syncItems[i].chunkTotal = 0;
    syncItems[i].chunkReceived = 0;
    for (size_t c = 0; c < MAX_CHUNKS_PER_TEMPLATE; c++) {
      syncItems[i].gotChunk[c] = false;
      syncItems[i].chunkData[c] = "";
    }
  }
}

void resetTemplateSyncState(const char* reason) {
  syncDone = false;
  syncExpected = false;
  syncState.inProgress = false;
  syncState.manifestReceived = false;
  syncState.syncId = "";
  syncState.totalTemplates = 0;
  syncState.successCount = 0;
  syncState.failedCount = 0;
  resetTemplateSyncItems();
  Serial.print("[SYNC] Reset state: ");
  Serial.println(reason);
}

void enterStandbyMode(bool clearTemplates) {
  Serial.println("[STANDBY] Enter standby mode");
  standbyMode = true;
  resetTemplateSyncState("standby");
  activeClassCode = "";
  activeClassName = "";
  clearMahasiswaList();
  mahasiswaDataReceived = false;

  uiShowStandbyPanel();

  if (clearTemplates) {
    Serial.println("[STANDBY] Clearing sensor templates");
    clearAllTemplates();
  }

  publishStatus("standby");
}

bool publishTemplateAck(const char* eventName, const char* templateUid, const char* message) {
  if (!mqtt.connected()) return false;

  StaticJsonDocument<512> doc;
  doc["device_id"] = DEVICE_ID;
  doc["event"] = eventName;
  doc["sync_id"] = syncState.syncId;
  doc["sync_done"] = syncDone;
  doc["in_progress"] = syncState.inProgress;
  doc["total_templates"] = syncState.totalTemplates;
  doc["success_count"] = syncState.successCount;
  doc["failed_count"] = syncState.failedCount;
  if (templateUid != nullptr && templateUid[0] != '\0') doc["template_uid"] = templateUid;
  if (message != nullptr && message[0] != '\0') doc["message"] = message;
  doc["ts_ms"] = (uint32_t)millis();

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) return false;

  bool ok = mqtt.publish(TOPIC_TEMPLATE_ACK, payload);
  Serial.print("[SYNC] ACK ");
  Serial.print(eventName);
  Serial.print(": ");
  Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

bool requestTemplateSync() {
  if (!mqtt.connected()) return false;
  if (standbyMode) {
    Serial.println("[SYNC] Request sync skipped (standby mode)");
    return false;
  }

  StaticJsonDocument<160> doc;
  doc["device_id"] = DEVICE_ID;
  doc["action"] = "sync_templates";

  char payload[160];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0) return false;

  bool ok = mqtt.publish(TOPIC_TEMPLATE_REQ, payload);
  Serial.print("[SYNC] Request sync publish: ");
  Serial.println(ok ? "OK" : "FAIL");
  if (ok) Serial.println(payload);
  return ok;
}

TemplateSyncItem* findSyncItem(const String& uid) {
  for (size_t i = 0; i < MAX_SYNC_TEMPLATES; i++) {
    if (syncItems[i].used && syncItems[i].templateUid == uid) return &syncItems[i];
  }
  return nullptr;
}

TemplateSyncItem* getOrCreateSyncItem(const String& uid) {
  TemplateSyncItem* found = findSyncItem(uid);
  if (found != nullptr) return found;

  for (size_t i = 0; i < MAX_SYNC_TEMPLATES; i++) {
    if (!syncItems[i].used) {
      syncItems[i].used = true;
      syncItems[i].restored = false;
      syncItems[i].failed = false;
      syncItems[i].templateUid = uid;
      syncItems[i].fingerprintId = 0;
      syncItems[i].chunkTotal = 0;
      syncItems[i].chunkReceived = 0;
      for (size_t c = 0; c < MAX_CHUNKS_PER_TEMPLATE; c++) {
        syncItems[i].gotChunk[c] = false;
        syncItems[i].chunkData[c] = "";
      }
      return &syncItems[i];
    }
  }
  return nullptr;
}

bool allChunksReady(const TemplateSyncItem& item) {
  if (item.chunkTotal == 0 || item.chunkTotal > MAX_CHUNKS_PER_TEMPLATE) return false;
  for (uint16_t i = 0; i < item.chunkTotal; i++) if (!item.gotChunk[i]) return false;
  return true;
}

void tryFinalizeSync() {
  if (!syncState.manifestReceived) return;

  if (syncState.totalTemplates == 0) {
    syncDone = true;
    syncState.inProgress = false;
    syncExpected = false;
    publishTemplateAck("sync_complete", "", "no_templates");
    uiSensorReady();
    uiShowReadyPanel();
    return;
  }

  uint16_t done = syncState.successCount + syncState.failedCount;
  if (done < syncState.totalTemplates) {
    char sensorLine[64];
    snprintf(sensorLine, sizeof(sensorLine), "Sync %u/%u", done, syncState.totalTemplates);
    uiSetPrepareStatus("Connected", "Connected", sensorLine, 85);
    return;
  }

  syncState.inProgress = false;
  syncDone = (syncState.failedCount == 0 && syncState.successCount == syncState.totalTemplates);
  syncExpected = false;
  publishTemplateAck("sync_complete", "", syncDone ? "all_restored" : "partial_or_failed");

  if (syncDone) {
    uiSensorReady();
    delayWithUi(500);
    uiShowReadyPanel();
  } else {
    uiShowFail("Template sync failed", "Check MQTT/template data");
  }
}

const Mahasiswa* findMahasiswaByFingerId(uint16_t fingerId, uint8_t* slotOut) {
  for (size_t i = 0; i < mahasiswaCount; i++) {
    for (uint8_t s = 0; s < 3; s++) {
      if (mahasiswaList[i].fingerId[s] == (int32_t)fingerId) {
        if (slotOut) *slotOut = s;
        return &mahasiswaList[i];
      }
    }
  }
  return nullptr;
}

bool publishRegistrasiResult(const Mahasiswa& m, uint8_t slot, uint16_t fingerId, bool success,
                             const char* message, const String* templateB64) {
  if (!mqtt.connected()) return false;

  size_t docSize = 640;
  if (templateB64 != nullptr) docSize += templateB64->length();
  DynamicJsonDocument doc(docSize);

  doc["device_id"] = DEVICE_ID;
  doc["nim"] = m.nim;
  doc["nama"] = m.nama;
  doc["name"] = m.nama;
  doc["slot"] = slot;
  doc["finger_id"] = fingerId;
  doc["success"] = success;
  doc["message"] = message;
  doc["ts_ms"] = (uint32_t)millis();
  if (templateB64 != nullptr) {
    doc["template_b64"] = *templateB64;
    doc["template_format"] = "zfm_template";
  }

  String payload;
  serializeJson(doc, payload);

  if (payload.length() > MQTT_BUFFER_SIZE - 64) {
    Serial.print("[MQTT] Payload registrasi mendekati batas buffer. len=");
    Serial.println((unsigned int)payload.length());
  }

  bool ok = mqtt.publish(TOPIC_REGISTRASI, payload.c_str());
  Serial.print("Publish registrasi: ");
  Serial.println(ok ? "OK" : "FAIL");
  if (ok) Serial.println(payload.c_str());
  return ok;
}

int32_t toFingerId(JsonVariantConst v) {
  if (v.isNull()) return -1;
  if (v.is<int32_t>() || v.is<int>() || v.is<long>()) {
    int32_t id = v.as<int32_t>();
    return (id > 0) ? id : -1;
  }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || s[0] == '\0') return -1;
    char* endPtr = nullptr;
    long parsed = strtol(s, &endPtr, 10);
    if (endPtr == s || *endPtr != '\0') return -1;
    return (parsed > 0) ? (int32_t)parsed : -1;
  }
  return -1;
}

bool decodeBase64ToBytes(const String& b64, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;
  if (b64.length() == 0) return false;

  size_t needed = 0;
  int rc = mbedtls_base64_decode(nullptr, 0, &needed,
                                 (const unsigned char*)b64.c_str(), b64.length());
  if (rc != 0 && rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return false;
  if (needed == 0 || needed > MAX_TEMPLATE_BYTES) return false;

  uint8_t* buf = (uint8_t*)malloc(needed);
  if (!buf) return false;

  size_t outSz = 0;
  rc = mbedtls_base64_decode(buf, needed, &outSz,
                             (const unsigned char*)b64.c_str(), b64.length());
  if (rc != 0 || outSz == 0) {
    free(buf);
    return false;
  }
  *outBuf = buf;
  *outLen = outSz;
  return true;
}

bool encodeBytesToBase64(const uint8_t* bytes, size_t len, String& outB64) {
  outB64 = "";
  if (!bytes || len == 0 || len > MAX_TEMPLATE_BYTES) return false;

  size_t needed = 0;
  int rc = mbedtls_base64_encode(nullptr, 0, &needed, bytes, len);
  if (rc != 0 && rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return false;
  if (needed == 0 || needed > (MAX_TEMPLATE_BASE64_CHARS + 1)) return false;

  unsigned char buf[MAX_TEMPLATE_BASE64_CHARS + 2];
  size_t outLenLocal = 0;
  rc = mbedtls_base64_encode(buf, sizeof(buf) - 1, &outLenLocal, bytes, len);
  if (rc != 0 || outLenLocal == 0) return false;

  buf[outLenLocal] = '\0';
  outB64 = String((const char*)buf);
  return true;
}

uint16_t getSensorPacketPayloadSize() { return getSensorPacketPayloadSizeRaw(); }

uint16_t getSensorPacketPayloadSizeRaw() {
  uint16_t packetSize = finger.packet_len;
  if (packetSize == 0) packetSize = 32;
  if (packetSize > 64) packetSize = 64;
  if (packetSize < 16) packetSize = 16;
  return packetSize;
}

// ========= RAW PACKET HELPERS FOR TEMPLATE UP/DOWN =========
// NOTE: Adafruit_Fingerprint internal readStructuredPacket punya limit payload 64.
// Kamu sudah bikin parser raw sendiri agar bisa 256 byte packetPayload.

bool readFingerprintPacketRaw(uint8_t* typeOut, uint8_t* payload, size_t payloadCap, size_t* payloadLen,
                              uint16_t timeoutMs) {
  if (!typeOut || !payload || !payloadLen) return false;
  *payloadLen = 0;

  auto readByte = [&](uint8_t* outByte, uint16_t timeout) -> bool {
    uint32_t started = millis();
    while ((uint32_t)(millis() - started) < timeout) {
      if (FPSerial.available() > 0) {
        int c = FPSerial.read();
        if (c >= 0) { *outByte = (uint8_t)c; return true; }
      }
      delay(1);
    }
    return false;
  };

  uint8_t b = 0;
  bool gotStart = false;
  uint32_t started = millis();
  while ((uint32_t)(millis() - started) < timeoutMs) {
    if (!readByte(&b, 10)) continue;
    if (b != (uint8_t)(FINGERPRINT_STARTCODE >> 8)) continue;

    uint8_t b2 = 0;
    if (!readByte(&b2, timeoutMs)) return false;
    if (b2 == (uint8_t)(FINGERPRINT_STARTCODE & 0xFF)) { gotStart = true; break; }
  }
  if (!gotStart) return false;

  uint8_t addr[4];
  for (int i = 0; i < 4; i++) if (!readByte(&addr[i], timeoutMs)) return false;

  uint8_t type = 0, lenHi = 0, lenLo = 0;
  if (!readByte(&type, timeoutMs)) return false;
  if (!readByte(&lenHi, timeoutMs)) return false;
  if (!readByte(&lenLo, timeoutMs)) return false;

  uint16_t wireLen = ((uint16_t)lenHi << 8) | lenLo;
  if (wireLen < 2) return false;

  size_t dataLen = (size_t)(wireLen - 2);
  uint32_t sum = (uint32_t)type + (uint32_t)lenHi + (uint32_t)lenLo;

  bool overflow = dataLen > payloadCap;
  for (size_t i = 0; i < dataLen; i++) {
    uint8_t dataByte = 0;
    if (!readByte(&dataByte, timeoutMs)) return false;
    if (!overflow) payload[i] = dataByte;
    sum += dataByte;
  }

  uint8_t csumHi = 0, csumLo = 0;
  if (!readByte(&csumHi, timeoutMs)) return false;
  if (!readByte(&csumLo, timeoutMs)) return false;
  uint16_t recvChecksum = ((uint16_t)csumHi << 8) | csumLo;
  uint16_t calcChecksum = (uint16_t)(sum & 0xFFFF);

  if (overflow) {
    Serial.print("[TPL] packet payload terlalu besar: ");
    Serial.print((unsigned int)dataLen);
    Serial.print(" > ");
    Serial.println((unsigned int)payloadCap);
    return false;
  }
  if (recvChecksum != calcChecksum) {
    Serial.print("[TPL] checksum mismatch recv=0x");
    Serial.print(recvChecksum, HEX);
    Serial.print(" calc=0x");
    Serial.println(calcChecksum, HEX);
    return false;
  }

  *typeOut = type;
  *payloadLen = dataLen;
  return true;
}

// DownChar command code untuk ZFM (Download to CharBuffer)
#define FINGERPRINT_DOWNLOAD 0x09

uint8_t sendDownCharToBuffer1WithPayload(const uint8_t* bytes, size_t len, uint16_t payloadSize) {
  if (!bytes || len == 0) return FINGERPRINT_PACKETRECIEVEERR;
  if (payloadSize == 0) payloadSize = getSensorPacketPayloadSize();
  if (payloadSize > 64) payloadSize = 64;

  uint8_t cmd[2] = {FINGERPRINT_DOWNLOAD, 0x01};
  Adafruit_Fingerprint_Packet cmdPacket(FINGERPRINT_COMMANDPACKET, sizeof(cmd), cmd);
  finger.writeStructuredPacket(cmdPacket);
  FPSerial.flush();

  uint8_t ackType = 0;
  uint8_t ackData[64];
  size_t ackLen = 0;
  if (!readFingerprintPacketRaw(&ackType, ackData, sizeof(ackData), &ackLen, 4000)) {
    Serial.println("[SYNC] DownChar ACK timeout/parse error.");
    return FINGERPRINT_PACKETRECIEVEERR;
  }
  if (ackType != FINGERPRINT_ACKPACKET || ackLen == 0) return FINGERPRINT_PACKETRECIEVEERR;
  if (ackData[0] != FINGERPRINT_OK) return ackData[0];

  size_t offset = 0;
  while (offset < len) {
    uint16_t chunk = (uint16_t)((len - offset) > payloadSize ? payloadSize : (len - offset));
    uint8_t pktType = ((offset + chunk) >= len) ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;
    Adafruit_Fingerprint_Packet dataPacket(pktType, chunk, (uint8_t*)(bytes + offset));
    finger.writeStructuredPacket(dataPacket);
    FPSerial.flush();
    offset += chunk;
    delay(6);
  }

  delay(100);

  // Banyak firmware R302 tidak kirim final ACK setelah data DownChar → timeout dianggap OK
  ackType = 0; ackLen = 0;
  if (!readFingerprintPacketRaw(&ackType, ackData, sizeof(ackData), &ackLen, 600)) {
    return FINGERPRINT_OK;
  }
  if (ackType == FINGERPRINT_ACKPACKET && ackLen > 0) return ackData[0];
  return FINGERPRINT_OK;
}

uint8_t sendDownCharToBuffer1(const uint8_t* bytes, size_t len) {
  return sendDownCharToBuffer1WithPayload(bytes, len, getSensorPacketPayloadSize());
}

size_t estimateEffectiveTemplateLength(const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0) return 0;
  size_t end = len;
  while (end > 0 && bytes[end - 1] == 0) end--;
  return end;
}

bool restoreTemplateToSensor(uint16_t fingerprintId, const uint8_t* bytes, size_t len) {
  if (fingerprintId == 0 || fingerprintId > finger.capacity) return false;

  uint8_t del = finger.deleteModel(fingerprintId);
  if (!(del == FINGERPRINT_OK || del == FINGERPRINT_NOTFOUND)) {
    Serial.print("[SYNC] deleteModel warning: 0x");
    Serial.println(del, HEX);
  }

  uint16_t packetSize = getSensorPacketPayloadSize();
  uint16_t packetSizeRaw = getSensorPacketPayloadSizeRaw();

  size_t effectiveLen = estimateEffectiveTemplateLength(bytes, len);
  size_t roundedLen = effectiveLen;
  if (roundedLen > 0 && packetSize > 0) {
    roundedLen = ((roundedLen + packetSize - 1) / packetSize) * packetSize;
    if (roundedLen > len) roundedLen = len;
  }
  size_t roundedLenRaw = effectiveLen;
  if (roundedLenRaw > 0 && packetSizeRaw > 0) {
    roundedLenRaw = ((roundedLenRaw + packetSizeRaw - 1) / packetSizeRaw) * packetSizeRaw;
    if (roundedLenRaw > len) roundedLenRaw = len;
  }

  size_t candidates[6] = {len, roundedLen, roundedLenRaw, effectiveLen, 512, 256};
  uint16_t payloadCandidates[2] = {packetSize, packetSizeRaw};
  uint8_t p = FINGERPRINT_PACKETRECIEVEERR;

  for (size_t i = 0; i < 6; i++) {
    size_t candidateLen = candidates[i];
    if (candidateLen == 0 || candidateLen > len) continue;

    bool dupLen = false;
    for (size_t j = 0; j < i; j++) if (candidates[j] == candidateLen) { dupLen = true; break; }
    if (dupLen) continue;

    for (size_t mode = 0; mode < 2; mode++) {
      uint16_t payloadSize = payloadCandidates[mode];
      bool dupPay = false;
      for (size_t pm = 0; pm < mode; pm++) if (payloadCandidates[pm] == payloadSize) { dupPay = true; break; }
      if (dupPay) continue;

      p = sendDownCharToBuffer1WithPayload(bytes, candidateLen, payloadSize);
      if (p == FINGERPRINT_OK) {
        delay(50);
        p = finger.storeModel(fingerprintId);
        if (p == FINGERPRINT_OK) return true;

        Serial.print("[SYNC] storeModel gagal: 0x");
        Serial.println(p, HEX);
      }
    }
  }
  return false;
}

bool startTemplateUploadRaw(uint8_t* packetType, uint8_t* packetPayload, size_t payloadCap,
                           size_t* packetPayloadLen) {
  if (!packetType || !packetPayload || !packetPayloadLen) return false;

  uint8_t cmd[2] = {FINGERPRINT_UPLOAD, 0x01};
  Adafruit_Fingerprint_Packet cmdPacket(FINGERPRINT_COMMANDPACKET, sizeof(cmd), cmd);
  finger.writeStructuredPacket(cmdPacket);

  if (!readFingerprintPacketRaw(packetType, packetPayload, payloadCap, packetPayloadLen, 2500)) {
    Serial.println("[TPL] upload start timeout.");
    return false;
  }

  if (*packetType == FINGERPRINT_ACKPACKET) {
    if (*packetPayloadLen == 0) return false;
    if (packetPayload[0] != FINGERPRINT_OK) {
      Serial.print("[TPL] upload ACK gagal: 0x");
      Serial.println(packetPayload[0], HEX);
      return false;
    }
    if (!readFingerprintPacketRaw(packetType, packetPayload, payloadCap, packetPayloadLen, 2500)) {
      Serial.println("[TPL] upload data pertama timeout.");
      return false;
    }
  }

  if (!(*packetType == FINGERPRINT_DATAPACKET || *packetType == FINGERPRINT_ENDDATAPACKET)) {
    Serial.print("[TPL] packet awal upload tidak valid: 0x");
    Serial.println(*packetType, HEX);
    return false;
  }
  return true;
}

bool downloadTemplateBytes(uint16_t fingerprintId, uint8_t* out, size_t outCap, size_t* outLen) {
  if (!outLen) return false;
  *outLen = 0;
  if (!out || outCap == 0) return false;

  uint8_t p = finger.loadModel(fingerprintId);
  if (p != FINGERPRINT_OK) {
    Serial.print("[TPL] loadModel gagal: 0x");
    Serial.println(p, HEX);
    return false;
  }

  uint8_t packetType = 0;
  uint8_t packetPayload[256];
  size_t packetPayloadLen = 0;
  size_t offset = 0;

  if (!startTemplateUploadRaw(&packetType, packetPayload, sizeof(packetPayload), &packetPayloadLen)) return false;

  while (true) {
    if (!(packetType == FINGERPRINT_DATAPACKET || packetType == FINGERPRINT_ENDDATAPACKET)) return false;

    if ((offset + packetPayloadLen) > outCap) {
      Serial.print("[TPL] buffer output kurang. needed=");
      Serial.print((unsigned int)(offset + packetPayloadLen));
      Serial.print(" cap=");
      Serial.println((unsigned int)outCap);
      return false;
    }

    memcpy(out + offset, packetPayload, packetPayloadLen);
    offset += packetPayloadLen;

    if (packetType == FINGERPRINT_ENDDATAPACKET) break;

    if (!readFingerprintPacketRaw(&packetType, packetPayload, sizeof(packetPayload), &packetPayloadLen, 2500)) {
      Serial.println("[TPL] baca packet raw gagal.");
      return false;
    }
  }

  if (offset == 0) return false;
  *outLen = offset;
  Serial.print("[TPL] Download template bytes=");
  Serial.println((unsigned int)offset);
  return true;
}

// ---------- Mahasiswa parsing ----------
int32_t readFingerIdField(JsonObjectConst obj, const char* key1, const char* key2, const char* key3) {
  if (obj.containsKey(key1)) return toFingerId(obj[key1]);
  if (key2 && obj.containsKey(key2)) return toFingerId(obj[key2]);
  if (key3 && obj.containsKey(key3)) return toFingerId(obj[key3]);
  return -1;
}
String readNameField(JsonObjectConst obj) {
  String nama = String(obj["nama"] | "");
  nama.trim();
  if (nama.length() > 0) return nama;
  nama = String(obj["name"] | "");
  nama.trim();
  return nama;
}
int32_t readFingerIdFromArray(JsonObjectConst obj, const char* arrKey, size_t idx) {
  if (!arrKey) return -1;
  if (!obj[arrKey].is<JsonArrayConst>()) return -1;
  JsonArrayConst arr = obj[arrKey].as<JsonArrayConst>();
  if (idx >= arr.size()) return -1;
  return toFingerId(arr[idx]);
}

void clearMahasiswaList() {
  for (size_t i = 0; i < MAX_MAHASISWA; i++) {
    mahasiswaList[i].nama = "";
    mahasiswaList[i].nim = "";
    mahasiswaList[i].fingerId[0] = -1;
    mahasiswaList[i].fingerId[1] = -1;
    mahasiswaList[i].fingerId[2] = -1;
  }
  mahasiswaCount = 0;
}

void updateNextIdFromMahasiswaData() {
  int32_t maxKnownId = 0;
  for (size_t i = 0; i < mahasiswaCount; i++) {
    for (int s = 0; s < 3; s++) if (mahasiswaList[i].fingerId[s] > maxKnownId) maxKnownId = mahasiswaList[i].fingerId[s];
  }
  if (maxKnownId > 0 && maxKnownId < 65535) {
    uint16_t candidate = (uint16_t)(maxKnownId + 1);
    if (candidate > nextID) nextID = candidate;
  }
}

void applyMahasiswaFromArray(JsonArrayConst arr) {
  clearMahasiswaList();

  for (JsonVariantConst item : arr) {
    if (!item.is<JsonObjectConst>()) continue;
    if (mahasiswaCount >= MAX_MAHASISWA) break;

    JsonObjectConst obj = item.as<JsonObjectConst>();
    Mahasiswa& m = mahasiswaList[mahasiswaCount];

    m.nama = readNameField(obj);
    m.nim = String(obj["nim"] | "");
    m.fingerId[0] = readFingerIdField(obj, "fingerprint1", "fingerprint_1", "fp1");
    m.fingerId[1] = readFingerIdField(obj, "fingerprint2", "fingerprint_2", "fp2");
    m.fingerId[2] = readFingerIdField(obj, "fingerprint3", "fingerprint_3", "fp3");
    if (m.fingerId[0] < 0) m.fingerId[0] = readFingerIdFromArray(obj, "fingerprints", 0);
    if (m.fingerId[1] < 0) m.fingerId[1] = readFingerIdFromArray(obj, "fingerprints", 1);
    if (m.fingerId[2] < 0) m.fingerId[2] = readFingerIdFromArray(obj, "fingerprints", 2);

    mahasiswaCount++;
  }

  mahasiswaDataReceived = true;
  updateNextIdFromMahasiswaData();
  Serial.print("Data mahasiswa diperbarui: ");
  Serial.println((unsigned int)mahasiswaCount);
}

void handleMahasiswaPayload(const char* payload, size_t length) {
  DynamicJsonDocument doc(length + 2048);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("Gagal parse JSON mahasiswa: ");
    Serial.println(err.c_str());
    return;
  }

  if (doc.is<JsonArrayConst>()) { applyMahasiswaFromArray(doc.as<JsonArrayConst>()); return; }
  if (!doc.is<JsonObjectConst>()) { Serial.println("Payload mahasiswa tidak didukung (bukan object/array)."); return; }

  JsonObjectConst root = doc.as<JsonObjectConst>();

  if (root["mahasiswa"].is<JsonArrayConst>()) { applyMahasiswaFromArray(root["mahasiswa"].as<JsonArrayConst>()); return; }
  if (root["data"].is<JsonArrayConst>())      { applyMahasiswaFromArray(root["data"].as<JsonArrayConst>()); return; }
  if (root["items"].is<JsonArrayConst>())     { applyMahasiswaFromArray(root["items"].as<JsonArrayConst>()); return; }
  if (root["students"].is<JsonArrayConst>())  { applyMahasiswaFromArray(root["students"].as<JsonArrayConst>()); return; }

  if (root.containsKey("nim") && (root.containsKey("nama") || root.containsKey("name"))) {
    clearMahasiswaList();
    Mahasiswa& m = mahasiswaList[0];
    m.nama = readNameField(root);
    m.nim = String(root["nim"] | "");
    m.fingerId[0] = readFingerIdField(root, "fingerprint1", "fingerprint_1", "fp1");
    m.fingerId[1] = readFingerIdField(root, "fingerprint2", "fingerprint_2", "fp2");
    m.fingerId[2] = readFingerIdField(root, "fingerprint3", "fingerprint_3", "fp3");
    if (m.fingerId[0] < 0) m.fingerId[0] = readFingerIdFromArray(root, "fingerprints", 0);
    if (m.fingerId[1] < 0) m.fingerId[1] = readFingerIdFromArray(root, "fingerprints", 1);
    if (m.fingerId[2] < 0) m.fingerId[2] = readFingerIdFromArray(root, "fingerprints", 2);
    mahasiswaCount = 1;
    mahasiswaDataReceived = true;
    updateNextIdFromMahasiswaData();
    Serial.println("Data mahasiswa diperbarui: 1");
    return;
  }

  Serial.println("Payload mahasiswa tidak memiliki field array yang dikenali.");
}

// ---------- Template sync ----------
TemplateSyncItem* parseManifestItem(JsonObjectConst itemObj) {
  String uid = String(itemObj["template_uid"] | "");
  if (uid.length() == 0) uid = String(itemObj["uid"] | "");
  if (uid.length() == 0) return nullptr;

  TemplateSyncItem* item = getOrCreateSyncItem(uid);
  if (!item) return nullptr;

  int fpId = itemObj["fingerprint_id"] | itemObj["finger_id"] | 0;
  int chunkTotal = itemObj["chunk_total"] | itemObj["chunks"] | 0;

  if (fpId <= 0 || fpId > 65535) { item->failed = true; return item; }
  if (chunkTotal <= 0 || chunkTotal > (int)MAX_CHUNKS_PER_TEMPLATE) { item->failed = true; return item; }

  item->fingerprintId = (uint16_t)fpId;
  item->chunkTotal = (uint16_t)chunkTotal;
  return item;
}

bool finalizeOneTemplate(TemplateSyncItem* item) {
  if (!item) return false;
  if (item->restored || item->failed) return false;
  if (!allChunksReady(*item)) return false;

  String base64All = "";
  base64All.reserve(item->chunkTotal * 256);
  for (uint16_t i = 0; i < item->chunkTotal; i++) base64All += item->chunkData[i];

  uint8_t* tplBytes = nullptr;
  size_t tplLen = 0;
  if (!decodeBase64ToBytes(base64All, &tplBytes, &tplLen)) {
    item->failed = true;
    syncState.failedCount++;
    publishTemplateAck("template_failed", item->templateUid.c_str(), "decode_base64_failed");
    tryFinalizeSync();
    return false;
  }

  bool ok = restoreTemplateToSensor(item->fingerprintId, tplBytes, tplLen);
  free(tplBytes);

  if (ok) {
    item->restored = true;
    syncState.successCount++;
    publishTemplateAck("template_restored", item->templateUid.c_str(), "ok");
  } else {
    item->failed = true;
    syncState.failedCount++;
    publishTemplateAck("template_failed", item->templateUid.c_str(), "restore_sensor_failed");
  }

  tryFinalizeSync();
  return ok;
}

void processPendingTemplateFinalization() {
  if (!syncState.inProgress) return;
  for (size_t i = 0; i < MAX_SYNC_TEMPLATES; i++) {
    if (!syncItems[i].used) continue;
    if (syncItems[i].restored || syncItems[i].failed) continue;
    if (!allChunksReady(syncItems[i])) continue;
    finalizeOneTemplate(&syncItems[i]);
  }
}

void handleTemplateManifestPayload(const char* payload, size_t length) {
  if (!syncExpected && !syncState.inProgress) {
    Serial.println("[SYNC] Manifest ignored (sync not expected)");
    return;
  }
  DynamicJsonDocument doc(length + 4096);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[SYNC] Manifest JSON parse gagal: ");
    Serial.println(err.c_str());
    return;
  }
  if (!doc.is<JsonObjectConst>()) {
    Serial.println("[SYNC] Manifest harus JSON object.");
    return;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  String syncId = String(root["sync_id"] | "");
  if (syncId.length() == 0) syncId = String(root["syncId"] | "");
  if (syncId.length() == 0) syncId = String((uint32_t)millis(), HEX);

  if (!syncState.inProgress || syncState.syncId != syncId) {
    resetTemplateSyncState("new_manifest");
    syncState.inProgress = true;
    syncState.syncId = syncId;
  }

  JsonArrayConst arr;
  if (root["templates"].is<JsonArrayConst>()) arr = root["templates"].as<JsonArrayConst>();
  else if (root["items"].is<JsonArrayConst>()) arr = root["items"].as<JsonArrayConst>();

  uint16_t totalTemplates = root["total_templates"] | root["total"] | 0;
  if (!arr.isNull() && totalTemplates == 0) totalTemplates = (uint16_t)arr.size();
  syncState.totalTemplates = totalTemplates;
  syncState.manifestReceived = true;

  if (!arr.isNull()) {
    for (JsonVariantConst v : arr) {
      if (!v.is<JsonObjectConst>()) continue;
      TemplateSyncItem* item = parseManifestItem(v.as<JsonObjectConst>());
      if (!item) continue;
      if (item->failed) {
        syncState.failedCount++;
        publishTemplateAck("template_failed", item->templateUid.c_str(), "manifest_invalid_item");
      } else {
        finalizeOneTemplate(item);
      }
    }
  }

  publishTemplateAck("manifest_received", "", "ok");
  tryFinalizeSync();
}

void handleTemplateChunkPayload(const char* payload, size_t length) {
  if (!syncExpected && !syncState.inProgress) {
    Serial.println("[SYNC] Chunk ignored (sync not expected)");
    return;
  }
  DynamicJsonDocument doc(length + 512);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[SYNC] Chunk JSON parse gagal: ");
    Serial.println(err.c_str());
    return;
  }
  if (!doc.is<JsonObjectConst>()) {
    Serial.println("[SYNC] Chunk harus JSON object.");
    return;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  String syncId = String(root["sync_id"] | "");
  if (syncId.length() == 0) syncId = String(root["syncId"] | "");
  if (syncId.length() == 0) return;

  if (!syncState.inProgress) {
    resetTemplateSyncState("chunk_starts_sync");
    syncState.inProgress = true;
    syncState.syncId = syncId;
  }
  if (syncState.syncId != syncId) return;

  String uid = String(root["template_uid"] | "");
  if (uid.length() == 0) uid = String(root["uid"] | "");
  if (uid.length() == 0) return;

  int chunkIndex = root["chunk_index"] | root["index"] | 0;
  int incomingChunkTotal = root["chunk_total"] | root["total_chunks"] | 0;
  String chunkB64 = String(root["chunk"] | "");
  if (chunkB64.length() == 0) chunkB64 = String(root["data"] | "");
  if (chunkB64.length() == 0) chunkB64 = String(root["payload"] | "");
  if (chunkB64.length() == 0) chunkB64 = String(root["b64"] | "");
  if (chunkIndex <= 0 || chunkB64.length() == 0) return;

  TemplateSyncItem* item = getOrCreateSyncItem(uid);
  if (!item) return;

  int fpId = root["fingerprint_id"] | root["finger_id"] | (int)item->fingerprintId;
  if (fpId > 0 && fpId < 65536) item->fingerprintId = (uint16_t)fpId;

  if (incomingChunkTotal > 0) {
    if (item->chunkTotal == 0) item->chunkTotal = (uint16_t)incomingChunkTotal;
  }
  if (item->chunkTotal == 0 || item->chunkTotal > MAX_CHUNKS_PER_TEMPLATE) {
    item->failed = true;
    if (syncState.manifestReceived) syncState.failedCount++;
    publishTemplateAck("template_failed", uid.c_str(), "chunk_total_invalid");
    tryFinalizeSync();
    return;
  }
  if (chunkIndex > item->chunkTotal || chunkIndex > (int)MAX_CHUNKS_PER_TEMPLATE) return;

  uint16_t idx = (uint16_t)(chunkIndex - 1);
  if (!item->gotChunk[idx]) {
    item->gotChunk[idx] = true;
    item->chunkData[idx] = chunkB64;
    item->chunkReceived++;
  }
}

// ---------- MQTT callback ----------
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char* raw = (char*)malloc(length + 1);
  if (!raw) return;
  memcpy(raw, payload, length);
  raw[length] = '\0';

  Serial.print("[MQTT] Topic: ");
  Serial.println(topic);

  if (strcmp(topic, TOPIC_MAHASISWA) == 0) {
    if (standbyMode) {
      Serial.println("[STANDBY] Catalog ignored");
      free(raw);
      return;
    }
    handleMahasiswaPayload(raw, length);
  } else if (strcmp(topic, TOPIC_SESSION_CLEAR) == 0) {
    resetTemplateSyncState("session_clear");
    clearAllTemplates();
    if (uiReady) lv_label_set_text(objects.obj9, "Session cleared");
  } else if (strcmp(topic, TOPIC_COMMAND) == 0) {
    DynamicJsonDocument doc(length + 256);
    if (!deserializeJson(doc, raw, length)) {
      const char* command = doc["command"] | "";
      if (strcmp(command, "STANDBY") == 0) {
        bool clearTemplates = doc["clear_templates"] | true;
        enterStandbyMode(clearTemplates);
      } else if (strcmp(command, "REGISTER") == 0) {
        String nim = String(doc["nim"] | "");
        String nama = String(doc["nama"] | "");
        if (nama.length() == 0) nama = String(doc["name"] | "");
        int slot = doc["slot"] | 1;
        if (slot < 1 || slot > 3) slot = 1;

        if (nim.length() > 0 && nama.length() > 0) {
          pendingRegistration.nim = nim;
          pendingRegistration.nama = nama;
          pendingRegistration.fingerId[0] = -1;
          pendingRegistration.fingerId[1] = -1;
          pendingRegistration.fingerId[2] = -1;
          pendingRegistrationSlot = (uint8_t)slot;
          registrationReturnToStandby = standbyMode;
          registrationRequested = true;

          if (standbyMode) {
            Serial.println("[STANDBY] Exit standby for registration");
            standbyMode = false;
          }

          Serial.print("[REGISTER] Queued registration for ");
          Serial.print(nim);
          Serial.print(" slot=");
          Serial.println(slot);
          uiShowRegisterPanel(nama.c_str(), nim.c_str(), "Scan your fingerprint to register");
          publishStatus("register");
        }
      } else if (strcmp(command, "SET_ACTIVE_CLASS") == 0) {
        String targetClass = String(doc["class_code"] | "");
        String targetClassName = String(doc["class_name"] | targetClass);
        if (targetClass.length() > 0) {
          if (standbyMode) {
            Serial.println("[STANDBY] Exit standby for class sync");
            standbyMode = false;
          }
          activeClassCode = targetClass;
          activeClassName = targetClassName;
          Serial.print("[INFO] Kelas aktif diubah via command: ");
          Serial.println(activeClassCode);
          
          uiShowSyncOnlySensorPanel("Checking...");
          resetTemplateSyncState("class_change");
          Serial.println("[SYNC] Wiping sensor templates before class load");
          clearAllTemplates();
          syncExpected = true;
        }
      }
    }
  } else if (strcmp(topic, TOPIC_TEMPLATE_MANIFEST) == 0) {
    if (standbyMode) {
      Serial.println("[STANDBY] Manifest ignored");
      free(raw);
      return;
    }
    uiSensorChecking();
    handleTemplateManifestPayload(raw, length);
  } else if (strcmp(topic, TOPIC_TEMPLATE_CHUNK) == 0) {
    if (standbyMode) {
      Serial.println("[STANDBY] Chunk ignored");
      free(raw);
      return;
    }
    uiSensorChecking();
    handleTemplateChunkPayload(raw, length);
  }
  free(raw);
}

void connectMQTT() {
  while (!mqtt.connected()) {
    uiMqttConnecting();
    Serial.print("Connecting to MQTT broker... ");

    String clientId = "esp32-" + String(DEVICE_ID) + "-" + String(random(0xffff), HEX);

    bool connected = mqtt.connect(
      clientId.c_str(),
      MQTT_USERNAME,
      MQTT_PASSWORD
    );

    if (connected) {
      Serial.println("connected");

      mqtt.subscribe(TOPIC_MAHASISWA, 1);
      mqtt.subscribe(TOPIC_SESSION_CLEAR, 1);
      mqtt.subscribe(TOPIC_COMMAND, 1);
      mqtt.subscribe(TOPIC_TEMPLATE_MANIFEST, 1);
      mqtt.subscribe(TOPIC_TEMPLATE_CHUNK, 1);

      Serial.println("Subscribed to:");
      Serial.println(TOPIC_MAHASISWA);
      Serial.println(TOPIC_SESSION_CLEAR);
      Serial.println(TOPIC_COMMAND);
      Serial.println(TOPIC_TEMPLATE_MANIFEST);
      Serial.println(TOPIC_TEMPLATE_CHUNK);

      publishStatus("online");
      uiMqttConnected();
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 5 seconds");

      if (uiReady) lv_label_set_text(objects.obj3, "MQTT : Retry...");
      delayWithUi(5000);
    }
  }
}

void serviceBackground() {
  uiTick();
  if (WiFi.status() != WL_CONNECTED) wifiConnect();
  connectMQTT();
  mqtt.loop();
  processPendingTemplateFinalization();
}

// ---------- Fingerprint helpers ----------
void waitFingerRemoved() {
  Serial.println("Angkat jari...");
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
  delay(250);
}

bool doEnroll(uint16_t id) {
  Serial.printf("\n=== ENROLL ID %u ===\n", id);

  Serial.println("Tempelkan jari (scan 1)...");
  while (finger.getImage() != FINGERPRINT_OK) {
    serviceBackground();
    delay(50);
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println("[FAIL] image2Tz(1)");
    beepFail();
    return false;
  }
  Serial.println("[OK] Scan 1");
  waitFingerRemoved();

  Serial.println("Tempelkan jari yang sama (scan 2)...");
  while (finger.getImage() != FINGERPRINT_OK) {
    serviceBackground();
    delay(50);
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    Serial.println("[FAIL] image2Tz(2)");
    beepFail();
    return false;
  }
  Serial.println("[OK] Scan 2");

  uint8_t p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("[FAIL] Scan 1 dan scan 2 tidak cocok.");
    beepFail();
    return false;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("[OK] Enroll sukses");
    beepSuccess();
    return true;
  }

  Serial.print("[FAIL] storeModel: 0x");
  Serial.println(p, HEX);
  beepFail();
  return false;
}

void doPresensiOnce() {
  Serial.println("\n=== PRESENSI (TRIGGER TOUCH) ===");
  if (!syncDone) {
    Serial.print("[LOCK] Presensi belum aktif. sync_id=");
    Serial.print(syncState.syncId);
    Serial.print(" restored=");
    Serial.print(syncState.successCount);
    Serial.print("/");
    Serial.print(syncState.totalTemplates);
    Serial.print(" failed=");
    Serial.println(syncState.failedCount);
    beepLocked();
    return;
  }
  delay(220);  // beri waktu jari benar2 menempel sebelum getImage

  uint8_t p = FINGERPRINT_NOFINGER;
  uint32_t start = millis();

  // tunggu sampai 1000ms untuk dapat image
  while ((millis() - start) < 1000) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    serviceBackground();
    delay(40);
  }

  if (p != FINGERPRINT_OK) {
    Serial.print("Gagal ambil gambar. code=0x");
    Serial.println(p, HEX);   // biar kita tahu NOFINGER atau error lain
    // beepFail(); // optional, tapi biasanya jangan bunyi kalau cuma NOFINGER
    return;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("Gagal konversi image2Tz.");
    beepFail();
    return;
  }

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    Serial.printf("[OK] MATCH ID=%d confidence=%d\n", finger.fingerID, finger.confidence);

    uint8_t slotIdx = 0;
    const Mahasiswa* matched = findMahasiswaByFingerId(finger.fingerID, &slotIdx);

    bool pubOk = false;
    if (matched != nullptr) {
      Serial.printf("[OK] Mahasiswa cocok: %s - %s (fingerprint_%u)\n",
                    matched->nim.c_str(), matched->nama.c_str(), (unsigned int)(slotIdx + 1));
      pubOk = publishPresensi(finger.fingerID, finger.confidence, matched, (int)(slotIdx + 1));
    } else {
      Serial.println("[WARN] Finger ID tidak ada di katalog / katalog belum tersedia.");
      pubOk = publishPresensi(finger.fingerID, finger.confidence, nullptr, -1);
    }

    if (pubOk) beepSuccess();
    else beepFail();

  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[FAIL] Sidik jari tidak dikenal");
    beepFail();
  } else {
    Serial.print("Search error: 0x");
    Serial.println(p, HEX);
    beepFail();
  }
}

// ---------- Button single press -> registration ----------
bool readButtonPressedOnce() {
  static bool lastStable = HIGH;
  static bool lastRead = HIGH;
  static uint32_t lastChangeMs = 0;
  bool raw = digitalRead(BTN_PIN);

  if (raw != lastRead) {
    lastRead = raw;
    lastChangeMs = millis();
  }

  if ((millis() - lastChangeMs) > DEBOUNCE_MS && raw != lastStable) {
    lastStable = raw;
    if (lastStable == LOW) {
      return true;  // trigger on press
    }
  }
  return false;
}

// ---------- TOUCH event (edge + cooldown) ----------
bool readTouchTrigger() {
  static uint8_t lastStable = (TOUCH_ACTIVE_LEVEL == HIGH) ? LOW : HIGH;
  static uint8_t lastRead = lastStable;
  static uint32_t lastChangeMs = 0;
  static uint32_t lastFireMs = 0;

  uint8_t raw = digitalRead(TOUCH_PIN);

  if (raw != lastRead) {
    lastRead = raw;
    lastChangeMs = millis();
  }

  if ((millis() - lastChangeMs) > TOUCH_DEBOUNCE_MS && raw != lastStable) {
    uint8_t prev = lastStable;
    lastStable = raw;

    // Fire on edge entering ACTIVE
    if (prev != TOUCH_ACTIVE_LEVEL && lastStable == TOUCH_ACTIVE_LEVEL) {
      if ((millis() - lastFireMs) > TOUCH_COOLDOWN_MS) {
        lastFireMs = millis();
        return true;
      }
    }
  }
  return false;
}

// ---------- Registration (serial wizard) ----------
bool readSerialLine(String& out, uint32_t timeoutMs) {
  out = "";
  uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    serviceBackground();
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') { out.trim(); return true; }
      if ((c >= 32 && c <= 126) || c == '\t') out += c;
    }
    delay(10);
  }
  return false;
}

bool parseUnsignedInt(const String& s, int& outValue) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) if (!isdigit((unsigned char)s[i])) return false;
  long v = s.toInt();
  if (v < 0 || v > 32767) return false;
  outValue = (int)v;
  return true;
}

void printMahasiswaList() {
  Serial.println("\nDaftar mahasiswa:");
  for (size_t i = 0; i < mahasiswaCount; i++) {
    const Mahasiswa& m = mahasiswaList[i];
    Serial.printf("  %u) %s - %s | fp1=%d fp2=%d fp3=%d\n",
                  (unsigned int)(i + 1),
                  m.nim.c_str(),
                  m.nama.c_str(),
                  (int)m.fingerId[0],
                  (int)m.fingerId[1],
                  (int)m.fingerId[2]);
  }
}

int promptMahasiswaIndex() {
  if (!mahasiswaDataReceived) { Serial.println("Data mahasiswa belum diterima dari MQTT topic presence/mahasiswa/catalog."); return -1; }
  if (mahasiswaCount == 0) { Serial.println("Daftar mahasiswa kosong."); return -1; }

  printMahasiswaList();
  Serial.printf("Pilih nomor mahasiswa [1-%u], atau 0 untuk batal:\n", (unsigned int)mahasiswaCount);

  while (true) {
    Serial.print("> ");
    String line;
    if (!readSerialLine(line, SERIAL_INPUT_TIMEOUT_MS)) {
      Serial.println("Timeout input serial, pendaftaran dibatalkan.");
      return -1;
    }
    int chosen = -1;
    if (!parseUnsignedInt(line, chosen)) { Serial.println("Input tidak valid. Masukkan angka."); continue; }
    if (chosen == 0) return -1;
    if (chosen < 1 || chosen > (int)mahasiswaCount) { Serial.println("Nomor mahasiswa di luar range."); continue; }
    return chosen - 1;
  }
}

uint16_t allocateFingerprintId() {
  finger.getTemplateCount();
  uint16_t candidate = (uint16_t)(finger.templateCount + 1);
  if (candidate < nextID) candidate = nextID;
  if (candidate == 0) candidate = 1;
  if (candidate > finger.capacity) return 0;
  return candidate;
}

bool enrollSlotForMahasiswa(Mahasiswa& m, uint8_t slotIdx) {
  if (slotIdx > 2) return false;

  uint16_t enrollId = 0;
  int32_t currentId = m.fingerId[slotIdx];
  if (currentId > 0) {
    enrollId = (uint16_t)currentId;
    if (enrollId > finger.capacity) {
      publishRegistrasiResult(m, (uint8_t)(slotIdx + 1), enrollId, false, "finger_id_di_luar_kapasitas", nullptr);
      return false;
    }
  } else {
    enrollId = allocateFingerprintId();
    if (enrollId == 0) {
      publishRegistrasiResult(m, (uint8_t)(slotIdx + 1), 0, false, "sensor_penuh", nullptr);
      return false;
    }
    m.fingerId[slotIdx] = enrollId;
  }

  uint8_t del = finger.deleteModel(enrollId);
  (void)del;

  bool ok = false;
  for (uint8_t attempt = 1; attempt <= ENROLL_RETRY_PER_SLOT; attempt++) {
    waitFingerRemoved();
    ok = doEnroll(enrollId);
    if (ok) break;
    Serial.println("[WARN] Enroll gagal, coba lagi.");
  }

  if (!ok) {
    publishRegistrasiResult(m, (uint8_t)(slotIdx + 1), enrollId, false, "enroll_gagal_setelah_retry", nullptr);
    return false;
  }

  uint8_t tplBuf[MAX_TEMPLATE_BYTES];
  size_t tplLen = 0;
  String tplB64;
  bool downloadedTemplate = downloadTemplateBytes(enrollId, tplBuf, sizeof(tplBuf), &tplLen);
  bool encodedTemplate = downloadedTemplate && encodeBytesToBase64(tplBuf, tplLen, tplB64);
  bool gotTemplate = downloadedTemplate && encodedTemplate;

  m.fingerId[slotIdx] = enrollId;
  if (enrollId >= nextID) nextID = enrollId + 1;

  if (gotTemplate) {
    publishRegistrasiResult(m, (uint8_t)(slotIdx + 1), enrollId, true, "enroll_sukses", &tplB64);
  } else {
    const char* failMessage = downloadedTemplate ? "enroll_sukses_template_encode_failed" : "enroll_sukses_template_download_failed";
    publishRegistrasiResult(m, (uint8_t)(slotIdx + 1), enrollId, true, failMessage, nullptr);
  }
  return true;
}

void printInfo() {
  finger.getParameters();
  Serial.print("Capacity (maks template): ");
  Serial.println(finger.capacity);

  finger.getTemplateCount();
  Serial.print("Template terisi (sensor): ");
  Serial.println(finger.templateCount);

  uint16_t sensorCandidate = (uint16_t)(finger.templateCount + 1);
  if (sensorCandidate > nextID) nextID = sensorCandidate;
  Serial.print("Next enroll ID (candidate): ");
  Serial.println(nextID);
}

void startRegistrationMode() {
  Serial.println("\n=== MODE PENDAFTARAN MAHASISWA (BTN 1x) ===");
  int mIdx = promptMahasiswaIndex();
  if (mIdx < 0) return;

  Mahasiswa& m = mahasiswaList[mIdx];
  for (uint8_t slotIdx = 0; slotIdx < 3; slotIdx++) {
    bool ok = enrollSlotForMahasiswa(m, slotIdx);
    if (!ok) {
      Serial.println("[FAIL] Pendaftaran dihentikan.");
      printInfo();
      return;
    }
  }
  Serial.println("[OK] Semua fingerprint (1,2,3) berhasil didaftarkan.");
  printInfo();
}

void processPendingRegistration() {
  if (!registrationRequested || registrationInProgress) return;

  registrationRequested = false;
  registrationInProgress = true;
  bool returnToStandby = registrationReturnToStandby;
  registrationReturnToStandby = false;

  Mahasiswa m = pendingRegistration;
  uint8_t slot = pendingRegistrationSlot;
  if (slot < 1 || slot > 3) slot = 1;

  Serial.print("\n=== MQTT REGISTER ");
  Serial.print(m.nim);
  Serial.print(" SLOT ");
  Serial.print(slot);
  Serial.println(" ===");

  publishStatus("register");
  uiShowRegisterPanel(m.nama.c_str(), m.nim.c_str(), "Scan your fingerprint to register");

  bool ok = enrollSlotForMahasiswa(m, (uint8_t)(slot - 1));
  uiShowRegisterPanel(
    m.nama.c_str(),
    m.nim.c_str(),
    ok ? "Registration saved" : "Registration failed"
  );

  delayWithUi(ok ? 1600 : 2200);
  registrationInProgress = false;

  if (returnToStandby) {
    enterStandbyMode(true);
  } else if (syncDone && !standbyMode) {
    uiShowReadyPanel();
    publishStatus("ready");
  } else if (standbyMode) {
    uiShowStandbyPanel();
    publishStatus("standby");
  } else {
    uiShowPreparePanel();
    publishStatus("preparing");
  }
}

bool doScanAttemptOnce() {
  if (!syncDone) {
    Serial.println("[LOCK] Presensi belum aktif (sync belum selesai).");
    beepLocked();
    return false;
  }

  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return false;
  if (p != FINGERPRINT_OK) {
    Serial.print("[FP] getImage err=0x");
    Serial.println(p, HEX);
    return false;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.print("[FP] image2Tz err=0x");
    Serial.println(p, HEX);
    beepFail();
    return true; // attempt terjadi
  }

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    Serial.printf("[OK] MATCH ID=%d confidence=%d\n", finger.fingerID, finger.confidence);

    uint8_t slotIdx = 0;
    const Mahasiswa* matched = findMahasiswaByFingerId(finger.fingerID, &slotIdx);

    bool pubOk = false;
    if (matched != nullptr) pubOk = publishPresensi(finger.fingerID, finger.confidence, matched, (int)(slotIdx + 1));
    else pubOk = publishPresensi(finger.fingerID, finger.confidence, nullptr, -1);

    if (pubOk) beepSuccess(); else beepFail();
    return true;
  }

  if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[FAIL] Sidik jari tidak dikenal");
    beepFail();
    return true;
  }

  Serial.print("[FP] search err=0x");
  Serial.println(p, HEX);
  beepFail();
  return true;
}
static uint8_t lastTouchRaw = 0;
bool touchRisingEdge() {
  uint8_t now = digitalRead(TOUCH_PIN);
  bool rising = (lastTouchRaw != TOUCH_ACTIVE_LEVEL && now == TOUCH_ACTIVE_LEVEL);
  lastTouchRaw = now;
  return rising;
}

static const uint32_t TOUCH_SCAN_START_DELAY_MS = 0;
static const uint32_t TOUCH_SCAN_TIMEOUT_MS     = 2000;
static const uint32_t AFTER_SCAN_COOLDOWN_MS    = 1200;
static uint32_t lastScanMs = 0;

bool isTouchActive() {
  return digitalRead(TOUCH_PIN) == TOUCH_ACTIVE_LEVEL;
}

void doPresensiTouchSession() {
  if ((millis() - lastScanMs) < AFTER_SCAN_COOLDOWN_MS) return;

  Serial.println("\n=== PRESENSI (AUTO SCAN) ===");
  uiShowScanning("Checking session...");

  if (!syncDone) {
    Serial.println("[LOCK] Presensi belum aktif (sync belum selesai).");
    uiShowFail("Device not ready", "Template sync is not complete");
    beepLocked();
    lastScanMs = millis();
    delayWithUi(1200);
    uiShowReadyPanel("Waiting for session data");
    return;
  }

  delayWithUi(TOUCH_SCAN_START_DELAY_MS);
  uiShowScanning("Reading fingerprint...");

  uint8_t p = FINGERPRINT_NOFINGER;
  uint32_t start = millis();

  while ((millis() - start) < TOUCH_SCAN_TIMEOUT_MS) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) break;
    serviceBackground();
    delayWithUi(40);
  }

  if (p != FINGERPRINT_OK) {
    Serial.print("[FAIL] getImage timeout/err code=0x");
    Serial.println(p, HEX);
    uiShowFail("Fingerprint not captured", "Please place your finger again");
    beepFail();
    lastScanMs = millis();
    delayWithUi(1400);
    uiShowReadyPanel();
    return;
  }

  uiShowScanning("Converting image...");
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.print("[FAIL] image2Tz: 0x");
    Serial.println(p, HEX);
    uiShowFail("Image error", "Finger image is not clear");
    beepFail();
    lastScanMs = millis();
    delayWithUi(1400);
    uiShowReadyPanel();
    return;
  }

  uiShowScanning("Matching identity...");
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    Serial.printf("[OK] MATCH ID=%d confidence=%d\n", finger.fingerID, finger.confidence);

    uint8_t slotIdx = 0;
    const Mahasiswa* matched = findMahasiswaByFingerId(finger.fingerID, &slotIdx);

    bool pubOk = false;
    if (matched != nullptr) {
      pubOk = publishPresensi(finger.fingerID, finger.confidence, matched, (int)(slotIdx + 1));
      char detail[64];
      snprintf(detail, sizeof(detail), "Status: Present | Conf: %d", finger.confidence);
      uiShowSuccess(matched->nama.c_str(), matched->nim.c_str(), pubOk ? detail : "MQTT publish failed");
    } else {
      pubOk = publishPresensi(finger.fingerID, finger.confidence, nullptr, -1);
      uiShowSuccess("Unknown student", "-", pubOk ? "Fingerprint matched, catalog missing" : "MQTT publish failed");
    }

    if (pubOk) beepSuccess(); else beepFail();
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[FAIL] Sidik jari tidak dikenal");
    uiShowFail("Fingerprint not recognized", "Please try again");
    beepFail();
  } else {
    Serial.print("[FAIL] Search error: 0x");
    Serial.println(p, HEX);
    uiShowFail("Search error", "Fingerprint module returned error");
    beepFail();
  }

  lastScanMs = millis();
  delayWithUi(2500);
  uiShowReadyPanel();

  Serial.println("[INFO] Tunggu jari diangkat...");
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    serviceBackground();
    delayWithUi(50);
  }
}

void pollFingerprintSensor() {
  if (standbyMode || registrationInProgress || registrationRequested || !syncDone) return;

  uint32_t now = millis();
  if ((now - lastScanMs) < AFTER_SCAN_COOLDOWN_MS) return;
  if ((now - lastFallbackPollMs) < FP_FALLBACK_INTERVAL_MS) return;
  lastFallbackPollMs = now;

  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return;
  if (p != FINGERPRINT_OK) {
    Serial.print("[FP] poll getImage err=0x");
    Serial.println(p, HEX);
    lastAttemptMs = now;
    return;
  }

  lastAttemptMs = now;
  doPresensiTouchSession();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n=== ESP32-S3 R302 + MQTT + EEZ UI ===");

  // LCD + EEZ
  lcdID = LCD_FORCE_ID;
  tft.begin(lcdID);
  tft.setRotation(1);
  // show white background immediately on power-up instead of black
  tft.fillScreen(0xFFFF);
  Serial.print("LCD ID: 0x");
  Serial.println(lcdID, HEX);

  initLvglForMcufriend();
  initEezUi();
  runBootAnimation();

  delayWithUi(100);
  uiShowPreparePanel();
  delayWithUi(200);



  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT_PULLUP);

  // Buzzer init (passive)
  pinMode(BUZZER_PIN, OUTPUT);
  ledcSetup(BUZZER_CH, BUZZER_BASE_FREQ, BUZZER_RES_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_CH);
  ledcWriteTone(BUZZER_CH, 0);

  // Fingerprint init
  uiSensorChecking();
  FPSerial.begin(FP_BAUD, SERIAL_8N1, FP_RX, FP_TX);
  finger.begin(FP_BAUD);

  Serial.printf("UART RX=%d TX=%d baud=%lu\n", FP_RX, FP_TX, (unsigned long)FP_BAUD);
  Serial.printf("Touch GPIO=%d (active=%s)\n", TOUCH_PIN, (TOUCH_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW"));
  Serial.printf("Button GPIO=%d (ke GND saat ditekan)\n", BTN_PIN);
  Serial.printf("Buzzer GPIO=%d\n", BUZZER_PIN);

  if (!finger.verifyPassword()) {
    Serial.println("[FAIL] Sensor tidak terdeteksi. Cek VCC/GND dan RX/TX.");
    uiShowFail("R302 not detected", "Check VCC, GND, TX, RX");
    beepFail();
    while (true) delayWithUi(1000);
  }
  Serial.println("[OK] Sensor terdeteksi");

  uint8_t packetSizeStatus = finger.setPacketSize(FINGERPRINT_PACKET_SIZE_32);
  if (packetSizeStatus != FINGERPRINT_OK) {
    Serial.print("[WARN] setPacketSize(32) gagal: 0x");
    Serial.println(packetSizeStatus, HEX);
  }
  delayWithUi(100);
  finger.getParameters();
  Serial.print("[INFO] Sensor packet_len aktif: ");
  Serial.println((unsigned int)finger.packet_len);
  uiSensorReady();

  // WiFi + MQTT + request sync
  wifiConnect();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setCallback(onMqttMessage);
  connectMQTT();

  resetTemplateSyncState("boot");
  if (CLEAR_SENSOR_TEMPLATES_ON_BOOT) {
    Serial.println("[BOOT] Mengosongkan template sensor...");
    clearAllTemplates();
  }

  uiSensorChecking();
  requestTemplateSync();
  syncExpected = true;
  publishTemplateAck("sync_requested", "", "boot_request");

  printInfo();

  // Kalau backend belum mengirim manifest/chunk, UI tetap siap tapi presensi masih lock oleh syncDone=false.
  // Untuk mode demo tanpa template sync, sementara bisa uncomment baris berikut:
  // syncDone = true;
  // uiShowReadyPanel();

  Serial.println("\nInstruksi:");
  Serial.println("  Tempelkan jari ke sensor -> presensi otomatis");
  // Serial.println("  Klik 1x tombol -> daftar 3 sidik jari mahasiswa via Serial");
}

void loop() {
  serviceBackground();

  processPendingRegistration();
  pollFingerprintSensor();

  if (!standbyMode && !registrationInProgress && readButtonPressedOnce()) {
    startRegistrationMode();
  }

  // Status heartbeat setiap 10 detik
  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs > 10000) {
    lastStatusMs = millis();
    publishStatus(standbyMode ? "standby" : (syncDone ? "ready" : "preparing"));
  }

  delayWithUi(10);
}
