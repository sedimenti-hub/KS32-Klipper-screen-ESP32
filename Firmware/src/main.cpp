#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <CST816S.h>
#include <lvgl.h>
#include "config.h"
#include "icons.h"
#include "icons_large.h"
#include "splash_img.h"

// Memoria non volatile per salvataggio orientamento schermo
Preferences prefs;
uint8_t screen_rotation = 0; // 0=0°, 1=90°, 2=180°, 3=270°

// ==========================================
// HARDWARE INSTANCES
// ==========================================
TFT_eSPI tft = TFT_eSPI();
CST816S touch(TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_RST_PIN, TOUCH_INT_PIN);

// LVGL Buffers (Doppio buffer a 60 linee allineato a 4 byte per massima velocita bus SPI e 60 FPS)
static const uint16_t screenWidth  = SCREEN_WIDTH;
static const uint16_t screenHeight = SCREEN_HEIGHT;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[screenWidth * 60] __attribute__((aligned(4)));
static lv_color_t buf2[screenWidth * 60] __attribute__((aligned(4)));

// Sincronizzazione RTOS e stato UI
SemaphoreHandle_t lvgl_mutex = NULL;
TaskHandle_t moonrakerTaskHandle = NULL;
bool ui_initialized = false;
void init_ui(); // Forward declaration

// ==========================================
// UI OBJECT REFERENCES
// ==========================================
lv_obj_t *tv;
lv_obj_t *tile1;
lv_obj_t *tile2;

// Tile 1 (Dashboard) Status Rings (solo per prima pagina, spessore ridotto del 20%)
#define NUM_STATUS_RINGS 5
lv_obj_t *status_rings[NUM_STATUS_RINGS] = {NULL};
lv_obj_t *status_ring = NULL;
// Telemetria Pagina 1 (al posto del grafico)
lv_obj_t *card_telemetry = NULL;
lv_obj_t *lbl_val_speed = NULL;
lv_obj_t *lbl_val_fan = NULL;
lv_obj_t *lbl_val_accel = NULL;
lv_obj_t *lbl_val_host = NULL;
lv_obj_t *lbl_nozzle = NULL;
lv_obj_t *lbl_bed = NULL;
extern const lv_font_t font_display_18;

// Tacca di stato (Pagina 1) e Modal Controlli Stampa
lv_obj_t *btn_notch = NULL;
lv_obj_t *lbl_notch = NULL;
lv_obj_t *modal_print_ctrl = NULL;
lv_obj_t *lbl_print_filename = NULL;
lv_obj_t *btn_pc_play = NULL;
lv_obj_t *btn_pc_pause = NULL;
lv_obj_t *btn_pc_stop = NULL;
lv_obj_t *btn_pc_restart = NULL;
lv_obj_t *btn_pc_rotate = NULL;
lv_obj_t *lbl_pc_rotate = NULL;
lv_obj_t *btn_pc_emergency = NULL;
lv_obj_t *btn_pc_fw_restart = NULL;
lv_obj_t *btn_pc_shutdown = NULL;
String current_print_filename = "";
void update_print_ctrl_ui();

// Stato stampante per anello esterno con sfumatura
enum PrinterStatusState {
    STATUS_READY,
    STATUS_PRINTING,
    STATUS_ERROR
};
PrinterStatusState current_printer_state = STATUS_ERROR;
bool ring_blink_state = false;
unsigned long last_ring_blink = 0;

// Coefficienti sfumatura (dal perimetro esterno verso l'interno: pre-miscelati con il fondo per zero overhead alpha)
static const uint8_t ring_ratios[NUM_STATUS_RINGS] = { 255, 195, 135, 75, 25 };

void update_status_rings_color(lv_color_t base_color) {
    lv_color_t bg = lv_color_hex(0x0C0D10);
    for (int i = 0; i < NUM_STATUS_RINGS; i++) {
        if (status_rings[i]) {
            lv_color_t c = lv_color_mix(base_color, bg, ring_ratios[i]);
            lv_obj_set_style_border_color(status_rings[i], c, 0);
            lv_obj_set_style_border_opa(status_rings[i], LV_OPA_COVER, 0);
        }
    }
}

void set_printer_state(PrinterStatusState new_state) {
    current_printer_state = new_state;
    if (current_printer_state == STATUS_READY) {
        update_status_rings_color(lv_color_hex(0xB0B0B0));
        if (btn_notch) {
            lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0x4CAF50), 0);
            if (lbl_notch) lv_label_set_text(lbl_notch, "READY");
        }
    } else if (current_printer_state == STATUS_PRINTING) {
        update_status_rings_color(lv_color_hex(0xE53935));
        if (btn_notch) lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0xE53935), 0);
    } else if (current_printer_state == STATUS_ERROR) {
        ring_blink_state = true;
        update_status_rings_color(lv_color_hex(0xFF9800));
        if (btn_notch) {
            lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0xFF9800), 0);
            if (lbl_notch) lv_label_set_text(lbl_notch, "OFFLINE");
        }
    }
    update_print_ctrl_ui();
}

// Modal Impostazione Temperatura (Pagina 1)
lv_obj_t *modal_temp = NULL;
lv_obj_t *lbl_modal_title = NULL;
lv_obj_t *lbl_modal_target = NULL;
bool setting_is_nozzle = true;
int temp_target_val = 200;
float current_ext_target = 0;
float current_bed_target = 0;

// Modal Impostazione Velocità (Speed)
lv_obj_t *modal_speed = NULL;
lv_obj_t *lbl_modal_speed_val = NULL;
int speed_target_val = 100;
float current_speed_val = 0;

// Modal Impostazione Accelerazione (Accel)
lv_obj_t *modal_accel = NULL;
lv_obj_t *lbl_modal_accel_val = NULL;
int accel_target_val = 5000;
float current_accel_val = 0;

// Modal Impostazione Ventola (Fan)
lv_obj_t *modal_fan = NULL;
lv_obj_t *lbl_modal_fan_val = NULL;
lv_obj_t *slider_modal_fan = NULL;

// Subpagine Modali Pagina 2
lv_obj_t *modal_extrude = NULL;
lv_obj_t *modal_move = NULL;
lv_obj_t *modal_calibrate = NULL;
lv_obj_t *modal_accessories = NULL;
lv_obj_t *modal_console = NULL;
lv_obj_t *cont_console_list = NULL;
lv_obj_t *lbl_console_status = NULL;
static lv_obj_t *arc_console_scroll = NULL;
static bool is_console_modal_open = false;
unsigned long last_console_query = 0;
String last_console_text = "";

lv_obj_t *modal_files = NULL;
lv_obj_t *cont_files_list = NULL;
lv_obj_t *lbl_files_status = NULL;
static lv_obj_t *arc_files_scroll = NULL;
volatile bool request_file_list = false;

#define MAX_PRINT_FILES 20
struct PrintFileInfo {
    char name[64];
    double modified;
    size_t size;
};
static PrintFileInfo print_file_list[MAX_PRINT_FILES];
static int print_file_count = 0;

// Parametri Subpagina Estrusione
int extrude_dist = 10;
int extrude_speed_mms = 5;
lv_obj_t *btn_dist[4] = {NULL};
lv_obj_t *btn_speed[3] = {NULL};
const int dist_values[4] = {5, 10, 50, 100};
const int speed_values[3] = {1, 5, 10};

// Subpagina Calibrazione
lv_obj_t *lbl_calib_status = NULL;

// Subpagina Accessori
lv_obj_t *btn_acc_light = NULL;
lv_obj_t *lbl_acc_light = NULL;
lv_obj_t *btn_acc_runout = NULL;
lv_obj_t *lbl_acc_runout = NULL;
lv_obj_t *btn_macro[3] = {NULL};
int current_fan_pct = 0;
bool light_state = false;
bool runout_state = false;
bool is_paused = false;

void update_light_ui(bool state) {
    light_state = state;
    if (lbl_acc_light) {
        lv_label_set_text(lbl_acc_light, light_state ? "LIGHT: ON" : "LIGHT: OFF");
        lv_obj_set_style_text_color(lbl_acc_light, light_state ? lv_color_hex(0xFFB300) : lv_color_hex(0x90A4AE), 0);
    }
    if (btn_acc_light) {
        lv_obj_set_style_bg_color(btn_acc_light, lv_color_hex(0x16181F), 0);
        lv_obj_set_style_border_color(btn_acc_light, light_state ? lv_color_hex(0xFFB300) : lv_color_hex(0x2A2F3D), 0);
    }
}

void update_runout_ui(bool state) {
    runout_state = state;
    if (lbl_acc_runout) {
        lv_label_set_text(lbl_acc_runout, runout_state ? "RUNOUT: ON" : "RUNOUT: OFF");
        lv_obj_set_style_text_color(lbl_acc_runout, runout_state ? lv_color_hex(0x4CAF50) : lv_color_hex(0x90A4AE), 0);
    }
    if (btn_acc_runout) {
        lv_obj_set_style_bg_color(btn_acc_runout, lv_color_hex(0x16181F), 0);
        lv_obj_set_style_border_color(btn_acc_runout, runout_state ? lv_color_hex(0x4CAF50) : lv_color_hex(0x2A2F3D), 0);
    }
}

// Timing variables
unsigned long last_query_time = 0;
const unsigned long QUERY_INTERVAL = 1500; // ms

// ==========================================
// MOONRAKER API CLIENT
// ==========================================
void sendGcode(const String& gcode) {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.setTimeout(800);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/printer/gcode/script";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["script"] = gcode;
    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);
    Serial.printf("[GCODE] '%s' -> HTTP %d\n", gcode.c_str(), httpCode);
    http.end();
}

void printerAction(const String& endpoint) {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.setTimeout(800);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/printer/print/" + endpoint;
    http.begin(url);
    int httpCode = http.POST("");
    Serial.printf("[ACTION] %s -> HTTP %d\n", endpoint.c_str(), httpCode);
    http.end();
}

void emergencyStop() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.setTimeout(800);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/printer/emergency_stop";
    http.begin(url);
    int httpCode = http.POST("");
    Serial.printf("[ACTION] Emergency Stop -> HTTP %d\n", httpCode);
    http.end();
}

void firmwareRestart() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.setTimeout(1200);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/printer/firmware_restart";
    http.begin(url);
    int httpCode = http.POST("");
    Serial.printf("[ACTION] Firmware Restart -> HTTP %d\n", httpCode);
    http.end();
    if (httpCode != HTTP_CODE_OK && httpCode != 200 && httpCode != 204) {
        sendGcode("FIRMWARE_RESTART");
    }
}

void machineShutdown() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.setTimeout(1200);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/machine/shutdown";
    http.begin(url);
    int httpCode = http.POST("");
    Serial.printf("[ACTION] Machine Shutdown -> HTTP %d\n", httpCode);
    http.end();
    if (httpCode != HTTP_CODE_OK && httpCode != 200 && httpCode != 204) {
        sendGcode("SHUTDOWN");
    }
}

void printStartFile(const String& filename) {
    if (WiFi.status() != WL_CONNECTED || filename.length() == 0) return;
    HTTPClient http;
    http.setTimeout(800);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/printer/print/start";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["filename"] = filename;
    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);
    Serial.printf("[PRINT START] %s -> HTTP %d\n", filename.c_str(), httpCode);
    http.end();

    if (httpCode != HTTP_CODE_OK && httpCode != 200 && httpCode != 204) {
        // Fallback G-code per virtual_sdcard di Klipper
        sendGcode("SDCARD_PRINT_FILE FILENAME=\"" + filename + "\"");
    }
}

void queryMoonraker() {
    if (WiFi.status() != WL_CONNECTED) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            set_printer_state(STATUS_ERROR);
            xSemaphoreGive(lvgl_mutex);
        }
        return;
    }

    HTTPClient http;
    http.setTimeout(800);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) +
                 "/printer/objects/query?heater_bed&extruder&print_stats&display_status&fan&motion_report&gcode_move&toolhead&temperature_sensor%20RPi&filament_switch_sensor%20" +
                 String(FILAMENT_SENSOR_NAME) + "&led%20chamber_led";
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            JsonObject status = doc["result"]["status"];

            float ext_temp = status["extruder"]["temperature"] | 0.0f;
            float ext_target = status["extruder"]["target"] | 0.0f;
            float bed_temp = status["heater_bed"]["temperature"] | 0.0f;
            float bed_target = status["heater_bed"]["target"] | 0.0f;
            const char* state_str = status["print_stats"]["state"] | "Offline";
            float progress = status["display_status"]["progress"] | 0.0f;
            int progress_pct = (int)(progress * 100.0f);

            const char* fn = status["print_stats"]["filename"] | "";
            if (fn && strlen(fn) > 0) {
                current_print_filename = String(fn);
            }

            // Aggiorna interfaccia utente con protezione mutex
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {

                // Aggiorna indicatore di stato perimetrale e tacca superiore con colore e percentuale
                String s = String(state_str);
                s.toLowerCase();
                if (s == "printing") {
                    set_printer_state(STATUS_PRINTING);
                    is_paused = false;
                    if (btn_notch && lbl_notch) {
                        lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0xE53935), 0);
                        lv_label_set_text_fmt(lbl_notch, "%d%%", progress_pct);
                    }
                } else if (s == "paused") {
                    set_printer_state(STATUS_PRINTING);
                    is_paused = true;
                    if (btn_notch && lbl_notch) {
                        lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0xEF6C00), 0);
                        lv_label_set_text_fmt(lbl_notch, "P %d%%", progress_pct);
                    }
                } else if (s == "complete") {
                    set_printer_state(STATUS_READY);
                    is_paused = false;
                    if (btn_notch && lbl_notch) {
                        lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0x4CAF50), 0);
                        lv_label_set_text(lbl_notch, "DONE");
                    }
                } else if (s == "error" || s == "shutdown" || s == "offline") {
                    set_printer_state(STATUS_ERROR);
                    is_paused = false;
                    if (btn_notch && lbl_notch) {
                        lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0xFF9800), 0);
                        lv_label_set_text(lbl_notch, "OFFLINE");
                    }
                } else {
                    // "standby", "ready", etc.
                    set_printer_state(STATUS_READY);
                    is_paused = false;
                    if (btn_notch && lbl_notch) {
                        lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0x4CAF50), 0);
                        lv_label_set_text(lbl_notch, "READY");
                    }
                }

                current_ext_target = ext_target;
                current_bed_target = bed_target;

                // Aggiorna telemetria Pagina 1: Velocità, Ventola, Accel, Host
                bool is_printing = (s == "printing");

                // 1. Velocità (in PRINTING: live velocity in tempo reale; in READY: velocità impostata in Klipper)
                float display_speed = 0.0f;
                if (is_printing) {
                    if (status["motion_report"].is<JsonObject>()) {
                        display_speed = status["motion_report"]["live_velocity"] | 0.0f;
                    } else if (status["gcode_move"].is<JsonObject>()) {
                        float g_speed = status["gcode_move"]["speed"] | 0.0f;
                        display_speed = (g_speed > 300.0f) ? (g_speed / 60.0f) : g_speed;
                    }
                } else {
                    // Stato READY: mostra la velocità configurata / impostata in Klipper
                    if (status["toolhead"].is<JsonObject>()) {
                        float max_v = status["toolhead"]["max_velocity"] | 0.0f;
                        float factor = 1.0f;
                        if (status["gcode_move"].is<JsonObject>()) {
                            factor = status["gcode_move"]["speed_factor"] | 1.0f;
                        }
                        display_speed = max_v * factor;
                    }
                    if (display_speed <= 0.0f && status["gcode_move"].is<JsonObject>()) {
                        float g_speed = status["gcode_move"]["speed"] | 0.0f;
                        display_speed = (g_speed > 300.0f) ? (g_speed / 60.0f) : g_speed;
                    }
                }
                current_speed_val = display_speed;
                if (lbl_val_speed) {
                    char s_buf[24];
                    snprintf(s_buf, sizeof(s_buf), "%d", (int)round(display_speed));
                    lv_label_set_text(lbl_val_speed, s_buf);
                }

                // 2. Ventola Pezzo (%)
                if (status["fan"].is<JsonObject>()) {
                    float fan_speed = status["fan"]["speed"] | 0.0f;
                    int fan_pct = (int)round(fan_speed * 100.0f);
                    if (fan_pct < 0) fan_pct = 0;
                    if (fan_pct > 100) fan_pct = 100;
                    current_fan_pct = fan_pct;
                    if (slider_modal_fan) {
                        lv_slider_set_value(slider_modal_fan, fan_pct, LV_ANIM_OFF);
                    }
                    if (lbl_modal_fan_val) {
                        lv_label_set_text_fmt(lbl_modal_fan_val, "%d%%", fan_pct);
                    }
                }
                if (lbl_val_fan) {
                    char f_buf[16];
                    snprintf(f_buf, sizeof(f_buf), "%d%%", current_fan_pct);
                    lv_label_set_text(lbl_val_fan, f_buf);
                }

                // 3. Accelerazione
                float accel_val = 0.0f;
                if (status["toolhead"].is<JsonObject>()) {
                    accel_val = status["toolhead"]["max_accel"] | 0.0f;
                }
                current_accel_val = accel_val;
                if (lbl_val_accel) {
                    char a_buf[24];
                    snprintf(a_buf, sizeof(a_buf), "%d", (int)round(accel_val));
                    lv_label_set_text(lbl_val_accel, a_buf);
                }

                // 4. Temperatura Host (Raspberry Pi / MCU)
                float host_temp = 0.0f;
                if (status["temperature_sensor RPi"].is<JsonObject>()) {
                    host_temp = status["temperature_sensor RPi"]["temperature"] | 0.0f;
                } else if (status["temperature_host RPi"].is<JsonObject>()) {
                    host_temp = status["temperature_host RPi"]["temperature"] | 0.0f;
                } else if (status["temperature_sensor host"].is<JsonObject>()) {
                    host_temp = status["temperature_sensor host"]["temperature"] | 0.0f;
                } else if (status["temperature_sensor raspberry_pi"].is<JsonObject>()) {
                    host_temp = status["temperature_sensor raspberry_pi"]["temperature"] | 0.0f;
                }
                if (lbl_val_host) {
                    char h_buf[20];
                    if (host_temp > 0.0f) {
                        snprintf(h_buf, sizeof(h_buf), "%.1f C", host_temp);
                    } else {
                        snprintf(h_buf, sizeof(h_buf), "-- C");
                    }
                    lv_label_set_text(lbl_val_host, h_buf);
                }

                char temp_buf[32];
                if (lbl_nozzle) {
                    snprintf(temp_buf, sizeof(temp_buf), "%d C", (int)round(ext_temp));
                    if (strcmp(lv_label_get_text(lbl_nozzle), temp_buf) != 0) {
                        lv_label_set_text(lbl_nozzle, temp_buf);
                    }
                }
                if (lbl_bed) {
                    snprintf(temp_buf, sizeof(temp_buf), "%d C", (int)round(bed_temp));
                    if (strcmp(lv_label_get_text(lbl_bed), temp_buf) != 0) {
                        lv_label_set_text(lbl_bed, temp_buf);
                    }
                }

                // Aggiorna stato Sensore Runout all'avvio e ad ogni ciclo
                String sensor_key = "filament_switch_sensor " + String(FILAMENT_SENSOR_NAME);
                String motion_key = "filament_motion_sensor " + String(FILAMENT_SENSOR_NAME);
                if (status[sensor_key].is<JsonObject>()) {
                    bool sensor_enabled = status[sensor_key]["enabled"] | false;
                    update_runout_ui(sensor_enabled);
                } else if (status[motion_key].is<JsonObject>()) {
                    bool sensor_enabled = status[motion_key]["enabled"] | false;
                    update_runout_ui(sensor_enabled);
                }

                // Aggiorna stato Luce all'avvio e ad ogni ciclo
                if (status["led chamber_led"].is<JsonObject>()) {
                    JsonArray color_arr = status["led chamber_led"]["color_data"][0];
                    if (color_arr) {
                        bool led_on = false;
                        for (JsonVariant v : color_arr) {
                            if (v.as<float>() > 0.01f) {
                                led_on = true;
                                break;
                            }
                        }
                        update_light_ui(led_on);
                    }
                }
                xSemaphoreGive(lvgl_mutex);
            }
        } else {
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                set_printer_state(STATUS_ERROR);
                xSemaphoreGive(lvgl_mutex);
            }
        }
    } else {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            set_printer_state(STATUS_ERROR);
            xSemaphoreGive(lvgl_mutex);
        }
    }
    http.end();
}

// ==========================================
// LVGL CALLBACKS & EVENT HANDLERS
// ==========================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // Con LV_COLOR_16_SWAP=1 i byte sono gia formattati per il bus: swap=false (massima velocita hardware)
    tft.pushColors((uint16_t *)&color_p->full, w * h, false);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void map_touch_coordinates(int16_t raw_x, int16_t raw_y, int16_t &mapped_x, int16_t &mapped_y, uint8_t rot) {
    switch (rot % 4) {
        case 0: // 0 gradi
            mapped_x = raw_x;
            mapped_y = raw_y;
            break;
        case 1: // 90 gradi orario (trasformazione inversa per le coordinate touch)
            mapped_x = raw_y;
            mapped_y = 239 - raw_x;
            break;
        case 2: // 180 gradi
            mapped_x = 239 - raw_x;
            mapped_y = 239 - raw_y;
            break;
        case 3: // 270 gradi orario (90 gradi antiorario)
            mapped_x = 239 - raw_y;
            mapped_y = raw_x;
            break;
    }
    if (mapped_x < 0) mapped_x = 0;
    if (mapped_x > 239) mapped_x = 239;
    if (mapped_y < 0) mapped_y = 0;
    if (mapped_y > 239) mapped_y = 239;
}

static int16_t last_touch_x = 0;
static int16_t last_touch_y = 0;
static bool touch_is_down = false;
static uint32_t last_touch_time = 0;

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    if (touch.available()) {
        last_touch_time = millis();
        if (touch.data.points > 0) {
            touch_is_down = true;
            map_touch_coordinates(touch.data.x, touch.data.y, last_touch_x, last_touch_y, screen_rotation);
        } else {
            touch_is_down = false;
        }
    } else if (touch_is_down && (millis() - last_touch_time > 120)) {
        // Rilascio di sicurezza se l'evento di release non e arrivato entro 120ms
        touch_is_down = false;
    }

    if (touch_is_down) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = last_touch_x;
        data->point.y = last_touch_y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// Splash Screen
lv_obj_t *obj_splash = NULL;

void show_splash_screen() {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    obj_splash = lv_obj_create(lv_scr_act());
    lv_obj_set_size(obj_splash, 240, 240);
    lv_obj_center(obj_splash);
    lv_obj_set_style_bg_color(obj_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj_splash, 0, 0);
    lv_obj_set_style_pad_all(obj_splash, 0, 0);
    lv_obj_clear_flag(obj_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img_splash = lv_img_create(obj_splash);
    lv_img_set_src(img_splash, &splash_img);
    lv_obj_center(img_splash);
    lv_obj_move_foreground(obj_splash);
}

// Transizione e dissolvenza Pagina 1 -> Pagina 2
static void tv_scroll_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL && tile2) {
        lv_obj_t *tv_obj = lv_event_get_target(e);
        lv_coord_t scroll_y = lv_obj_get_scroll_y(tv_obj);
        int opa_val = (scroll_y * 255) / 240;
        if (opa_val < 0) opa_val = 0;
        if (opa_val > 255) opa_val = 255;
        lv_obj_set_style_opa(tile2, (lv_opa_t)opa_val, 0);
    }
}

static void event_tab_click(lv_event_t * e) {
    if (tv && tile2) {
        lv_obj_set_tile(tv, tile2, LV_ANIM_ON);
    }
}

// ==========================================
// EVENTI SUBPAGINE E MODALI PAGINA 2
// ==========================================
static void event_close_modal(lv_event_t * e) {
    lv_obj_t *m = (lv_obj_t*)lv_event_get_user_data(e);
    if (m) {
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
        if (m == modal_console) {
            is_console_modal_open = false;
            if (cont_console_list) {
                lv_obj_clean(cont_console_list);
                lbl_console_status = NULL;
            }
            last_console_text = "";
        }
        if (m == modal_files) {
            if (cont_files_list) {
                lv_obj_clean(cont_files_list);
                lbl_files_status = NULL;
            }
        }
    }
}

const char* get_rotation_icon(uint8_t rot) {
    switch (rot % 4) {
        case 0: return LV_SYMBOL_UP;
        case 1: return LV_SYMBOL_RIGHT;
        case 2: return LV_SYMBOL_DOWN;
        case 3: return LV_SYMBOL_LEFT;
    }
    return LV_SYMBOL_UP;
}

const char* get_rotation_text(uint8_t rot) {
    switch (rot % 4) {
        case 0: return "0\xC2\xB0";
        case 1: return "90\xC2\xB0";
        case 2: return "180\xC2\xB0";
        case 3: return "270\xC2\xB0";
    }
    return "0\xC2\xB0";
}

void update_print_ctrl_ui() {
    if (!modal_print_ctrl) return;

    bool is_printing = (current_printer_state == STATUS_PRINTING);

    // I comandi Play, Pausa e Stop vengono visualizzati solo se c'e una stampa in corso
    if (btn_pc_play) {
        if (is_printing) lv_obj_clear_flag(btn_pc_play, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btn_pc_play, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_pc_pause) {
        if (is_printing) lv_obj_clear_flag(btn_pc_pause, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btn_pc_pause, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_pc_stop) {
        if (is_printing) lv_obj_clear_flag(btn_pc_stop, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btn_pc_stop, LV_OBJ_FLAG_HIDDEN);
    }

    if (is_printing) {
        // In stampa: Stop in basso a sinistra, Tasto rotazione in basso a destra
        // Ristampa, Firmware Restart e Shutdown NON devono essere visualizzati durante la stampa
        if (btn_pc_stop) lv_obj_align(btn_pc_stop, LV_ALIGN_CENTER, -42, 44);
        if (btn_pc_restart) lv_obj_add_flag(btn_pc_restart, LV_OBJ_FLAG_HIDDEN);
        if (btn_pc_fw_restart) lv_obj_add_flag(btn_pc_fw_restart, LV_OBJ_FLAG_HIDDEN);
        if (btn_pc_shutdown) lv_obj_add_flag(btn_pc_shutdown, LV_OBJ_FLAG_HIDDEN);
        if (btn_pc_rotate) {
            lv_obj_align(btn_pc_rotate, LV_ALIGN_CENTER, 42, 44);
            lv_obj_clear_flag(btn_pc_rotate, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // Nessuna stampa in corso: Firmware Restart e Shutdown sempre abilitati in alto
        if (btn_pc_fw_restart) {
            lv_obj_clear_flag(btn_pc_fw_restart, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(btn_pc_fw_restart, LV_ALIGN_CENTER, -42, -14);
        }
        if (btn_pc_shutdown) {
            lv_obj_clear_flag(btn_pc_shutdown, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(btn_pc_shutdown, LV_ALIGN_CENTER, 42, -14);
        }

        // Se e presente un file recente: Ristampa (Basso-Sinistra) e Rotazione (Basso-Destra)
        bool has_file = (current_print_filename.length() > 0);
        if (has_file && btn_pc_restart) {
            lv_obj_clear_flag(btn_pc_restart, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(btn_pc_restart, LV_ALIGN_CENTER, -42, 44);
            if (btn_pc_rotate) {
                lv_obj_align(btn_pc_rotate, LV_ALIGN_CENTER, 42, 44);
                lv_obj_clear_flag(btn_pc_rotate, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            // Nessun file recente: Rotazione centrata in basso
            if (btn_pc_restart) lv_obj_add_flag(btn_pc_restart, LV_OBJ_FLAG_HIDDEN);
            if (btn_pc_rotate) {
                lv_obj_align(btn_pc_rotate, LV_ALIGN_CENTER, 0, 44);
                lv_obj_clear_flag(btn_pc_rotate, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (lbl_pc_rotate) {
        lv_label_set_text_fmt(lbl_pc_rotate, "%s\n%s", get_rotation_icon(screen_rotation), get_rotation_text(screen_rotation));
    }
}

static void event_screen_rotate(lv_event_t * e) {
    screen_rotation = (screen_rotation + 1) % 4;
    tft.setRotation(screen_rotation);

    prefs.begin("screen", false);
    prefs.putUChar("rot", screen_rotation);
    prefs.end();

    if (lbl_pc_rotate) {
        lv_label_set_text_fmt(lbl_pc_rotate, "%s\n%s", get_rotation_icon(screen_rotation), get_rotation_text(screen_rotation));
    }

    Serial.printf("[DISPLAY] Nuova rotazione: %d (%s) salvata in memoria\n", screen_rotation, get_rotation_text(screen_rotation));
    lv_obj_invalidate(lv_scr_act());
}

static void event_open_print_ctrl(lv_event_t * e) {
    if (modal_print_ctrl) {
        if (lbl_print_filename) {
            if (current_print_filename.length() > 0) {
                String short_name = current_print_filename;
                int last_slash = short_name.lastIndexOf('/');
                if (last_slash >= 0) short_name = short_name.substring(last_slash + 1);
                lv_label_set_text(lbl_print_filename, short_name.c_str());
            } else {
                lv_label_set_text(lbl_print_filename, "No file");
            }
        }
        update_print_ctrl_ui();
        lv_obj_clear_flag(modal_print_ctrl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_print_ctrl);
    }
}

static void event_print_play(lv_event_t * e) {
    printerAction("resume");
}

static void event_print_pause(lv_event_t * e) {
    printerAction("pause");
}

static void event_print_stop(lv_event_t * e) {
    printerAction("cancel");
}

static void event_emergency_stop(lv_event_t * e) {
    emergencyStop();
    sendGcode("M112");
    if (modal_print_ctrl) {
        lv_obj_add_flag(modal_print_ctrl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void event_firmware_restart(lv_event_t * e) {
    firmwareRestart();
    if (modal_print_ctrl) {
        lv_obj_add_flag(modal_print_ctrl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void event_machine_shutdown(lv_event_t * e) {
    machineShutdown();
    if (modal_print_ctrl) {
        lv_obj_add_flag(modal_print_ctrl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void event_print_restart(lv_event_t * e) {
    if (current_print_filename.length() > 0) {
        printStartFile(current_print_filename);
    }
}

static void event_open_extrude(lv_event_t * e) {
    if (modal_extrude) lv_obj_clear_flag(modal_extrude, LV_OBJ_FLAG_HIDDEN);
}

static void event_open_move(lv_event_t * e) {
    if (modal_move) lv_obj_clear_flag(modal_move, LV_OBJ_FLAG_HIDDEN);
}

static void event_open_calibrate(lv_event_t * e) {
    if (modal_calibrate) {
        if (lbl_calib_status) lv_label_set_text(lbl_calib_status, "");
        lv_obj_clear_flag(modal_calibrate, LV_OBJ_FLAG_HIDDEN);
    }
}

static void event_open_accessories(lv_event_t * e) {
    if (modal_accessories) lv_obj_clear_flag(modal_accessories, LV_OBJ_FLAG_HIDDEN);
}

// ==========================================
// SUBPAGINA MODALE CONSOLE (AUTO-REFRESH 0.5s IN BACKGROUND)
// ==========================================
// Helper per filtrare i messaggi di boot/handshake iniziali di Klipper
static bool is_klipper_boot_message(const String& s) {
    if (s.isEmpty()) return true;
    if (s.equalsIgnoreCase("M115")) return true;
    if (s.equalsIgnoreCase("RESTART") || s.equalsIgnoreCase("FIRMWARE_RESTART")) return true;
    if (s.indexOf("FIRMWARE_NAME:") >= 0) return true;
    if (s.startsWith("// Klipper state: Disconnect")) return true;
    if (s.startsWith("// Klipper state: Ready")) return true;
    if (s.startsWith("// Klipper state: Startup")) return true;
    if (s.startsWith("// Klipper state: Disconnected")) return true;
    if (s.indexOf("printer is ready") >= 0 || s.indexOf("Printer is ready") >= 0) return true;
    return false;
}

static void update_console_curvature() {
    if (!cont_console_list) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(cont_console_list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(cont_console_list, i);
        if (!child) continue;

        if (child == lbl_console_status) {
            lv_obj_set_style_translate_x(child, 0, 0);
            continue;
        }

        // Coordinate a schermo dell'elemento
        lv_area_t area;
        lv_obj_get_coords(child, &area);

        // Centro verticale dell'elemento rispetto al centro del display (120, 120)
        lv_coord_t y_mid = (area.y1 + area.y2) / 2;
        int dy = abs(y_mid - 120);
        if (dy > 116) dy = 116;

        // Equazione della circonferenza: x_curva = R - sqrt(R^2 - dy^2) con R = 120
        int delta = 14400 - (dy * dy);
        if (delta < 0) delta = 0;
        int root = (int)sqrtf((float)delta);
        int x_curve = 120 - root;

        // Margine estetico dal bordo sinistro del display circolare
        int target_x = x_curve + 10;

        lv_coord_t cur_tr = lv_obj_get_style_translate_x(child, 0);
        if (cur_tr != target_x) {
            lv_obj_set_style_translate_x(child, target_x, 0);
        }
    }
}

static void event_console_scroll(lv_event_t * e) {
    if (!cont_console_list) return;

    // 1. Allinea e adatta dinamicamente gli elementi alla curvatura del display durante lo scroll
    update_console_curvature();

    // 2. Aggiorna l'indicatore di scorrimento ad arco sul bordo destro
    if (arc_console_scroll) {
        lv_coord_t top = lv_obj_get_scroll_top(cont_console_list);
        lv_coord_t bottom = lv_obj_get_scroll_bottom(cont_console_list);
        lv_coord_t total = top + bottom;
        int pct = 0;
        if (total > 0) {
            pct = (top * 100) / total;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        uint16_t a_start = (pct * 65) / 100;
        uint16_t a_end = a_start + 25;
        lv_arc_set_angles(arc_console_scroll, a_start, a_end);
    }
}

void bg_fetch_console() {
    if (WiFi.status() != WL_CONNECTED) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (cont_console_list) {
                lv_obj_clean(cont_console_list);
                lbl_console_status = lv_label_create(cont_console_list);
                lv_obj_set_style_text_font(lbl_console_status, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(lbl_console_status, lv_color_hex(0xEF5350), 0);
                lv_obj_set_style_text_align(lbl_console_status, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(lbl_console_status, 240);
                lv_label_set_text(lbl_console_status, "Offline - No Wi-Fi");
            }
            xSemaphoreGive(lvgl_mutex);
        }
        return;
    }

    HTTPClient http;
    http.setTimeout(1000);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/server/gcode_store?count=30";
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            JsonArray arr = doc["result"]["gcode_store"];
            Serial.printf("[CONSOLE] Raw gcode_store items: %u\n", (unsigned int)arr.size());
            #define MAX_CONSOLE_MSGS 20
            String msgs[MAX_CONSOLE_MSGS];
            int msg_count = 0;

            for (JsonObject item : arr) {
                const char* raw_msg = item["message"] | "";
                String msg_str = String(raw_msg);
                msg_str.trim();

                if (msg_str.length() == 0) continue;
                if (is_klipper_boot_message(msg_str)) continue;

                if (msg_count < MAX_CONSOLE_MSGS) {
                    msgs[msg_count++] = msg_str;
                } else {
                    for (int k = 0; k < MAX_CONSOLE_MSGS - 1; k++) {
                        msgs[k] = msgs[k + 1];
                    }
                    msgs[MAX_CONSOLE_MSGS - 1] = msg_str;
                }
            }

            // Calcola signature per rilevare se ci sono nuovi messaggi reali
            String new_sig = "";
            for (int i = 0; i < msg_count; i++) {
                new_sig += msgs[i];
                new_sig += "\n";
            }
            if (new_sig.length() == 0) {
                new_sig = "__EMPTY__";
            }

            // Aggiorna la UI solo se i messaggi sono cambiati (permette uno scroll touch fluido)
            if (new_sig != last_console_text) {
                Serial.printf("[CONSOLE] New content detected (%d msgs). Acquiring mutex...\n", msg_count);
                if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    last_console_text = new_sig;
                    if (cont_console_list && is_console_modal_open) {
                        lv_obj_clean(cont_console_list);
                        lbl_console_status = NULL;

                        if (msg_count == 0) {
                            lbl_console_status = lv_label_create(cont_console_list);
                            lv_obj_set_style_text_font(lbl_console_status, &lv_font_montserrat_12, 0);
                            lv_obj_set_style_text_color(lbl_console_status, lv_color_hex(0x90A4AE), 0);
                            lv_obj_set_style_text_align(lbl_console_status, LV_TEXT_ALIGN_CENTER, 0);
                            lv_obj_set_width(lbl_console_status, 240);
                            lv_label_set_text(lbl_console_status, "Console vuota");
                        } else {
                            for (int i = 0; i < msg_count; i++) {
                                String m = msgs[i];

                                const char* ico_sym = LV_SYMBOL_RIGHT;
                                lv_color_t ico_col = lv_color_hex(0xBA68C8); // Violetto accento console
                                lv_color_t text_col = lv_color_hex(0xCFD8DC);

                                if (m.startsWith("!!")) {
                                    ico_sym = LV_SYMBOL_WARNING;
                                    ico_col = lv_color_hex(0xEF5350); // Rosso errore
                                    text_col = lv_color_hex(0xFF8A80);
                                    m = m.substring(2);
                                    m.trim();
                                } else if (m.startsWith("//") || m.startsWith("echo:")) {
                                    ico_sym = LV_SYMBOL_RIGHT;
                                    ico_col = lv_color_hex(0x81C784); // Verde responso Klipper
                                    text_col = lv_color_hex(0xA5D6A7);
                                    if (m.startsWith("//")) {
                                        m = m.substring(2);
                                        m.trim();
                                    }
                                } else if (m.startsWith("File") || m.indexOf(".gcode") >= 0) {
                                    ico_sym = LV_SYMBOL_FILE;
                                    ico_col = lv_color_hex(0x4FC3F7); // Azzurro file
                                    text_col = lv_color_hex(0xB3E5FC);
                                }

                                if (m.length() > 140) {
                                    m = m.substring(0, 137) + "...";
                                }

                                lv_obj_t *card = lv_obj_create(cont_console_list);
                                lv_obj_set_width(card, 172);
                                lv_obj_set_height(card, LV_SIZE_CONTENT);
                                lv_obj_set_style_min_height(card, 26, 0);
                                lv_obj_set_style_radius(card, 10, 0);
                                lv_obj_set_style_bg_color(card, lv_color_hex(0x1E242C), 0);
                                lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
                                lv_obj_set_style_border_color(card, lv_color_hex(0x28303C), 0);
                                lv_obj_set_style_border_width(card, 1, 0);
                                lv_obj_set_style_pad_hor(card, 8, 0);
                                lv_obj_set_style_pad_ver(card, 5, 0);
                                lv_obj_set_style_shadow_width(card, 0, 0);
                                lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
                                lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLL_CHAIN);
                                lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

                                // Icona
                                lv_obj_t *ico = lv_label_create(card);
                                lv_label_set_text(ico, ico_sym);
                                lv_obj_set_style_text_font(ico, &lv_font_montserrat_12, 0);
                                lv_obj_set_style_text_color(ico, ico_col, 0);
                                lv_obj_align(ico, LV_ALIGN_TOP_LEFT, 0, 1);
                                lv_obj_clear_flag(ico, LV_OBJ_FLAG_CLICKABLE);

                                // Testo messaggio
                                lv_obj_t *lbl = lv_label_create(card);
                                lv_obj_set_width(lbl, 140);
                                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
                                lv_obj_set_style_text_color(lbl, text_col, 0);
                                lv_label_set_text(lbl, m.c_str());
                                lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 16, 0);
                                lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
                            }

                            lv_obj_update_layout(cont_console_list);
                            update_console_curvature();
                            lv_obj_scroll_to_y(cont_console_list, LV_COORD_MAX, LV_ANIM_OFF);
                            update_console_curvature();
                        }

                        if (arc_console_scroll) {
                            lv_coord_t top = lv_obj_get_scroll_top(cont_console_list);
                            lv_coord_t bottom = lv_obj_get_scroll_bottom(cont_console_list);
                            lv_coord_t total = top + bottom;
                            int pct = 0;
                            if (total > 0) {
                                pct = (top * 100) / total;
                                if (pct < 0) pct = 0;
                                if (pct > 100) pct = 100;
                            }
                            uint16_t a_start = (pct * 65) / 100;
                            uint16_t a_end = a_start + 25;
                            lv_arc_set_angles(arc_console_scroll, a_start, a_end);
                        }
                    }
                    xSemaphoreGive(lvgl_mutex);
                    Serial.println("[CONSOLE] UI updated successfully, mutex released.");
                } else {
                    Serial.println("[CONSOLE] WARNING: Mutex timeout (500ms)!");
                }
            }
        } else {
            Serial.printf("[CONSOLE] JSON deserialization error: %s\n", err.c_str());
        }
    } else {
        http.end();
        Serial.printf("[CONSOLE] HTTP error: %d\n", httpCode);
    }
}

static void event_open_console(lv_event_t * e) {
    Serial.println("[CONSOLE] >>> event_open_console called!");
    if (modal_files) {
        lv_obj_add_flag(modal_files, LV_OBJ_FLAG_HIDDEN);
        if (cont_files_list) {
            lv_obj_clean(cont_files_list);
            lbl_files_status = NULL;
        }
    }
    if (modal_console) {
        lv_obj_clear_flag(modal_console, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_console);
        is_console_modal_open = true;
        last_console_query = 0; // Trigger immediato al prossimo ciclo
        last_console_text = "";  // Forza il refresh del testo
        if (cont_console_list) {
            lv_obj_clean(cont_console_list);
            lbl_console_status = lv_label_create(cont_console_list);
            lv_obj_set_style_text_font(lbl_console_status, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl_console_status, lv_color_hex(0x90A4AE), 0);
            lv_obj_set_style_text_align(lbl_console_status, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lbl_console_status, 240);
            lv_label_set_text(lbl_console_status, "Connessione console...");
        }
        if (arc_console_scroll) {
            lv_arc_set_angles(arc_console_scroll, 0, 25);
        }
    }
}

// ==========================================
// SUBPAGINA MODALE FILE DI STAMPA (ASYNC IN BACKGROUND)
// ==========================================
static void event_click_file(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < print_file_count) {
        String fname = String(print_file_list[idx].name);
        if (current_printer_state == STATUS_PRINTING) {
            Serial.printf("[FILE LIST] Printer is busy, ignoring request for: %s\n", fname.c_str());
            return;
        }

        Serial.printf("[FILE LIST] Clicked file [%d]: %s -> Starting print\n", idx, fname.c_str());
        current_print_filename = fname;
        if (lbl_print_filename) lv_label_set_text(lbl_print_filename, fname.c_str());

        // Invia comando di avvio stampa a Moonraker
        printStartFile(fname);

        // Chiudi modale file e torna alla schermata principale Dashboard
        if (modal_files) {
            lv_obj_add_flag(modal_files, LV_OBJ_FLAG_HIDDEN);
            if (cont_files_list) {
                lv_obj_clean(cont_files_list);
                lbl_files_status = NULL;
            }
        }
        if (tv) lv_obj_set_tile_id(tv, 0, 0, LV_ANIM_ON);
    }
}

// Helper per formattare la dimensione file stile smartwatch UI
static void format_file_size(size_t sz, char* out, size_t max_len) {
    if (sz >= 1048576) {
        float mb = (float)sz / (1024.0f * 1024.0f);
        snprintf(out, max_len, "(%.1f MB)", mb);
    } else if (sz >= 1024) {
        float kb = (float)sz / 1024.0f;
        if (kb >= 10.0f) {
            snprintf(out, max_len, "(%d KB)", (int)kb);
        } else {
            snprintf(out, max_len, "(%.1f KB)", kb);
        }
    } else if (sz > 0) {
        snprintf(out, max_len, "(%u B)", (unsigned int)sz);
    } else {
        out[0] = '\0';
    }
}

static void get_file_display_text(const char* name, size_t sz, char* out, size_t max_len) {
    char clean_name[32];
    size_t len = strlen(name);
    if (len > 16) {
        strncpy(clean_name, name, 13);
        clean_name[13] = '\0';
        strcat(clean_name, "...");
    } else {
        strncpy(clean_name, name, sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0';
    }

    char sz_buf[16];
    format_file_size(sz, sz_buf, sizeof(sz_buf));

    if (strlen(sz_buf) > 0) {
        snprintf(out, max_len, "#ffffff %s# #8a96a0 %s#", clean_name, sz_buf);
    } else {
        snprintf(out, max_len, "#ffffff %s#", clean_name);
    }
}

static void update_files_curvature() {
    if (!cont_files_list) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(cont_files_list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(cont_files_list, i);
        if (!child) continue;

        if (child == lbl_files_status) {
            lv_obj_set_style_translate_x(child, 0, 0);
            continue;
        }

        // Coordinate a schermo dell'elemento
        lv_area_t area;
        lv_obj_get_coords(child, &area);

        // Centro verticale dell'elemento rispetto al centro del display (120, 120)
        lv_coord_t y_mid = (area.y1 + area.y2) / 2;
        int dy = abs(y_mid - 120);
        if (dy > 116) dy = 116;

        // Equazione della circonferenza: x_curva = R - sqrt(R^2 - dy^2) con R = 120
        int delta = 14400 - (dy * dy);
        if (delta < 0) delta = 0;
        int root = (int)sqrtf((float)delta);
        int x_curve = 120 - root;

        // Margine estetico dal bordo sinistro del display circolare
        int target_x = x_curve + 10;

        lv_coord_t cur_tr = lv_obj_get_style_translate_x(child, 0);
        if (cur_tr != target_x) {
            lv_obj_set_style_translate_x(child, target_x, 0);
        }
    }
}

static void event_files_scroll(lv_event_t * e) {
    if (!cont_files_list) return;

    // 1. Allinea e adatta dinamicamente gli elementi alla curvatura del display durante lo scroll
    update_files_curvature();

    // 2. Aggiorna l'indicatore di scorrimento ad arco sul bordo destro
    if (arc_files_scroll) {
        lv_coord_t top = lv_obj_get_scroll_top(cont_files_list);
        lv_coord_t bottom = lv_obj_get_scroll_bottom(cont_files_list);
        lv_coord_t total = top + bottom;
        int pct = 0;
        if (total > 0) {
            pct = (top * 100) / total;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        uint16_t a_start = (pct * 65) / 100;
        uint16_t a_end = a_start + 25;
        lv_arc_set_angles(arc_files_scroll, a_start, a_end);
    }
}

void bg_fetch_files() {
    Serial.println("[FILES] >>> Starting bg_fetch_files in background task...");
    Serial.printf("[FILES] Free heap: %u, Stack watermark: %u\n", (unsigned int)ESP.getFreeHeap(), (unsigned int)uxTaskGetStackHighWaterMark(NULL));

    if (WiFi.status() != WL_CONNECTED) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cont_files_list) {
                lv_obj_clean(cont_files_list);
                lbl_files_status = lv_label_create(cont_files_list);
                lv_obj_set_style_text_font(lbl_files_status, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(lbl_files_status, lv_color_hex(0xEF5350), 0);
                lv_obj_set_style_text_align(lbl_files_status, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(lbl_files_status, 240);
                lv_label_set_text(lbl_files_status, "Offline - No Wi-Fi");
            }
            xSemaphoreGive(lvgl_mutex);
        }
        return;
    }

    HTTPClient http;
    http.setTimeout(4000);
    String url = "http://" + String(MOONRAKER_HOST) + ":" + String(MOONRAKER_PORT) + "/server/files/list?root=gcodes";
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        WiFiClient *stream = http.getStreamPtr();

        JsonDocument filter;
        filter["result"][0]["path"] = true;
        filter["result"][0]["modified"] = true;
        filter["result"][0]["size"] = true;
        filter["result"]["*"]["path"] = true;
        filter["result"]["*"]["modified"] = true;
        filter["result"]["*"]["size"] = true;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));
        http.end();
        Serial.printf("[FILES] JSON stream parsed: %s, free heap: %u\n", err ? err.c_str() : "OK", (unsigned int)ESP.getFreeHeap());

        if (!err) {
            JsonArray arr = doc["result"];
            print_file_count = 0;

            for (JsonObject item : arr) {
                const char* p = item["path"] | "";
                double m = item["modified"] | 0.0;
                size_t sz = item["size"] | (size_t)0;
                if (strlen(p) == 0) continue;

                // Solo file gcode validi
                String sp = String(p);
                if (!sp.endsWith(".gcode") && !sp.endsWith(".gco") && !sp.endsWith(".g")) continue;

                // Inserimento ordinato per data modified decrescente (più recenti in cima)
                if (print_file_count < MAX_PRINT_FILES) {
                    int pos = print_file_count;
                    while (pos > 0 && print_file_list[pos - 1].modified < m) {
                        print_file_list[pos] = print_file_list[pos - 1];
                        pos--;
                    }
                    strncpy(print_file_list[pos].name, p, sizeof(print_file_list[pos].name) - 1);
                    print_file_list[pos].name[sizeof(print_file_list[pos].name) - 1] = '\0';
                    print_file_list[pos].modified = m;
                    print_file_list[pos].size = sz;
                    print_file_count++;
                } else {
                    if (m > print_file_list[MAX_PRINT_FILES - 1].modified) {
                        int pos = MAX_PRINT_FILES - 1;
                        while (pos > 0 && print_file_list[pos - 1].modified < m) {
                            print_file_list[pos] = print_file_list[pos - 1];
                            pos--;
                        }
                        strncpy(print_file_list[pos].name, p, sizeof(print_file_list[pos].name) - 1);
                        print_file_list[pos].name[sizeof(print_file_list[pos].name) - 1] = '\0';
                        print_file_list[pos].modified = m;
                        print_file_list[pos].size = sz;
                    }
                }
            }

            Serial.printf("[FILES] Selected top %d files, taking lvgl_mutex...\n", print_file_count);

            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                Serial.println("[FILES] Mutex acquired, building UI...");
                if (cont_files_list) {
                    lv_obj_clean(cont_files_list);
                    lbl_files_status = NULL;
                    if (print_file_count == 0) {
                        lbl_files_status = lv_label_create(cont_files_list);
                        lv_obj_set_style_text_font(lbl_files_status, &lv_font_montserrat_12, 0);
                        lv_obj_set_style_text_color(lbl_files_status, lv_color_hex(0x90A4AE), 0);
                        lv_obj_set_style_text_align(lbl_files_status, LV_TEXT_ALIGN_CENTER, 0);
                        lv_obj_set_width(lbl_files_status, 240);
                        lv_label_set_text(lbl_files_status, "Nessun file G-code");
                    } else {
                        for (int i = 0; i < print_file_count; i++) {
                            lv_obj_t *btn = lv_btn_create(cont_files_list);
                            lv_obj_set_size(btn, 172, 28);
                            lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
                            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E242C), 0);
                            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
                            lv_obj_set_style_bg_color(btn, lv_color_hex(0x354050), LV_STATE_PRESSED);
                            lv_obj_set_style_border_width(btn, 0, 0);
                            lv_obj_set_style_pad_hor(btn, 8, 0);
                            lv_obj_set_style_pad_ver(btn, 0, 0);
                            lv_obj_set_style_shadow_width(btn, 0, 0);
                            lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
                            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
                            lv_obj_add_event_cb(btn, event_click_file, LV_EVENT_CLICKED, (void*)(intptr_t)i);

                            // Icona File azzurro cielo
                            lv_obj_t *ico = lv_label_create(btn);
                            lv_label_set_text(ico, LV_SYMBOL_FILE);
                            lv_obj_set_style_text_font(ico, &lv_font_montserrat_12, 0);
                            lv_obj_set_style_text_color(ico, lv_color_hex(0x4FC3F7), 0);
                            lv_obj_align(ico, LV_ALIGN_LEFT_MID, 0, 0);
                            lv_obj_clear_flag(ico, LV_OBJ_FLAG_CLICKABLE);

                            // Nome File (bianco) + Dimensione (grigio)
                            lv_obj_t *lbl = lv_label_create(btn);
                            lv_label_set_recolor(lbl, true);
                            char disp_buf[96];
                            get_file_display_text(print_file_list[i].name, print_file_list[i].size, disp_buf, sizeof(disp_buf));
                            lv_label_set_text(lbl, disp_buf);
                            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
                            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 18, 0);
                            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
                        }

                        Serial.println("[FILES] Updating layout...");
                        lv_obj_update_layout(cont_files_list);
                        Serial.println("[FILES] Updating curvature...");
                        update_files_curvature();
                        Serial.println("[FILES] Curvature updated!");
                    }
                    if (arc_files_scroll) {
                        lv_arc_set_angles(arc_files_scroll, 0, 25);
                    }
                }
                xSemaphoreGive(lvgl_mutex);
                Serial.println("[FILES] Mutex released, bg_fetch_files finished!");
            } else {
                Serial.println("[FILES] WARNING: Could not take lvgl_mutex (timeout)!");
            }
        } else {
            Serial.printf("[FILES] JSON deserialize error: %s\n", err.c_str());
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (cont_files_list) {
                    lv_obj_clean(cont_files_list);
                    lbl_files_status = lv_label_create(cont_files_list);
                    lv_obj_set_style_text_font(lbl_files_status, &lv_font_montserrat_12, 0);
                    lv_obj_set_style_text_color(lbl_files_status, lv_color_hex(0xEF5350), 0);
                    lv_obj_set_style_text_align(lbl_files_status, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_set_width(lbl_files_status, 240);
                    lv_label_set_text(lbl_files_status, "Errore JSON");
                }
                xSemaphoreGive(lvgl_mutex);
            }
        }
    } else {
        http.end();
        Serial.printf("[FILES] HTTP error: %d\n", httpCode);
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cont_files_list) {
                lv_obj_clean(cont_files_list);
                lbl_files_status = lv_label_create(cont_files_list);
                lv_obj_set_style_text_font(lbl_files_status, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(lbl_files_status, lv_color_hex(0xEF5350), 0);
                lv_obj_set_style_text_align(lbl_files_status, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(lbl_files_status, 240);
                char errBuf[32];
                snprintf(errBuf, sizeof(errBuf), "Errore HTTP %d", httpCode);
                lv_label_set_text(lbl_files_status, errBuf);
            }
            xSemaphoreGive(lvgl_mutex);
        }
    }
}

static void event_open_files(lv_event_t * e) {
    Serial.println("[FILES] >>> event_open_files called!");
    if (modal_console) {
        lv_obj_add_flag(modal_console, LV_OBJ_FLAG_HIDDEN);
        is_console_modal_open = false;
        if (cont_console_list) {
            lv_obj_clean(cont_console_list);
            lbl_console_status = NULL;
        }
        last_console_text = "";
    }
    if (modal_files) {
        lv_obj_clear_flag(modal_files, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_files);
        if (cont_files_list) {
            lv_obj_clean(cont_files_list);
            lbl_files_status = lv_label_create(cont_files_list);
            lv_obj_set_style_text_font(lbl_files_status, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl_files_status, lv_color_hex(0x90A4AE), 0);
            lv_obj_set_style_text_align(lbl_files_status, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lbl_files_status, 240);
            lv_label_set_text(lbl_files_status, "Caricamento file...");
        }
        if (arc_files_scroll) {
            lv_arc_set_angles(arc_files_scroll, 0, 25);
        }
        request_file_list = true;
        Serial.println("[FILES] request_file_list set to true");
    }
}


// Subpagina Estrusione
static void update_extrude_dist_ui() {
    for (int i = 0; i < 4; i++) {
        if (!btn_dist[i]) continue;
        lv_obj_t *lbl = lv_obj_get_child(btn_dist[i], 0);
        if (dist_values[i] == extrude_dist) {
            lv_obj_set_style_bg_color(btn_dist[i], lv_color_hex(0x1A2838), 0);
            lv_obj_set_style_border_color(btn_dist[i], lv_color_hex(0x00BCD4), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), 0);
        } else {
            lv_obj_set_style_bg_color(btn_dist[i], lv_color_hex(0x16181F), 0);
            lv_obj_set_style_border_color(btn_dist[i], lv_color_hex(0x2A2F3D), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x90A4AE), 0);
        }
    }
}

static void update_extrude_speed_ui() {
    for (int i = 0; i < 3; i++) {
        if (!btn_speed[i]) continue;
        lv_obj_t *lbl = lv_obj_get_child(btn_speed[i], 0);
        if (speed_values[i] == extrude_speed_mms) {
            lv_obj_set_style_bg_color(btn_speed[i], lv_color_hex(0x1A2838), 0);
            lv_obj_set_style_border_color(btn_speed[i], lv_color_hex(0x00BCD4), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), 0);
        } else {
            lv_obj_set_style_bg_color(btn_speed[i], lv_color_hex(0x16181F), 0);
            lv_obj_set_style_border_color(btn_speed[i], lv_color_hex(0x2A2F3D), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x90A4AE), 0);
        }
    }
}

static void event_select_dist(lv_event_t * e) {
    int val = (int)(intptr_t)lv_event_get_user_data(e);
    extrude_dist = val;
    update_extrude_dist_ui();
}

static void event_select_speed(lv_event_t * e) {
    int val = (int)(intptr_t)lv_event_get_user_data(e);
    extrude_speed_mms = val;
    update_extrude_speed_ui();
}

static void event_action_extrude(lv_event_t * e) {
    int feedrate = extrude_speed_mms * 60;
    sendGcode("M83\nG1 E" + String(extrude_dist) + " F" + String(feedrate) + "\nG90");
}

static void event_action_retract(lv_event_t * e) {
    int feedrate = extrude_speed_mms * 60;
    sendGcode("M83\nG1 E-" + String(extrude_dist) + " F" + String(feedrate) + "\nG90");
}

// Subpagina Movimento (Assi XYZ)
static void event_jog_xp(lv_event_t *e) { sendGcode("G91\nG1 X" + String(JOG_STEP_XY) + " F" + String(JOG_FEEDRATE_XY) + "\nG90"); }
static void event_jog_xm(lv_event_t *e) { sendGcode("G91\nG1 X-" + String(JOG_STEP_XY) + " F" + String(JOG_FEEDRATE_XY) + "\nG90"); }
static void event_jog_yp(lv_event_t *e) { sendGcode("G91\nG1 Y" + String(JOG_STEP_XY) + " F" + String(JOG_FEEDRATE_XY) + "\nG90"); }
static void event_jog_ym(lv_event_t *e) { sendGcode("G91\nG1 Y-" + String(JOG_STEP_XY) + " F" + String(JOG_FEEDRATE_XY) + "\nG90"); }
static void event_jog_zp(lv_event_t *e) { sendGcode("G91\nG1 Z" + String(JOG_STEP_Z) + " F" + String(JOG_FEEDRATE_Z) + "\nG90"); }
static void event_jog_zm(lv_event_t *e) { sendGcode("G91\nG1 Z-" + String(JOG_STEP_Z) + " F" + String(JOG_FEEDRATE_Z) + "\nG90"); }
static void event_home_all(lv_event_t *e) { sendGcode("G28"); }

// Subpagina Calibrazione
static void event_calib_bed(lv_event_t *e) {
    sendGcode(GCODE_BED_LEVEL);
    if (lbl_calib_status) lv_label_set_text(lbl_calib_status, "Starting Bed Mesh...");
}

static void event_calib_shaper(lv_event_t *e) {
    sendGcode(GCODE_RESONANCE);
    if (lbl_calib_status) lv_label_set_text(lbl_calib_status, "Starting Input Shaper...");
}

// Subpagina Accessori
static void event_acc_light(lv_event_t *e) {
    bool new_state = !light_state;
    if (new_state) {
        sendGcode(GCODE_LIGHT_ON);
    } else {
        sendGcode(GCODE_LIGHT_OFF);
    }
    update_light_ui(new_state);
}

static void event_acc_runout(lv_event_t *e) {
    bool new_state = !runout_state;
    sendGcode("SET_FILAMENT_SENSOR SENSOR=" + String(FILAMENT_SENSOR_NAME) + " ENABLE=" + String(new_state ? 1 : 0));
    update_runout_ui(new_state);
}

void apply_fan_pct(int val) {
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    current_fan_pct = val;
    int pwm = (val * 255) / 100;
    sendGcode("M106 S" + String(pwm));
    if (lbl_val_fan) {
        lv_label_set_text_fmt(lbl_val_fan, "%d%%", val);
    }
    if (lbl_modal_fan_val) {
        lv_label_set_text_fmt(lbl_modal_fan_val, "%d%%", val);
    }
    if (slider_modal_fan) {
        lv_slider_set_value(slider_modal_fan, val, LV_ANIM_OFF);
    }
    Serial.printf("[FAN] Apply -> %d%% (PWM %d)\n", val, pwm);
}

// Event handlers per i 3 pulsanti Macro nella subpagina Accessori
static void event_macro_1(lv_event_t *e) {
    sendGcode(GCODE_MACRO_1);
    Serial.printf("[MACRO] M1 -> %s\n", GCODE_MACRO_1);
}

static void event_macro_2(lv_event_t *e) {
    sendGcode(GCODE_MACRO_2);
    Serial.printf("[MACRO] M2 -> %s\n", GCODE_MACRO_2);
}

static void event_macro_3(lv_event_t *e) {
    sendGcode(GCODE_MACRO_3);
    Serial.printf("[MACRO] M3 -> %s\n", GCODE_MACRO_3);
}

// ==========================================
// EVENTI MODALE TEMPERATURA
// ==========================================
void update_modal_target_label() {
    if (lbl_modal_target) {
        lv_label_set_text_fmt(lbl_modal_target, "%d C", temp_target_val);
    }
}

static void event_open_nozzle_setting(lv_event_t * e) {
    setting_is_nozzle = true;
    temp_target_val = (current_ext_target > 0) ? (int)current_ext_target : 200;
    lv_label_set_text(lbl_modal_title, "SET NOZZLE");
    lv_obj_set_style_text_color(lbl_modal_title, lv_color_hex(0xE53935), 0);
    update_modal_target_label();
    lv_obj_clear_flag(modal_temp, LV_OBJ_FLAG_HIDDEN);
}

static void event_open_bed_setting(lv_event_t * e) {
    setting_is_nozzle = false;
    temp_target_val = (current_bed_target > 0) ? (int)current_bed_target : 60;
    lv_label_set_text(lbl_modal_title, "SET BED");
    lv_obj_set_style_text_color(lbl_modal_title, lv_color_hex(0x1E88E5), 0);
    update_modal_target_label();
    lv_obj_clear_flag(modal_temp, LV_OBJ_FLAG_HIDDEN);
}

static void event_temp_m10(lv_event_t *e) {
    temp_target_val -= 10;
    if (temp_target_val < 0) temp_target_val = 0;
    update_modal_target_label();
}

static void event_temp_m1(lv_event_t *e) {
    temp_target_val -= 1;
    if (temp_target_val < 0) temp_target_val = 0;
    update_modal_target_label();
}

static void event_temp_p1(lv_event_t *e) {
    temp_target_val += 1;
    int max_val = setting_is_nozzle ? 300 : 130;
    if (temp_target_val > max_val) temp_target_val = max_val;
    update_modal_target_label();
}

static void event_temp_p10(lv_event_t *e) {
    temp_target_val += 10;
    int max_val = setting_is_nozzle ? 300 : 130;
    if (temp_target_val > max_val) temp_target_val = max_val;
    update_modal_target_label();
}

static void event_temp_off(lv_event_t *e) {
    temp_target_val = 0;
    update_modal_target_label();
}

static void event_temp_cancel(lv_event_t *e) {
    lv_obj_add_flag(modal_temp, LV_OBJ_FLAG_HIDDEN);
}

static void event_temp_apply(lv_event_t *e) {
    if (setting_is_nozzle) {
        sendGcode("M104 S" + String(temp_target_val));
    } else {
        sendGcode("M140 S" + String(temp_target_val));
    }
    lv_obj_add_flag(modal_temp, LV_OBJ_FLAG_HIDDEN);
}

// ==========================================
// EVENTI MODALE VELOCITÀ (SPEED)
// ==========================================
void update_modal_speed_label() {
    if (lbl_modal_speed_val) {
        lv_label_set_text_fmt(lbl_modal_speed_val, "%d", speed_target_val);
    }
}

static void event_open_speed_setting(lv_event_t * e) {
    speed_target_val = (current_speed_val > 0) ? (int)round(current_speed_val) : 100;
    update_modal_speed_label();
    lv_obj_clear_flag(modal_speed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_speed);
}

static void event_speed_m50(lv_event_t *e) {
    speed_target_val -= 50;
    if (speed_target_val < 10) speed_target_val = 10;
    update_modal_speed_label();
}

static void event_speed_m10(lv_event_t *e) {
    speed_target_val -= 10;
    if (speed_target_val < 10) speed_target_val = 10;
    update_modal_speed_label();
}

static void event_speed_p10(lv_event_t *e) {
    speed_target_val += 10;
    if (speed_target_val > 1000) speed_target_val = 1000;
    update_modal_speed_label();
}

static void event_speed_p50(lv_event_t *e) {
    speed_target_val += 50;
    if (speed_target_val > 1000) speed_target_val = 1000;
    update_modal_speed_label();
}

static void event_speed_reset(lv_event_t *e) {
    speed_target_val = 100;
    update_modal_speed_label();
}

static void event_speed_cancel(lv_event_t *e) {
    lv_obj_add_flag(modal_speed, LV_OBJ_FLAG_HIDDEN);
}

static void event_speed_apply(lv_event_t *e) {
    sendGcode("SET_VELOCITY_LIMIT VELOCITY=" + String(speed_target_val));
    current_speed_val = speed_target_val;
    if (lbl_val_speed) {
        char s_buf[16];
        snprintf(s_buf, sizeof(s_buf), "%d", speed_target_val);
        lv_label_set_text(lbl_val_speed, s_buf);
    }
    lv_obj_add_flag(modal_speed, LV_OBJ_FLAG_HIDDEN);
}

// ==========================================
// EVENTI MODALE ACCELERAZIONE (ACCEL)
// ==========================================
void update_modal_accel_label() {
    if (lbl_modal_accel_val) {
        lv_label_set_text_fmt(lbl_modal_accel_val, "%d", accel_target_val);
    }
}

static void event_open_accel_setting(lv_event_t * e) {
    accel_target_val = (current_accel_val > 0) ? (int)round(current_accel_val) : 5000;
    update_modal_accel_label();
    lv_obj_clear_flag(modal_accel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_accel);
}

static void event_accel_m1000(lv_event_t *e) {
    accel_target_val -= 1000;
    if (accel_target_val < 100) accel_target_val = 100;
    update_modal_accel_label();
}

static void event_accel_m100(lv_event_t *e) {
    accel_target_val -= 100;
    if (accel_target_val < 100) accel_target_val = 100;
    update_modal_accel_label();
}

static void event_accel_p100(lv_event_t *e) {
    accel_target_val += 100;
    if (accel_target_val > 30000) accel_target_val = 30000;
    update_modal_accel_label();
}

static void event_accel_p1000(lv_event_t *e) {
    accel_target_val += 1000;
    if (accel_target_val > 30000) accel_target_val = 30000;
    update_modal_accel_label();
}

static void event_accel_reset(lv_event_t *e) {
    accel_target_val = 5000;
    update_modal_accel_label();
}

static void event_accel_cancel(lv_event_t *e) {
    lv_obj_add_flag(modal_accel, LV_OBJ_FLAG_HIDDEN);
}

static void event_accel_apply(lv_event_t *e) {
    sendGcode("SET_VELOCITY_LIMIT ACCEL=" + String(accel_target_val));
    sendGcode("M204 S" + String(accel_target_val));
    current_accel_val = accel_target_val;
    if (lbl_val_accel) {
        char a_buf[16];
        snprintf(a_buf, sizeof(a_buf), "%d", accel_target_val);
        lv_label_set_text(lbl_val_accel, a_buf);
    }
    lv_obj_add_flag(modal_accel, LV_OBJ_FLAG_HIDDEN);
}

// ==========================================
// EVENTI MODALE VENTOLA (FAN)
// ==========================================
static void event_open_fan_setting(lv_event_t * e) {
    if (lbl_modal_fan_val) {
        lv_label_set_text_fmt(lbl_modal_fan_val, "%d%%", current_fan_pct);
    }
    if (slider_modal_fan) {
        lv_slider_set_value(slider_modal_fan, current_fan_pct, LV_ANIM_OFF);
    }
    lv_obj_clear_flag(modal_fan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_fan);
}

static void event_modal_fan_slider(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);

    if (code == LV_EVENT_VALUE_CHANGED) {
        if (lbl_modal_fan_val) lv_label_set_text_fmt(lbl_modal_fan_val, "%d%%", val);
        if (lbl_val_fan) lv_label_set_text_fmt(lbl_val_fan, "%d%%", val);
    } else if (code == LV_EVENT_RELEASED) {
        apply_fan_pct(val);
    }
}

static void event_fan_preset_0(lv_event_t *e) { apply_fan_pct(0); }
static void event_fan_preset_50(lv_event_t *e) { apply_fan_pct(50); }
static void event_fan_preset_100(lv_event_t *e) { apply_fan_pct(100); }
static void event_modal_fan_close(lv_event_t *e) {
    lv_obj_add_flag(modal_fan, LV_OBJ_FLAG_HIDDEN);
}



// ==========================================
// UI CREATION HELPERS
// ==========================================
lv_obj_t* create_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_coord_t w, lv_coord_t h, lv_color_t color) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    // Stile Card Pagina 2: fondo ardesia scuro, pressed feedback, angoli 10-12px, bordo 1px
    lv_obj_set_style_radius(btn, (h <= 28) ? 10 : 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x16181F), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x28303F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    // Rileva accenti vivaci vs toni neutri per il colore di bordo e testo
    uint32_t c32 = lv_color_to32(color);
    int r = (c32 >> 16) & 0xFF;
    int g = (c32 >> 8) & 0xFF;
    int b = c32 & 0xFF;
    int diff = abs(r - g);
    int d2 = abs(g - b);
    int d3 = abs(r - b);
    if (d2 > diff) diff = d2;
    if (d3 > diff) diff = d3;

    lv_color_t text_color;
    if (diff > 35) {
        // Tasto d'azione / accento vivace (es. EXTRUDE, RETRACT, SET, OFF, BED LEVELING, HOME)
        lv_obj_set_style_border_color(btn, color, 0);
        text_color = color;
    } else {
        // Tasto neutro standard: bordo card Pagina 2 e testo silver
        lv_obj_set_style_border_color(btn, lv_color_hex(0x2A2F3D), 0);
        text_color = lv_color_hex(0xCFD8DC);
    }

    if (text) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, text_color, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(lbl);
    }
    return btn;
}

lv_obj_t* create_icon_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, lv_coord_t size, lv_color_t color) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x16181F), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x28303F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, color, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t* create_modal_base() {
    lv_obj_t *modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, 240, 240);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x121418), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal, 120, 0);
    lv_obj_set_style_border_color(modal, lv_color_hex(0x282D35), 0);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);

    // Tasto Indietro standard in basso (capsula / pillola smartwatch UI)
    lv_obj_t *btn_back = lv_btn_create(modal);
    lv_obj_set_size(btn_back, 106, 28);
    lv_obj_set_style_radius(btn_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x28303C), 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x3E4856), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(btn_back, event_close_modal, LV_EVENT_CLICKED, modal);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " INDIETRO");
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xCFD8DC), 0);
    lv_obj_clear_flag(lbl_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl_back);
    return modal;
}

lv_obj_t* create_wedge_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_color_t bg_color, const lv_img_dsc_t *icon_dsc, lv_color_t icon_color, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 118, 118);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x282D37), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon_dsc);
    lv_obj_set_style_img_recolor(img, icon_color, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(img);
    return btn;
}

lv_obj_t* create_menu_card(lv_obj_t *parent, lv_coord_t x_ofs, lv_coord_t y_ofs, lv_coord_t w, lv_coord_t h,
                           const lv_img_dsc_t *icon_img, const char *symbol_str,
                           lv_color_t icon_color, const char *label_text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x16181F), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x28303F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A2F3D), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    if (icon_img) {
        lv_obj_t *img = lv_img_create(btn);
        lv_img_set_src(img, icon_img);
        lv_obj_set_style_img_recolor(img, icon_color, 0);
        lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, -8);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else if (symbol_str) {
        lv_obj_t *sym = lv_label_create(btn);
        lv_label_set_text(sym, symbol_str);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sym, icon_color, 0);
        lv_obj_align(sym, LV_ALIGN_CENTER, 0, -8);
        lv_obj_clear_flag(sym, LV_OBJ_FLAG_CLICKABLE);
    }

    if (label_text) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label_text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCFD8DC), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 11);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    return btn;
}

void init_ui() {
    Serial.println("[UI] init_ui() START. Free heap: " + String(ESP.getFreeHeap()));
    // Schermo e Tileview base circolare
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    tv = lv_tileview_create(lv_scr_act());
    Serial.printf("[INIT] tv: %p\n", tv);
    lv_obj_set_size(tv, 240, 240);
    lv_obj_center(tv);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tv, 120, 0);          // Clip circolare per display rotondo
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tv, LV_OBJ_FLAG_SCROLL_ELASTIC); // Nessun rimbalzo oltre i limiti
    lv_obj_add_flag(tv, LV_OBJ_FLAG_SCROLL_MOMENTUM);  // Inerzia di lancio attiva
    lv_obj_add_flag(tv, LV_OBJ_FLAG_SCROLL_ONE);       // Transizione limitata a un solo tile
    lv_obj_add_event_cb(tv, tv_scroll_event_cb, LV_EVENT_SCROLL, NULL);

    // Creazione pagine con scorrimento verticale (dal basso verso l'alto)
    tile1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM); // Da Pagina 1 si puo scorrere solo verso Pagina 2
    Serial.printf("[INIT] tile1: %p\n", tile1);
    lv_obj_set_style_bg_color(tile1, lv_color_hex(0x0C0D10), 0);
    lv_obj_set_style_bg_opa(tile1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile1, 120, 0);        // Forma circolare pagina 1
    lv_obj_set_style_pad_all(tile1, 0, 0);
    lv_obj_set_style_border_width(tile1, 0, 0);
    lv_obj_set_style_clip_corner(tile1, true, 0);  // Taglia contenuti fuori dal cerchio display 240px
    lv_obj_clear_flag(tile1, LV_OBJ_FLAG_SCROLLABLE);

    tile2 = lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP);    // Da Pagina 2 si puo scorrere solo indietro (MAI oltre)
    lv_obj_set_style_bg_color(tile2, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile2, 120, 0);        // Forma circolare pagina 2
    lv_obj_set_style_pad_all(tile2, 0, 0);
    lv_obj_set_style_clip_corner(tile2, true, 0);  // Taglia contenuto fuori dal cerchio
    lv_obj_clear_flag(tile2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(tile2, LV_OPA_TRANSP, 0); // Inizialmente invisibile, compare con sfumatura allo scorrimento

    // Inizializza anelli di stato concentrici SOLO sulla prima pagina (tile1)
    for (int i = 0; i < NUM_STATUS_RINGS; i++) {
        status_rings[i] = lv_obj_create(tile1);
        lv_obj_set_size(status_rings[i], 238 - i * 4, 238 - i * 4);
        lv_obj_center(status_rings[i]);
        lv_obj_set_style_radius(status_rings[i], (238 - i * 4) / 2, 0);
        lv_obj_set_style_bg_opa(status_rings[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_rings[i], 2, 0);
        lv_obj_set_style_pad_all(status_rings[i], 0, 0);
        lv_obj_clear_flag(status_rings[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    status_ring = status_rings[0];
    update_status_rings_color(lv_color_hex(0xFF9800)); // Arancione all'avvio

    // ----------------------------------------------------
    // ----------------------------------------------------
    // SCHERMATA 1: DASHBOARD
    // ----------------------------------------------------
    // Telemetria Dashboard circolare centrale (Velocità, Ventola, Accelerazione, Host, Nozzle, Bed)
    card_telemetry = lv_obj_create(tile1);
    Serial.printf("[INIT] card_telemetry: %p\n", card_telemetry);
    lv_obj_set_size(card_telemetry, 216, 216);
    lv_obj_center(card_telemetry);
    lv_obj_set_style_radius(card_telemetry, 108, 0); // Raggio 108px per cerchio perfetto 216x216
    lv_obj_set_style_bg_color(card_telemetry, lv_color_hex(0x131519), 0);
    lv_obj_set_style_bg_opa(card_telemetry, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card_telemetry, 1, 0);
    lv_obj_set_style_border_color(card_telemetry, lv_color_hex(0x242832), 0);
    lv_obj_set_style_pad_all(card_telemetry, 0, 0);
    lv_obj_set_style_clip_corner(card_telemetry, true, 0); // Ritaglio perfettamente circolare per tutti i quadranti interni
    lv_obj_clear_flag(card_telemetry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card_telemetry, LV_OBJ_FLAG_SCROLL_CHAIN);

    // Divisorio orizzontale interno
    lv_obj_t *div_h = lv_obj_create(card_telemetry);
    lv_obj_set_size(div_h, 176, 1);
    lv_obj_align(div_h, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(div_h, lv_color_hex(0x20242C), 0);
    lv_obj_set_style_bg_opa(div_h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div_h, 0, 0);
    lv_obj_set_style_pad_all(div_h, 0, 0);
    lv_obj_clear_flag(div_h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Divisorio verticale interno
    lv_obj_t *div_v = lv_obj_create(card_telemetry);
    lv_obj_set_size(div_v, 1, 94);
    lv_obj_align(div_v, LV_ALIGN_TOP_MID, -1, 26);
    lv_obj_set_style_bg_color(div_v, lv_color_hex(0x282D35), 0);
    lv_obj_set_style_bg_opa(div_v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div_v, 0, 0);
    lv_obj_set_style_pad_all(div_v, 0, 0);
    lv_obj_clear_flag(div_v, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Linea divisoria inferiore di base (chiusura semiluna a contatto con tasti Nozzle e Bed)
    lv_obj_t *div_base = lv_obj_create(card_telemetry);
    lv_obj_set_size(div_base, 214, 1);
    lv_obj_align(div_base, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_color(div_base, lv_color_hex(0x282D35), 0);
    lv_obj_set_style_bg_opa(div_base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div_base, 0, 0);
    lv_obj_set_style_pad_all(div_base, 0, 0);
    lv_obj_clear_flag(div_base, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 1. Cella Alto-Sinistra: SPEED (Cliccabile - allineato a destra)
    lv_obj_t *btn_cell_speed = lv_btn_create(card_telemetry);
    lv_obj_set_size(btn_cell_speed, 94, 44);
    lv_obj_set_pos(btn_cell_speed, 12, 26);
    lv_obj_set_style_bg_opa(btn_cell_speed, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_cell_speed, lv_color_hex(0x37474F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_cell_speed, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_cell_speed, 6, 0);
    lv_obj_set_style_border_width(btn_cell_speed, 0, 0);
    lv_obj_set_style_pad_all(btn_cell_speed, 0, 0);
    lv_obj_set_style_shadow_width(btn_cell_speed, 0, 0);
    lv_obj_add_flag(btn_cell_speed, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(btn_cell_speed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_cell_speed, event_open_speed_setting, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_title_speed = lv_label_create(btn_cell_speed);
    lv_label_set_text(lbl_title_speed, "SPEED");
    lv_obj_set_style_text_font(lbl_title_speed, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title_speed, lv_color_hex(0x8A909D), 0);
    lv_obj_set_style_text_align(lbl_title_speed, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_title_speed, LV_ALIGN_TOP_RIGHT, -10, 2);
    lv_obj_clear_flag(lbl_title_speed, LV_OBJ_FLAG_CLICKABLE);

    lbl_val_speed = lv_label_create(btn_cell_speed);
    lv_label_set_text(lbl_val_speed, "0");
    lv_obj_set_style_text_font(lbl_val_speed, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_val_speed, lv_color_hex(0x00A2FF), 0); // Colore azzurro Speed / Move
    lv_obj_set_style_text_align(lbl_val_speed, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_val_speed, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
    lv_obj_clear_flag(lbl_val_speed, LV_OBJ_FLAG_CLICKABLE);

    // 2. Cella Alto-Destra: FAN (Cliccabile)
    lv_obj_t *btn_cell_fan = lv_btn_create(card_telemetry);
    lv_obj_set_size(btn_cell_fan, 94, 44);
    lv_obj_set_pos(btn_cell_fan, 108, 26);
    lv_obj_set_style_bg_opa(btn_cell_fan, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_cell_fan, lv_color_hex(0x37474F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_cell_fan, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_cell_fan, 6, 0);
    lv_obj_set_style_border_width(btn_cell_fan, 0, 0);
    lv_obj_set_style_pad_all(btn_cell_fan, 0, 0);
    lv_obj_set_style_shadow_width(btn_cell_fan, 0, 0);
    lv_obj_add_flag(btn_cell_fan, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(btn_cell_fan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_cell_fan, event_open_fan_setting, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_title_fan = lv_label_create(btn_cell_fan);
    lv_label_set_text(lbl_title_fan, "FAN");
    lv_obj_set_style_text_font(lbl_title_fan, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title_fan, lv_color_hex(0x8A909D), 0);
    lv_obj_align(lbl_title_fan, LV_ALIGN_TOP_LEFT, 10, 2);
    lv_obj_clear_flag(lbl_title_fan, LV_OBJ_FLAG_CLICKABLE);

    lbl_val_fan = lv_label_create(btn_cell_fan);
    lv_label_set_text(lbl_val_fan, "0%");
    lv_obj_set_style_text_font(lbl_val_fan, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_val_fan, lv_color_hex(0x26C6DA), 0);
    lv_obj_align(lbl_val_fan, LV_ALIGN_BOTTOM_LEFT, 10, -4);
    lv_obj_clear_flag(lbl_val_fan, LV_OBJ_FLAG_CLICKABLE);

    // 3. Cella Basso-Sinistra: ACCELERAZIONE (Cliccabile - allineato a destra)
    lv_obj_t *btn_cell_accel = lv_btn_create(card_telemetry);
    lv_obj_set_size(btn_cell_accel, 94, 48);
    lv_obj_set_pos(btn_cell_accel, 12, 71);
    lv_obj_set_style_bg_opa(btn_cell_accel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn_cell_accel, lv_color_hex(0x37474F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_cell_accel, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_cell_accel, 6, 0);
    lv_obj_set_style_border_width(btn_cell_accel, 0, 0);
    lv_obj_set_style_pad_all(btn_cell_accel, 0, 0);
    lv_obj_set_style_shadow_width(btn_cell_accel, 0, 0);
    lv_obj_add_flag(btn_cell_accel, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(btn_cell_accel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_cell_accel, event_open_accel_setting, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_title_accel = lv_label_create(btn_cell_accel);
    lv_label_set_text(lbl_title_accel, "ACCEL");
    lv_obj_set_style_text_font(lbl_title_accel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title_accel, lv_color_hex(0x8A909D), 0);
    lv_obj_set_style_text_align(lbl_title_accel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_title_accel, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_clear_flag(lbl_title_accel, LV_OBJ_FLAG_CLICKABLE);

    lbl_val_accel = lv_label_create(btn_cell_accel);
    lv_label_set_text(lbl_val_accel, "0");
    lv_obj_set_style_text_font(lbl_val_accel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_val_accel, lv_color_hex(0xFFB300), 0); // Colore ambra Accel / Calibrate
    lv_obj_set_style_text_align(lbl_val_accel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_val_accel, LV_ALIGN_BOTTOM_RIGHT, -10, -5);
    lv_obj_clear_flag(lbl_val_accel, LV_OBJ_FLAG_CLICKABLE);

    // 4. Cella Basso-Destra: TEMPERATURA HOST
    lv_obj_t *cell_host = lv_obj_create(card_telemetry);
    lv_obj_set_size(cell_host, 94, 48);
    lv_obj_set_pos(cell_host, 108, 71);
    lv_obj_set_style_bg_opa(cell_host, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell_host, 0, 0);
    lv_obj_set_style_pad_all(cell_host, 0, 0);
    lv_obj_clear_flag(cell_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell_host, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *lbl_title_host = lv_label_create(cell_host);
    lv_label_set_text(lbl_title_host, "HOST");
    lv_obj_set_style_text_font(lbl_title_host, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title_host, lv_color_hex(0x8A909D), 0);
    lv_obj_align(lbl_title_host, LV_ALIGN_TOP_LEFT, 10, 5);
    lv_obj_clear_flag(lbl_title_host, LV_OBJ_FLAG_CLICKABLE);

    lbl_val_host = lv_label_create(cell_host);
    lv_label_set_text(lbl_val_host, "-- C");
    lv_obj_set_style_text_font(lbl_val_host, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_val_host, lv_color_hex(0x66BB6A), 0);
    lv_obj_align(lbl_val_host, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    lv_obj_clear_flag(lbl_val_host, LV_OBJ_FLAG_CLICKABLE);

    // Bottone Nozzle (in basso a sinistra, a spicchio simmetrico nel cerchio)
    lv_obj_t *btn_nozzle = lv_btn_create(card_telemetry);
    lv_obj_set_size(btn_nozzle, 108, 96);
    lv_obj_align(btn_nozzle, LV_ALIGN_CENTER, -54, 60);
    lv_obj_set_style_bg_color(btn_nozzle, lv_color_hex(0x14161A), 0);
    lv_obj_set_style_radius(btn_nozzle, 0, 0); // Ritaglio circolare ereditato da card_telemetry
    lv_obj_set_style_border_width(btn_nozzle, 1, 0);
    lv_obj_set_style_border_side(btn_nozzle, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT, 0); // Divisore orizzontale superiore e verticale centrale
    lv_obj_set_style_border_color(btn_nozzle, lv_color_hex(0x282D35), 0);
    lv_obj_set_style_pad_all(btn_nozzle, 0, 0);
    lv_obj_set_style_shadow_width(btn_nozzle, 0, 0);
    lv_obj_add_flag(btn_nozzle, LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(btn_nozzle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_nozzle, event_open_nozzle_setting, LV_EVENT_CLICKED, NULL);

    // Icona visibile posizionata verso il centro dello schermo
    lv_obj_t *img_nozzle = lv_img_create(btn_nozzle);
    lv_img_set_src(img_nozzle, &icon_nozzle);
    lv_obj_set_style_img_recolor_opa(img_nozzle, LV_OPA_COVER, 0);
    lv_obj_set_style_img_recolor(img_nozzle, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_img_opa(img_nozzle, LV_OPA_COVER, 0); // Piena visibilita
    lv_obj_align(img_nozzle, LV_ALIGN_CENTER, 18, -22); // Verso il centro dello schermo
    lv_obj_clear_flag(img_nozzle, LV_OBJ_FLAG_CLICKABLE);

    // Valore testuale ruotato lungo la curvatura esterna con base verso l'esterno (su tile1 per evitare clipping da card_telemetry)
    lbl_nozzle = lv_label_create(tile1);
    lv_label_set_text(lbl_nozzle, "-- C");
    lv_obj_set_style_text_color(lbl_nozzle, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_text_font(lbl_nozzle, &font_display_18, 0); // Font Old Display 18px (+10% rispetto a 16px)
    lv_obj_set_style_transform_pivot_x(lbl_nozzle, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(lbl_nozzle, lv_pct(50), 0);
    lv_obj_set_style_transform_angle(lbl_nozzle, 450, 0); // 45.0 gradi (base verso la curvatura esterna)
    lv_obj_align(lbl_nozzle, LV_ALIGN_CENTER, -60, 66); // Simmetrico sul cerchio esterno
    lv_obj_add_flag(lbl_nozzle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_nozzle, event_open_nozzle_setting, LV_EVENT_CLICKED, NULL);

    // Bottone Bed (in basso a destra, a spicchio simmetrico nel cerchio)
    lv_obj_t *btn_bed = lv_btn_create(card_telemetry);
    lv_obj_set_size(btn_bed, 108, 96);
    lv_obj_align(btn_bed, LV_ALIGN_CENTER, 54, 60);
    lv_obj_set_style_bg_color(btn_bed, lv_color_hex(0x14161A), 0);
    lv_obj_set_style_radius(btn_bed, 0, 0); // Ritaglio circolare ereditato da card_telemetry
    lv_obj_set_style_border_width(btn_bed, 1, 0);
    lv_obj_set_style_border_side(btn_bed, LV_BORDER_SIDE_TOP, 0); // Divisore orizzontale superiore
    lv_obj_set_style_border_color(btn_bed, lv_color_hex(0x282D35), 0);
    lv_obj_set_style_pad_all(btn_bed, 0, 0);
    lv_obj_set_style_shadow_width(btn_bed, 0, 0);
    lv_obj_add_flag(btn_bed, LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(btn_bed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_bed, event_open_bed_setting, LV_EVENT_CLICKED, NULL);

    // Icona visibile posizionata verso il centro dello schermo
    lv_obj_t *img_bed = lv_img_create(btn_bed);
    lv_img_set_src(img_bed, &icon_bed);
    lv_obj_set_style_img_recolor_opa(img_bed, LV_OPA_COVER, 0);
    lv_obj_set_style_img_recolor(img_bed, lv_color_hex(0x00A2FF), 0);
    lv_obj_set_style_img_opa(img_bed, LV_OPA_COVER, 0); // Piena visibilita
    lv_obj_align(img_bed, LV_ALIGN_CENTER, -18, -22); // Verso il centro dello schermo
    lv_obj_clear_flag(img_bed, LV_OBJ_FLAG_CLICKABLE);

    // Valore testuale ruotato lungo la curvatura esterna con base verso l'esterno (su tile1 per evitare clipping da card_telemetry)
    lbl_bed = lv_label_create(tile1);
    lv_label_set_text(lbl_bed, "-- C");
    lv_obj_set_style_text_color(lbl_bed, lv_color_hex(0x00A2FF), 0);
    lv_obj_set_style_text_font(lbl_bed, &font_display_18, 0); // Font Old Display 18px (+10% rispetto a 16px)
    lv_obj_set_style_transform_pivot_x(lbl_bed, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(lbl_bed, lv_pct(50), 0);
    lv_obj_set_style_transform_angle(lbl_bed, 3150, 0); // -45.0 gradi (3150 in 0.1 deg, base verso la curvatura esterna)
    lv_obj_align(lbl_bed, LV_ALIGN_CENTER, 60, 66); // Simmetrico sul cerchio esterno
    lv_obj_add_flag(lbl_bed, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_bed, event_open_bed_setting, LV_EVENT_CLICKED, NULL);

    // Porta in primo piano i bottoni Nozzle e Bed rispetto alla semiluna di sfondo
    lv_obj_move_foreground(btn_nozzle);
    lv_obj_move_foreground(btn_bed);
    // Linguetta minimale statica per suggerire lo swipe verso l'alto
    lv_obj_t *tab_hint = lv_obj_create(tile1);
    lv_obj_set_size(tab_hint, 44, 18);
    lv_obj_align(tab_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(tab_hint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_hint, 0, 0);
    lv_obj_set_style_pad_all(tab_hint, 0, 0);
    lv_obj_clear_flag(tab_hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tab_hint, event_tab_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint_icon = lv_label_create(tab_hint);
    lv_label_set_text(hint_icon, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(hint_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint_icon, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_style_text_opa(hint_icon, LV_OPA_70, 0);
    lv_obj_align(hint_icon, LV_ALIGN_TOP_MID, 0, -2);
    lv_obj_clear_flag(hint_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hint_bar = lv_obj_create(tab_hint);
    lv_obj_set_size(hint_bar, 28, 3);
    lv_obj_align(hint_bar, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_style_radius(hint_bar, 2, 0);
    lv_obj_set_style_bg_color(hint_bar, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_70, 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_style_pad_all(hint_bar, 0, 0);
    lv_obj_clear_flag(hint_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Porta in primo piano gli anelli sfumati per incorniciare il display circolare di pagina 1
    for (int i = 0; i < NUM_STATUS_RINGS; i++) {
        if (status_rings[i]) lv_obj_move_foreground(status_rings[i]);
    }

    // Tacca di stato stampante inglobata nel cerchio di stato in alto (elimina il gap vuoto)
    btn_notch = lv_btn_create(tile1);
    lv_obj_set_size(btn_notch, 80, 30);
    lv_obj_align(btn_notch, LV_ALIGN_TOP_MID, 0, -6);
    lv_obj_set_style_radius(btn_notch, 12, 0);
    lv_obj_set_style_bg_color(btn_notch, lv_color_hex(0xFF9800), 0); // Arancione all'avvio
    lv_obj_set_style_bg_opa(btn_notch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_notch, 0, 0); // Nessun bordo per fusione continua con l'anello
    lv_obj_set_style_pad_all(btn_notch, 0, 0);
    lv_obj_set_style_shadow_width(btn_notch, 0, 0);
    lv_obj_add_event_cb(btn_notch, event_open_print_ctrl, LV_EVENT_CLICKED, NULL);

    lbl_notch = lv_label_create(btn_notch);
    lv_label_set_text(lbl_notch, "OFFLINE");
    lv_obj_set_style_text_font(lbl_notch, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_notch, lv_color_white(), 0);
    lv_obj_clear_flag(lbl_notch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_notch, LV_ALIGN_CENTER, 0, 3); // Centrato nell'area visibile

    lv_obj_move_foreground(btn_notch);
    lv_obj_move_foreground(tab_hint);
    lv_obj_move_foreground(lbl_nozzle);
    lv_obj_move_foreground(lbl_bed);


    // ----------------------------------------------------
    // SCHERMATA 2: MENU A 6 PULSANTI
    // Riga 1: Alto-SX (Estrusione - Rosso) & Alto-DX (Movimento - Blu)
    // Riga 2: Centro-SX (Calibrazione - Ambra) & Centro-DX (Accessori - Verde)
    // Riga 3: Basso-SX (Console - Viola) & Basso-DX (Files - Ciano)
    // ----------------------------------------------------
    create_menu_card(tile2, -46, -66, 82, 46, &icon_nozzle, NULL, lv_color_hex(0xFF5252), "EXTRUDE", event_open_extrude);
    create_menu_card(tile2, 46, -66, 82, 46, &icon_move, NULL, lv_color_hex(0x00A2FF), "MOVE", event_open_move);

    create_menu_card(tile2, -52, 0, 94, 46, &icon_calibrate, NULL, lv_color_hex(0xFFB300), "CALIBRATE", event_open_calibrate);
    create_menu_card(tile2, 52, 0, 94, 46, &icon_accessories, NULL, lv_color_hex(0x4CAF50), "ACCESS", event_open_accessories);

    create_menu_card(tile2, -46, 66, 82, 46, NULL, LV_SYMBOL_KEYBOARD, lv_color_hex(0xBA68C8), "CONSOLE", event_open_console);
    create_menu_card(tile2, 46, 66, 82, 46, NULL, LV_SYMBOL_FILE, lv_color_hex(0x00BCD4), "FILES", event_open_files);


    // ----------------------------------------------------
    // SUBPAGINA MODALE: ESTRUSIONE
    // ----------------------------------------------------
    modal_extrude = create_modal_base();

    lv_obj_t *lbl_ext_title = lv_label_create(modal_extrude);
    lv_label_set_text(lbl_ext_title, "EXTRUSION");
    lv_obj_set_style_text_font(lbl_ext_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ext_title, lv_color_hex(0xFF5252), 0);
    lv_obj_align(lbl_ext_title, LV_ALIGN_TOP_MID, 0, 18);

    // Frecce direzionali: Estrudi (Giù) e Ritrai (Su)
    lv_obj_t *btn_act_ext = create_button(modal_extrude, LV_SYMBOL_DOWN " EXTRUDE", event_action_extrude, 92, 34, lv_color_hex(0x4CAF50));
    lv_obj_align(btn_act_ext, LV_ALIGN_CENTER, -48, -44);

    lv_obj_t *btn_act_ret = create_button(modal_extrude, LV_SYMBOL_UP " RETRACT", event_action_retract, 92, 34, lv_color_hex(0x00A2FF));
    lv_obj_align(btn_act_ret, LV_ALIGN_CENTER, 48, -44);

    // Selezione Distanza (mm)
    lv_obj_t *lbl_dist_title = lv_label_create(modal_extrude);
    lv_label_set_text(lbl_dist_title, "DISTANCE (mm)");
    lv_obj_set_style_text_font(lbl_dist_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_dist_title, lv_color_hex(0x90A4AE), 0);
    lv_obj_align(lbl_dist_title, LV_ALIGN_CENTER, 0, -12);

    for (int i = 0; i < 4; i++) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", dist_values[i]);
        btn_dist[i] = create_button(modal_extrude, buf, NULL, 42, 26, lv_color_hex(0x2A2F3D));
        lv_obj_add_event_cb(btn_dist[i], event_select_dist, LV_EVENT_CLICKED, (void*)(intptr_t)dist_values[i]);
        lv_obj_align(btn_dist[i], LV_ALIGN_CENTER, -66 + i * 44, 10);
    }

    // Selezione Velocità (mm/s)
    lv_obj_t *lbl_spd_title = lv_label_create(modal_extrude);
    lv_label_set_text(lbl_spd_title, "SPEED (mm/s)");
    lv_obj_set_style_text_font(lbl_spd_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_spd_title, lv_color_hex(0x90A4AE), 0);
    lv_obj_align(lbl_spd_title, LV_ALIGN_CENTER, 0, 34);

    for (int i = 0; i < 3; i++) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", speed_values[i]);
        btn_speed[i] = create_button(modal_extrude, buf, NULL, 50, 26, lv_color_hex(0x2A2F3D));
        lv_obj_add_event_cb(btn_speed[i], event_select_speed, LV_EVENT_CLICKED, (void*)(intptr_t)speed_values[i]);
        lv_obj_align(btn_speed[i], LV_ALIGN_CENTER, -54 + i * 54, 56);
    }

    update_extrude_dist_ui();
    update_extrude_speed_ui();


    // ----------------------------------------------------
    // SUBPAGINA MODALE: MOVIMENTO ASSI XYZ (Ex Pagina 3)
    // ----------------------------------------------------
    modal_move = create_modal_base();

    lv_obj_t *lbl_move_title = lv_label_create(modal_move);
    lv_label_set_text(lbl_move_title, "XYZ AXES");
    lv_obj_set_style_text_font(lbl_move_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_move_title, lv_color_hex(0x00A2FF), 0);
    lv_obj_align(lbl_move_title, LV_ALIGN_TOP_MID, 0, 18);

    // Y+ (Top)
    lv_obj_t *btn_yp = create_button(modal_move, "Y+", event_jog_yp, 44, 34, lv_color_hex(0x2A2F3D));
    lv_obj_align(btn_yp, LV_ALIGN_CENTER, 0, -52);

    // X- (Left), Home (Center), X+ (Right)
    lv_obj_t *btn_xm = create_button(modal_move, "X-", event_jog_xm, 44, 34, lv_color_hex(0x2A2F3D));
    lv_obj_align(btn_xm, LV_ALIGN_CENTER, -50, -10);

    lv_obj_t *btn_home = create_button(modal_move, LV_SYMBOL_HOME, event_home_all, 44, 34, lv_color_hex(0x00BCD4));
    lv_obj_align(btn_home, LV_ALIGN_CENTER, 0, -10);
    if (lv_obj_get_child_cnt(btn_home) > 0) {
        lv_obj_set_style_text_font(lv_obj_get_child(btn_home, 0), &lv_font_montserrat_16, 0);
    }

    lv_obj_t *btn_xp = create_button(modal_move, "X+", event_jog_xp, 44, 34, lv_color_hex(0x2A2F3D));
    lv_obj_align(btn_xp, LV_ALIGN_CENTER, 50, -10);

    // Y- (Bottom)
    lv_obj_t *btn_ym = create_button(modal_move, "Y-", event_jog_ym, 44, 34, lv_color_hex(0x2A2F3D));
    lv_obj_align(btn_ym, LV_ALIGN_CENTER, 0, 32);

    // Z- e Z+
    lv_obj_t *btn_zm = create_button(modal_move, "Z-", event_jog_zm, 40, 30, lv_color_hex(0x2A2F3D));
    lv_obj_align(btn_zm, LV_ALIGN_CENTER, -62, 50);

    lv_obj_t *btn_zp = create_button(modal_move, "Z+", event_jog_zp, 40, 30, lv_color_hex(0x2A2F3D));
    lv_obj_align(btn_zp, LV_ALIGN_CENTER, 62, 50);


    // ----------------------------------------------------
    // SUBPAGINA MODALE: CALIBRAZIONE
    // ----------------------------------------------------
    modal_calibrate = create_modal_base();

    lv_obj_t *lbl_cal_title = lv_label_create(modal_calibrate);
    lv_label_set_text(lbl_cal_title, "CALIBRATION");
    lv_obj_set_style_text_font(lbl_cal_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_cal_title, lv_color_hex(0xFFB300), 0);
    lv_obj_align(lbl_cal_title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *btn_cal_bed = create_button(modal_calibrate, "BED LEVELING", event_calib_bed, 160, 36, lv_color_hex(0xFFB300));
    lv_obj_align(btn_cal_bed, LV_ALIGN_CENTER, 0, -32);

    lv_obj_t *btn_cal_res = create_button(modal_calibrate, "CALC RESONANCE", event_calib_shaper, 160, 36, lv_color_hex(0x00A2FF));
    lv_obj_align(btn_cal_res, LV_ALIGN_CENTER, 0, 14);

    lbl_calib_status = lv_label_create(modal_calibrate);
    lv_label_set_text(lbl_calib_status, "");
    lv_obj_set_style_text_font(lbl_calib_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_calib_status, lv_color_hex(0x81C784), 0);
    lv_obj_align(lbl_calib_status, LV_ALIGN_CENTER, 0, 52);


    // ----------------------------------------------------
    // SUBPAGINA MODALE: ACCESSORI
    // ----------------------------------------------------
    modal_accessories = create_modal_base();

    lv_obj_t *lbl_acc_title = lv_label_create(modal_accessories);
    lv_label_set_text(lbl_acc_title, "ACCESSORIES");
    lv_obj_set_style_text_font(lbl_acc_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_acc_title, lv_color_hex(0x4CAF50), 0);
    lv_obj_align(lbl_acc_title, LV_ALIGN_TOP_MID, 0, 18);

    btn_acc_light = create_button(modal_accessories, "LIGHT: OFF", event_acc_light, 140, 32, lv_color_hex(0x2A2F3D));
    lbl_acc_light = lv_obj_get_child(btn_acc_light, 0);
    lv_obj_align(btn_acc_light, LV_ALIGN_CENTER, 0, -46);

    btn_acc_runout = create_button(modal_accessories, "RUNOUT: OFF", event_acc_runout, 140, 32, lv_color_hex(0x2A2F3D));
    lbl_acc_runout = lv_obj_get_child(btn_acc_runout, 0);
    lv_obj_align(btn_acc_runout, LV_ALIGN_CENTER, 0, -10);

    update_light_ui(light_state);
    update_runout_ui(runout_state);

    // Sezione Macro (3 pulsanti M1, M2, M3)
    const char* macro_labels[3] = {"M1", "M2", "M3"};
    lv_event_cb_t macro_cbs[3] = {event_macro_1, event_macro_2, event_macro_3};
    for (int i = 0; i < 3; i++) {
        btn_macro[i] = create_button(modal_accessories, macro_labels[i], macro_cbs[i], 42, 32, lv_color_hex(0x2A2F3D));
        lv_obj_align(btn_macro[i], LV_ALIGN_CENTER, (i - 1) * 52, 36);
    }

    // ----------------------------------------------------
    // SUBPAGINA MODALE: CONTROLLI STAMPA (Tacca Pagina 1)
    // ----------------------------------------------------
    modal_print_ctrl = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal_print_ctrl, 240, 240);
    lv_obj_center(modal_print_ctrl);
    lv_obj_set_style_bg_color(modal_print_ctrl, lv_color_hex(0x121418), 0);
    lv_obj_set_style_bg_opa(modal_print_ctrl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal_print_ctrl, 120, 0);
    lv_obj_set_style_border_color(modal_print_ctrl, lv_color_hex(0x282D35), 0);
    lv_obj_set_style_border_width(modal_print_ctrl, 2, 0);
    lv_obj_set_style_pad_all(modal_print_ctrl, 0, 0);
    lv_obj_clear_flag(modal_print_ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_print_ctrl, LV_OBJ_FLAG_HIDDEN);

    // Unghia di Arresto d'Emergenza (in alto a forma di unghia/tacca, ingrandita del 20%)
    btn_pc_emergency = lv_btn_create(modal_print_ctrl);
    lv_obj_set_size(btn_pc_emergency, 100, 36);
    lv_obj_align(btn_pc_emergency, LV_ALIGN_TOP_MID, 0, -6);
    lv_obj_set_style_radius(btn_pc_emergency, 14, 0);
    lv_obj_set_style_bg_color(btn_pc_emergency, lv_color_hex(0xE53935), 0); // Rosso acceso
    lv_obj_set_style_bg_color(btn_pc_emergency, lv_color_hex(0xB71C1C), LV_STATE_PRESSED); // Rosso scuro premuto
    lv_obj_set_style_bg_opa(btn_pc_emergency, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_pc_emergency, 0, 0);
    lv_obj_set_style_pad_all(btn_pc_emergency, 0, 0);
    lv_obj_set_style_shadow_width(btn_pc_emergency, 0, 0);
    lv_obj_clear_flag(btn_pc_emergency, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_pc_emergency, event_emergency_stop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_pc_emergency = lv_label_create(btn_pc_emergency);
    lv_label_set_text(lbl_pc_emergency, LV_SYMBOL_WARNING " SOS");
    lv_obj_set_style_text_font(lbl_pc_emergency, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_pc_emergency, lv_color_white(), 0);
    lv_obj_clear_flag(lbl_pc_emergency, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(lbl_pc_emergency, LV_ALIGN_CENTER, 0, 4); // Centrato nell'area visibile dell'unghia

    // Nome file di stampa
    lbl_print_filename = lv_label_create(modal_print_ctrl);
    lv_label_set_long_mode(lbl_print_filename, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_print_filename, 180);
    lv_obj_set_style_text_align(lbl_print_filename, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_print_filename, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_print_filename, lv_color_hex(0x546E7A), 0);
    lv_label_set_text(lbl_print_filename, "No file");
    lv_obj_align(lbl_print_filename, LV_ALIGN_TOP_MID, 0, 38);

    // Pulsanti di controllo (visibilita dinamica condizionata dallo stato di stampa)
    // 1. Play (Riprendi) - Verde
    btn_pc_play = create_icon_button(modal_print_ctrl, LV_SYMBOL_PLAY, event_print_play, 50, lv_color_hex(0x4CAF50));
    lv_obj_align(btn_pc_play, LV_ALIGN_CENTER, -42, -14);

    // 2. Pausa - Ambra
    btn_pc_pause = create_icon_button(modal_print_ctrl, LV_SYMBOL_PAUSE, event_print_pause, 50, lv_color_hex(0xFFB300));
    lv_obj_align(btn_pc_pause, LV_ALIGN_CENTER, 42, -14);

    // 3. Stop - Rosso
    btn_pc_stop = create_icon_button(modal_print_ctrl, LV_SYMBOL_STOP, event_print_stop, 50, lv_color_hex(0xFF5252));
    lv_obj_align(btn_pc_stop, LV_ALIGN_CENTER, -42, 44);

    // 4. Ristampa - Blu
    btn_pc_restart = create_icon_button(modal_print_ctrl, LV_SYMBOL_REFRESH, event_print_restart, 50, lv_color_hex(0x00A2FF));
    lv_obj_align(btn_pc_restart, LV_ALIGN_CENTER, 42, 44);

    // 5. Tasto Riavvio Firmware (Ambra/Arancio, nascosto durante la stampa)
    btn_pc_fw_restart = lv_btn_create(modal_print_ctrl);
    lv_obj_set_size(btn_pc_fw_restart, 50, 50);
    lv_obj_set_style_radius(btn_pc_fw_restart, 12, 0);
    lv_obj_set_style_bg_color(btn_pc_fw_restart, lv_color_hex(0x16181F), 0);
    lv_obj_set_style_bg_color(btn_pc_fw_restart, lv_color_hex(0x28303F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_pc_fw_restart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_pc_fw_restart, 1, 0);
    lv_obj_set_style_border_color(btn_pc_fw_restart, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_pad_all(btn_pc_fw_restart, 0, 0);
    lv_obj_set_style_shadow_width(btn_pc_fw_restart, 0, 0);
    lv_obj_clear_flag(btn_pc_fw_restart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_pc_fw_restart, event_firmware_restart, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_fw = lv_label_create(btn_pc_fw_restart);
    lv_label_set_text(lbl_fw, LV_SYMBOL_REFRESH "\nFW");
    lv_obj_set_style_text_align(lbl_fw, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_fw, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_fw, lv_color_hex(0xFFB74D), 0);
    lv_obj_clear_flag(lbl_fw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl_fw);

    // 6. Tasto Spegnimento Macchina / Shutdown (Rosso, nascosto durante la stampa)
    btn_pc_shutdown = lv_btn_create(modal_print_ctrl);
    lv_obj_set_size(btn_pc_shutdown, 50, 50);
    lv_obj_set_style_radius(btn_pc_shutdown, 12, 0);
    lv_obj_set_style_bg_color(btn_pc_shutdown, lv_color_hex(0x16181F), 0);
    lv_obj_set_style_bg_color(btn_pc_shutdown, lv_color_hex(0x28303F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_pc_shutdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_pc_shutdown, 1, 0);
    lv_obj_set_style_border_color(btn_pc_shutdown, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_pad_all(btn_pc_shutdown, 0, 0);
    lv_obj_set_style_shadow_width(btn_pc_shutdown, 0, 0);
    lv_obj_clear_flag(btn_pc_shutdown, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_pc_shutdown, event_machine_shutdown, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_shut = lv_label_create(btn_pc_shutdown);
    lv_label_set_text(lbl_shut, LV_SYMBOL_POWER "\nSHUT");
    lv_obj_set_style_text_align(lbl_shut, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_shut, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_shut, lv_color_hex(0xFF8A80), 0);
    lv_obj_clear_flag(lbl_shut, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl_shut);

    // 7. Tasto Rotazione Display (ciclo 4 angolazioni: 0°, 90°, 180°, 270°)
    btn_pc_rotate = lv_btn_create(modal_print_ctrl);
    lv_obj_set_size(btn_pc_rotate, 50, 50);
    lv_obj_set_style_radius(btn_pc_rotate, 12, 0);
    lv_obj_set_style_bg_color(btn_pc_rotate, lv_color_hex(0x16181F), 0);
    lv_obj_set_style_bg_color(btn_pc_rotate, lv_color_hex(0x28303F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_pc_rotate, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_pc_rotate, 1, 0);
    lv_obj_set_style_border_color(btn_pc_rotate, lv_color_hex(0x00BCD4), 0);
    lv_obj_set_style_pad_all(btn_pc_rotate, 0, 0);
    lv_obj_set_style_shadow_width(btn_pc_rotate, 0, 0);
    lv_obj_clear_flag(btn_pc_rotate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_pc_rotate, event_screen_rotate, LV_EVENT_CLICKED, NULL);

    lbl_pc_rotate = lv_label_create(btn_pc_rotate);
    lv_label_set_text_fmt(lbl_pc_rotate, "%s\n%s", get_rotation_icon(screen_rotation), get_rotation_text(screen_rotation));
    lv_obj_set_style_text_align(lbl_pc_rotate, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_pc_rotate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_pc_rotate, lv_color_hex(0x00E5FF), 0);
    lv_obj_clear_flag(lbl_pc_rotate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl_pc_rotate);

    update_print_ctrl_ui();

    // Tasto Indietro (capsula smartwatch) in basso
    lv_obj_t *btn_pc_back = lv_btn_create(modal_print_ctrl);
    lv_obj_set_size(btn_pc_back, 106, 28);
    lv_obj_align(btn_pc_back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(btn_pc_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_pc_back, lv_color_hex(0x28303C), 0);
    lv_obj_set_style_bg_color(btn_pc_back, lv_color_hex(0x3E4856), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_pc_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_pc_back, 0, 0);
    lv_obj_set_style_pad_all(btn_pc_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_pc_back, 0, 0);
    lv_obj_add_event_cb(btn_pc_back, event_close_modal, LV_EVENT_CLICKED, modal_print_ctrl);

    lv_obj_t *lbl_pc_back = lv_label_create(btn_pc_back);
    lv_label_set_text(lbl_pc_back, LV_SYMBOL_LEFT " INDIETRO");
    lv_obj_set_style_text_font(lbl_pc_back, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_pc_back, lv_color_hex(0xCFD8DC), 0);
    lv_obj_clear_flag(lbl_pc_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl_pc_back);

    // ----------------------------------------------------
    // SUBPAGINA MODALE: CONSOLE (STILE SMARTWATCH UI)
    // ----------------------------------------------------
    modal_console = create_modal_base();

    // 1. Personalizzazione Tasto INDIETRO in basso (capsula / pillola come da UI)
    lv_obj_t *btn_con_back = lv_obj_get_child(modal_console, 0);
    if (btn_con_back) {
        lv_obj_set_size(btn_con_back, 106, 28);
        lv_obj_set_style_radius(btn_con_back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn_con_back, lv_color_hex(0x28303C), 0);
        lv_obj_set_style_bg_color(btn_con_back, lv_color_hex(0x3E4856), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn_con_back, 0, 0);
        lv_obj_align(btn_con_back, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_t *lbl_b = lv_obj_get_child(btn_con_back, 0);
        if (lbl_b) {
            lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " INDIETRO");
            lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl_b, lv_color_hex(0xCFD8DC), 0);
        }
    }

    // 2. Traccia ad arco circolare sinistra (dettaglio estetico simmetrico della UI)
    lv_obj_t *arc_con_left = lv_arc_create(modal_console);
    lv_obj_set_size(arc_con_left, 234, 234);
    lv_obj_center(arc_con_left);
    lv_arc_set_rotation(arc_con_left, 135);
    lv_arc_set_bg_angles(arc_con_left, 0, 90);
    lv_arc_set_angles(arc_con_left, 0, 0);
    lv_obj_set_style_arc_width(arc_con_left, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_con_left, lv_color_hex(0x222730), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_con_left, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_opa(arc_con_left, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_con_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc_con_left, LV_OBJ_FLAG_SCROLLABLE);

    // 3. Indicatore di scorrimento ad arco circolare destro (traccia + cursore viola/accento console)
    arc_console_scroll = lv_arc_create(modal_console);
    lv_obj_set_size(arc_console_scroll, 234, 234);
    lv_obj_center(arc_console_scroll);
    lv_arc_set_rotation(arc_console_scroll, 315);
    lv_arc_set_bg_angles(arc_console_scroll, 0, 90);
    lv_arc_set_angles(arc_console_scroll, 0, 25);
    lv_obj_set_style_arc_width(arc_console_scroll, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_console_scroll, lv_color_hex(0x222730), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_console_scroll, LV_OPA_60, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc_console_scroll, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_console_scroll, lv_color_hex(0xBA68C8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_console_scroll, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc_console_scroll, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_console_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc_console_scroll, LV_OBJ_FLAG_SCROLLABLE);

    // 4. Titolo "CONSOLE" centrato in alto in bianco puro (#FFFFFF)
    lv_obj_t *lbl_con_title = lv_label_create(modal_console);
    lv_label_set_text(lbl_con_title, "CONSOLE");
    lv_obj_set_style_text_font(lbl_con_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_con_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_con_title, LV_ALIGN_TOP_MID, 0, 16);

    // 5. Container lista messaggi a tutta larghezza (240px) per allineamento a sinistra e curvatura dinamica
    cont_console_list = lv_obj_create(modal_console);
    lv_obj_set_size(cont_console_list, 240, 154);
    lv_obj_align(cont_console_list, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_opa(cont_console_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_console_list, 0, 0);
    lv_obj_set_style_pad_all(cont_console_list, 0, 0);
    lv_obj_set_style_pad_top(cont_console_list, 4, 0);
    lv_obj_set_style_pad_bottom(cont_console_list, 8, 0);
    lv_obj_set_style_pad_row(cont_console_list, 4, 0);
    lv_obj_set_flex_flow(cont_console_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont_console_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(cont_console_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont_console_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cont_console_list, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(cont_console_list, event_console_scroll, LV_EVENT_SCROLL, NULL);

    lbl_console_status = lv_label_create(cont_console_list);
    lv_obj_set_style_text_font(lbl_console_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_console_status, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_style_text_align(lbl_console_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_console_status, 240);
    lv_label_set_text(lbl_console_status, "In attesa messaggi...");
    if (btn_con_back) lv_obj_move_foreground(btn_con_back);

    // ----------------------------------------------------
    // SUBPAGINA MODALE: GESTORE FILE (STILE SMARTWATCH UI)
    // ----------------------------------------------------
    modal_files = create_modal_base();

    // 1. Personalizzazione Tasto INDIETRO in basso (pillola / capsula come da mockup UI)
    lv_obj_t *btn_files_back = lv_obj_get_child(modal_files, 0);
    if (btn_files_back) {
        lv_obj_set_size(btn_files_back, 106, 28);
        lv_obj_set_style_radius(btn_files_back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn_files_back, lv_color_hex(0x28303C), 0);
        lv_obj_set_style_bg_color(btn_files_back, lv_color_hex(0x3E4856), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn_files_back, 0, 0);
        lv_obj_align(btn_files_back, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_t *lbl_b = lv_obj_get_child(btn_files_back, 0);
        if (lbl_b) {
            lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " INDIETRO");
            lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl_b, lv_color_hex(0xCFD8DC), 0);
        }
    }

    // 2. Traccia ad arco circolare sinistra (dettaglio estetico simmetrico della UI)
    lv_obj_t *arc_track_left = lv_arc_create(modal_files);
    lv_obj_set_size(arc_track_left, 234, 234);
    lv_obj_center(arc_track_left);
    lv_arc_set_rotation(arc_track_left, 135);
    lv_arc_set_bg_angles(arc_track_left, 0, 90);
    lv_arc_set_angles(arc_track_left, 0, 0);
    lv_obj_set_style_arc_width(arc_track_left, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_track_left, lv_color_hex(0x222730), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_track_left, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_opa(arc_track_left, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_track_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc_track_left, LV_OBJ_FLAG_SCROLLABLE);

    // 3. Indicatore di scorrimento ad arco circolare destro (traccia + cursore cyan)
    arc_files_scroll = lv_arc_create(modal_files);
    lv_obj_set_size(arc_files_scroll, 234, 234);
    lv_obj_center(arc_files_scroll);
    lv_arc_set_rotation(arc_files_scroll, 315);
    lv_arc_set_bg_angles(arc_files_scroll, 0, 90);
    lv_arc_set_angles(arc_files_scroll, 0, 25);
    lv_obj_set_style_arc_width(arc_files_scroll, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_files_scroll, lv_color_hex(0x222730), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_files_scroll, LV_OPA_60, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc_files_scroll, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_files_scroll, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_files_scroll, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc_files_scroll, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc_files_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc_files_scroll, LV_OBJ_FLAG_SCROLLABLE);

    // 4. Titolo "GESTORE FILE" centrato in alto
    lv_obj_t *lbl_files_title = lv_label_create(modal_files);
    lv_label_set_text(lbl_files_title, "GESTORE FILE");
    lv_obj_set_style_text_font(lbl_files_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_files_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_files_title, LV_ALIGN_TOP_MID, 0, 16);

    // 5. Container lista file a tutta larghezza (240px) per allineamento a sinistra e curvatura dinamica
    cont_files_list = lv_obj_create(modal_files);
    lv_obj_set_size(cont_files_list, 240, 154);
    lv_obj_align(cont_files_list, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_opa(cont_files_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_files_list, 0, 0);
    lv_obj_set_style_pad_all(cont_files_list, 0, 0);
    lv_obj_set_style_pad_top(cont_files_list, 4, 0);
    lv_obj_set_style_pad_bottom(cont_files_list, 8, 0);
    lv_obj_set_style_pad_row(cont_files_list, 4, 0);
    lv_obj_set_flex_flow(cont_files_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont_files_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(cont_files_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont_files_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cont_files_list, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(cont_files_list, event_files_scroll, LV_EVENT_SCROLL, NULL);

    lbl_files_status = lv_label_create(cont_files_list);
    lv_obj_set_style_text_font(lbl_files_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_files_status, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_style_text_align(lbl_files_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_files_status, 240);
    lv_label_set_text(lbl_files_status, "Pronto.");
    if (btn_files_back) lv_obj_move_foreground(btn_files_back);

    // ----------------------------------------------------
    // POPUP MODALE: IMPOSTAZIONE TEMPERATURA
    // ----------------------------------------------------
    modal_temp = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal_temp, 240, 240);
    lv_obj_center(modal_temp);
    lv_obj_set_style_bg_color(modal_temp, lv_color_hex(0x181818), 0);
    lv_obj_set_style_radius(modal_temp, 120, 0);
    lv_obj_set_style_border_color(modal_temp, lv_color_hex(0x424242), 0);
    lv_obj_set_style_border_width(modal_temp, 2, 0);
    lv_obj_clear_flag(modal_temp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_temp, LV_OBJ_FLAG_HIDDEN); // Nascosto di default

    lbl_modal_title = lv_label_create(modal_temp);
    lv_label_set_text(lbl_modal_title, "SET TEMP");
    lv_obj_set_style_text_font(lbl_modal_title, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_modal_title, LV_ALIGN_CENTER, 0, -72);

    lbl_modal_target = lv_label_create(modal_temp);
    lv_label_set_text(lbl_modal_target, "200 C");
    lv_obj_set_style_text_font(lbl_modal_target, &lv_font_montserrat_24, 0);
    lv_obj_align(lbl_modal_target, LV_ALIGN_CENTER, 0, -32);

    // Pulsanti regolazione [-10] [-1] [+1] [+10]
    create_button(modal_temp, "-10", event_temp_m10, 42, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(lv_obj_get_child(modal_temp, 2), LV_ALIGN_CENTER, -66, 10);

    create_button(modal_temp, "-1", event_temp_m1, 38, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(lv_obj_get_child(modal_temp, 3), LV_ALIGN_CENTER, -22, 10);

    create_button(modal_temp, "+1", event_temp_p1, 38, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(lv_obj_get_child(modal_temp, 4), LV_ALIGN_CENTER, 22, 10);

    create_button(modal_temp, "+10", event_temp_p10, 42, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(lv_obj_get_child(modal_temp, 5), LV_ALIGN_CENTER, 66, 10);

    // Pulsanti inferiori [OFF] [ESC] [SET]
    create_button(modal_temp, "OFF", event_temp_off, 48, 32, lv_color_hex(0xFF5252));
    lv_obj_align(lv_obj_get_child(modal_temp, 6), LV_ALIGN_CENTER, -58, 58);

    create_button(modal_temp, "ESC", event_temp_cancel, 48, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(lv_obj_get_child(modal_temp, 7), LV_ALIGN_CENTER, 0, 58);

    create_button(modal_temp, "SET", event_temp_apply, 48, 32, lv_color_hex(0x4CAF50));
    lv_obj_align(lv_obj_get_child(modal_temp, 8), LV_ALIGN_CENTER, 58, 58);

    // ----------------------------------------------------
    // POPUP MODALE: IMPOSTAZIONE VELOCITÀ (SPEED)
    // ----------------------------------------------------
    modal_speed = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal_speed, 240, 240);
    lv_obj_center(modal_speed);
    lv_obj_set_style_bg_color(modal_speed, lv_color_hex(0x181818), 0);
    lv_obj_set_style_radius(modal_speed, 120, 0);
    lv_obj_set_style_border_color(modal_speed, lv_color_hex(0x424242), 0);
    lv_obj_set_style_border_width(modal_speed, 2, 0);
    lv_obj_clear_flag(modal_speed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_speed, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_title_sp = lv_label_create(modal_speed);
    lv_label_set_text(lbl_title_sp, "SET SPEED");
    lv_obj_set_style_text_color(lbl_title_sp, lv_color_hex(0x00A2FF), 0);
    lv_obj_set_style_text_font(lbl_title_sp, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_title_sp, LV_ALIGN_CENTER, 0, -72);

    lbl_modal_speed_val = lv_label_create(modal_speed);
    lv_label_set_text(lbl_modal_speed_val, "100");
    lv_obj_set_style_text_font(lbl_modal_speed_val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_modal_speed_val, lv_color_hex(0x00A2FF), 0);
    lv_obj_align(lbl_modal_speed_val, LV_ALIGN_CENTER, 0, -32);

    lv_obj_t *b_sp_m50 = create_button(modal_speed, "-50", event_speed_m50, 42, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_sp_m50, LV_ALIGN_CENTER, -66, 10);

    lv_obj_t *b_sp_m10 = create_button(modal_speed, "-10", event_speed_m10, 38, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_sp_m10, LV_ALIGN_CENTER, -22, 10);

    lv_obj_t *b_sp_p10 = create_button(modal_speed, "+10", event_speed_p10, 38, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_sp_p10, LV_ALIGN_CENTER, 22, 10);

    lv_obj_t *b_sp_p50 = create_button(modal_speed, "+50", event_speed_p50, 42, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_sp_p50, LV_ALIGN_CENTER, 66, 10);

    lv_obj_t *b_sp_rst = create_button(modal_speed, "RESET", event_speed_reset, 50, 32, lv_color_hex(0x00A2FF));
    lv_obj_align(b_sp_rst, LV_ALIGN_CENTER, -58, 58);

    lv_obj_t *b_sp_esc = create_button(modal_speed, "ESC", event_speed_cancel, 48, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_sp_esc, LV_ALIGN_CENTER, 0, 58);

    lv_obj_t *b_sp_set = create_button(modal_speed, "SET", event_speed_apply, 48, 32, lv_color_hex(0x4CAF50));
    lv_obj_align(b_sp_set, LV_ALIGN_CENTER, 58, 58);

    // ----------------------------------------------------
    // POPUP MODALE: IMPOSTAZIONE ACCELERAZIONE (ACCEL)
    // ----------------------------------------------------
    modal_accel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal_accel, 240, 240);
    lv_obj_center(modal_accel);
    lv_obj_set_style_bg_color(modal_accel, lv_color_hex(0x181818), 0);
    lv_obj_set_style_radius(modal_accel, 120, 0);
    lv_obj_set_style_border_color(modal_accel, lv_color_hex(0x424242), 0);
    lv_obj_set_style_border_width(modal_accel, 2, 0);
    lv_obj_clear_flag(modal_accel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_accel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_title_ac = lv_label_create(modal_accel);
    lv_label_set_text(lbl_title_ac, "SET ACCEL");
    lv_obj_set_style_text_color(lbl_title_ac, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(lbl_title_ac, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_title_ac, LV_ALIGN_CENTER, 0, -72);

    lbl_modal_accel_val = lv_label_create(modal_accel);
    lv_label_set_text(lbl_modal_accel_val, "5000");
    lv_obj_set_style_text_font(lbl_modal_accel_val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_modal_accel_val, lv_color_hex(0xFFB300), 0);
    lv_obj_align(lbl_modal_accel_val, LV_ALIGN_CENTER, 0, -32);

    lv_obj_t *b_ac_m1k = create_button(modal_accel, "-1k", event_accel_m1000, 44, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_ac_m1k, LV_ALIGN_CENTER, -66, 10);

    lv_obj_t *b_ac_m100 = create_button(modal_accel, "-100", event_accel_m100, 40, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_ac_m100, LV_ALIGN_CENTER, -22, 10);

    lv_obj_t *b_ac_p100 = create_button(modal_accel, "+100", event_accel_p100, 40, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_ac_p100, LV_ALIGN_CENTER, 22, 10);

    lv_obj_t *b_ac_p1k = create_button(modal_accel, "+1k", event_accel_p1000, 44, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_ac_p1k, LV_ALIGN_CENTER, 66, 10);

    lv_obj_t *b_ac_rst = create_button(modal_accel, "RESET", event_accel_reset, 50, 32, lv_color_hex(0xFFB300));
    lv_obj_align(b_ac_rst, LV_ALIGN_CENTER, -58, 58);

    lv_obj_t *b_ac_esc = create_button(modal_accel, "ESC", event_accel_cancel, 48, 32, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_ac_esc, LV_ALIGN_CENTER, 0, 58);

    lv_obj_t *b_ac_set = create_button(modal_accel, "SET", event_accel_apply, 48, 32, lv_color_hex(0x4CAF50));
    lv_obj_align(b_ac_set, LV_ALIGN_CENTER, 58, 58);

    // ----------------------------------------------------
    // POPUP MODALE: IMPOSTAZIONE VENTOLA (FAN)
    // ----------------------------------------------------
    modal_fan = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal_fan, 240, 240);
    lv_obj_center(modal_fan);
    lv_obj_set_style_bg_color(modal_fan, lv_color_hex(0x181818), 0);
    lv_obj_set_style_radius(modal_fan, 120, 0);
    lv_obj_set_style_border_color(modal_fan, lv_color_hex(0x424242), 0);
    lv_obj_set_style_border_width(modal_fan, 2, 0);
    lv_obj_clear_flag(modal_fan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_fan, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_title_fn = lv_label_create(modal_fan);
    lv_label_set_text(lbl_title_fn, "SET FAN");
    lv_obj_set_style_text_color(lbl_title_fn, lv_color_hex(0x26C6DA), 0);
    lv_obj_set_style_text_font(lbl_title_fn, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_title_fn, LV_ALIGN_CENTER, 0, -74);

    lbl_modal_fan_val = lv_label_create(modal_fan);
    lv_label_set_text(lbl_modal_fan_val, "0%");
    lv_obj_set_style_text_color(lbl_modal_fan_val, lv_color_hex(0x26C6DA), 0);
    lv_obj_set_style_text_font(lbl_modal_fan_val, &lv_font_montserrat_24, 0);
    lv_obj_align(lbl_modal_fan_val, LV_ALIGN_CENTER, 0, -36);

    slider_modal_fan = lv_slider_create(modal_fan);
    lv_obj_set_size(slider_modal_fan, 150, 14);
    lv_obj_align(slider_modal_fan, LV_ALIGN_CENTER, 0, 4);
    lv_slider_set_range(slider_modal_fan, 0, 100);
    lv_slider_set_value(slider_modal_fan, current_fan_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_modal_fan, lv_color_hex(0x263238), LV_PART_MAIN);
    lv_obj_set_style_radius(slider_modal_fan, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_modal_fan, lv_color_hex(0x0284C7), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_modal_fan, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_modal_fan, lv_color_hex(0x38BDF8), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_modal_fan, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider_modal_fan, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider_modal_fan, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_modal_fan, event_modal_fan_slider, LV_EVENT_ALL, NULL);

    lv_obj_t *b_fn_0 = create_button(modal_fan, "0%", event_fan_preset_0, 44, 28, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_fn_0, LV_ALIGN_CENTER, -54, 40);

    lv_obj_t *b_fn_50 = create_button(modal_fan, "50%", event_fan_preset_50, 44, 28, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_fn_50, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *b_fn_100 = create_button(modal_fan, "100%", event_fan_preset_100, 44, 28, lv_color_hex(0x2A2F3D));
    lv_obj_align(b_fn_100, LV_ALIGN_CENTER, 54, 40);

    // Tasto Indietro (capsula smartwatch) in basso
    lv_obj_t *btn_fn_close = lv_btn_create(modal_fan);
    lv_obj_set_size(btn_fn_close, 106, 28);
    lv_obj_align(btn_fn_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(btn_fn_close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_fn_close, lv_color_hex(0x28303C), 0);
    lv_obj_set_style_bg_color(btn_fn_close, lv_color_hex(0x3E4856), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_fn_close, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_fn_close, 0, 0);
    lv_obj_set_style_pad_all(btn_fn_close, 0, 0);
    lv_obj_set_style_shadow_width(btn_fn_close, 0, 0);
    lv_obj_add_event_cb(btn_fn_close, event_modal_fan_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_fn_close = lv_label_create(btn_fn_close);
    lv_label_set_text(lbl_fn_close, LV_SYMBOL_LEFT " INDIETRO");
    lv_obj_set_style_text_font(lbl_fn_close, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_fn_close, lv_color_hex(0xCFD8DC), 0);
    lv_obj_clear_flag(lbl_fn_close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(lbl_fn_close);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[UI] init_ui() END. System Free heap: %u, LVGL MEM used: %d / %d (%d%%), free: %d, max_free: %d\n",
        (unsigned int)ESP.getFreeHeap(), (int)(mon.total_size - mon.free_size), (int)mon.total_size, (int)mon.used_pct, (int)mon.free_size, (int)mon.free_biggest_size);
}

// ==========================================
// BACKGROUND TASK (MOONRAKER NETWORK POLLING)
// ==========================================
void moonraker_task(void *pvParameters) {
    for (;;) {
        if (WiFi.status() == WL_CONNECTED && ui_initialized) {
            unsigned long now = millis();

            // 1. Se la lista file è stata richiesta, scaricala in background
            if (request_file_list) {
                request_file_list = false;
                bg_fetch_files();
            }

            // 2. Se la console è aperta, aggiornala in automatico ogni 0.5s (500ms)
            if (is_console_modal_open) {
                if (now - last_console_query >= 500) {
                    last_console_query = now;
                    bg_fetch_console();
                }
            }

            // 3. Telemetria periodica (stato, temperature, velocità, accelerazione) ogni 1500ms
            if (now - last_query_time >= QUERY_INTERVAL) {
                last_query_time = now;
                queryMoonraker();
            }
        } else if (ui_initialized && WiFi.status() != WL_CONNECTED) {
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                set_printer_state(STATUS_ERROR);
                xSemaphoreGive(lvgl_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==========================================
// SETUP & LOOP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n--- Avvio KlipperScreen ESP32-2424S012 (High Perf 80MHz) ---");

    // 0. Caricamento orientamento schermo da memoria NVS
    prefs.begin("screen", false);
    screen_rotation = prefs.getUChar("rot", 0) % 4;
    prefs.end();
    Serial.printf("[DISPLAY] Orientamento caricato: %d (%s)\n", screen_rotation, get_rotation_text(screen_rotation));

    // 1. Spegnimento iniziale Retroilluminazione per evitare qualsiasi glitch visivo
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, LOW);

    // 2. Inizializzazione Hardware Display e pulizia immediata a NERO PURO
    tft.begin();
    tft.setRotation(screen_rotation);
    tft.fillScreen(TFT_BLACK);

    // 3. Accensione Retroilluminazione su schermo completamente nero (nessun artefatto visibile)
    digitalWrite(TFT_BL_PIN, HIGH);
    delay(250); // Schermo completamente nero prima della splash screen

    // 4. Inizializzazione Touch Screen
    touch.begin();
    Wire.setClock(TOUCH_I2C_FREQ); // Fast I2C mode per minima latenza touch

    // 5. Inizializzazione Mutex e LVGL con doppio buffer espanso a 60 linee (aligned)
    lvgl_mutex = xSemaphoreCreateMutex();
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, screenWidth * 60);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.screen_transp = 0;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // 6. Mostra Splash Screen per 2.5 secondi esatti con rendering sincrono
    show_splash_screen();
    lv_timer_handler(); // Disegna immediatamente la splash screen su hardware GC9A01
    Serial.println("[BOOT] Splash screen rendered to display");

    uint32_t t_splash = millis();
    while (millis() - t_splash < 2500) {
        lv_timer_handler();
        delay(20);
    }

    // 7. Rimuovi la splash screen e Inizializza l'Interfaccia Utente (Dashboard)
    if (obj_splash && lv_obj_is_valid(obj_splash)) {
        lv_obj_del(obj_splash);
        obj_splash = NULL;
    }
    init_ui();
    ui_initialized = true;

    // Forza invalidazione e refresh COMPLETO del display hardware (Dashboard a video)
    Serial.println("[DBG] Calling lv_obj_invalidate...");
    lv_obj_invalidate(lv_scr_act());
    Serial.println("[DBG] Calling first lv_timer_handler()...");
    lv_timer_handler();
    Serial.println("[DBG] First lv_timer_handler() finished!");
    delay(50);
    lv_timer_handler();
    Serial.println("[UI] Dashboard rendered and active on screen!");

    // 8. Connessione Wi-Fi (solo ORA, dopo che la UI e gia visibile a schermo)
    Serial.print("Connessione a WiFi: ");
    Serial.println(WIFI_SSID);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
        if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            Serial.println("[WiFi] Connesso all'AP!");
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            Serial.print("[WiFi] IP ottenuto: ");
            Serial.println(WiFi.localIP());
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[WiFi] Disconnesso (motivo: %d)\n", info.wifi_sta_disconnected.reason);
        }
    });

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // 9. Avvio Task Moonraker FreeRTOS (esegue in background senza bloccare il rendering)
    xTaskCreate(
        moonraker_task,
        "MoonrakerTask",
        16384,
        NULL,
        1,
        &moonrakerTaskHandle
    );

    Serial.println("[SETUP] Setup complete, entering loop. Free heap: " + String(ESP.getFreeHeap()));
}

void loop() {
    // Gestione LVGL con protezione mutex
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        lv_timer_handler();

        // Lampeggio stato Errore (arancione lampeggiante ogni 500ms)
        if (ui_initialized && current_printer_state == STATUS_ERROR) {
            if (millis() - last_ring_blink > 500) {
                last_ring_blink = millis();
                ring_blink_state = !ring_blink_state;
                update_status_rings_color(ring_blink_state ? lv_color_hex(0xFF9800) : lv_color_hex(0x000000));
            }
        }
        xSemaphoreGive(lvgl_mutex);
    }

    // Comandi di test diagnostici via Porta Seriale
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == 'c') {
            Serial.println("[CMD] Open console via serial");
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                event_open_console(NULL);
                xSemaphoreGive(lvgl_mutex);
            }
        } else if (ch == 'f') {
            Serial.println("[CMD] Open files via serial");
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                event_open_files(NULL);
                xSemaphoreGive(lvgl_mutex);
            }
        } else if (ch == 'x') {
            Serial.println("[CMD] Close modals via serial");
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (modal_console) {
                    lv_obj_add_flag(modal_console, LV_OBJ_FLAG_HIDDEN);
                    is_console_modal_open = false;
                    if (cont_console_list) {
                        lv_obj_clean(cont_console_list);
                        lbl_console_status = NULL;
                    }
                    last_console_text = "";
                }
                if (modal_files) {
                    lv_obj_add_flag(modal_files, LV_OBJ_FLAG_HIDDEN);
                    if (cont_files_list) {
                        lv_obj_clean(cont_files_list);
                        lbl_files_status = NULL;
                    }
                }
                xSemaphoreGive(lvgl_mutex);
            }
        } else if (ch == 'm') {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            Serial.printf("[MEM] Free Heap: %u, LVGL used: %d/%d (%d%%), free: %d\n",
                (unsigned int)ESP.getFreeHeap(), (int)(mon.total_size - mon.free_size), (int)mon.total_size, (int)mon.used_pct, (int)mon.free_size);
            if (moonrakerTaskHandle) {
                UBaseType_t wm = uxTaskGetStackHighWaterMark(moonrakerTaskHandle);
                Serial.printf("[STACK] MoonrakerTask watermark: %u bytes\n", (unsigned int)(wm * sizeof(StackType_t)));
            }
        }
    }

    delay(2);
}