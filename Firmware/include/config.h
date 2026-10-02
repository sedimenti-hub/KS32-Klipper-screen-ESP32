#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// 1. BOARD / MCU SELECTION (ESP32)
// =============================================================================
// Select the board/ESP32 type by uncommenting the one in use.
// This setting serves as a reference for predefined pinouts.
#define BOARD_ESP32_C3_DEVKIT       // ESP32-C3 DevKitM-1 (Current default)
// #define BOARD_ESP32_STANDARD     // ESP32 WROOM / DevKit V1 (30 or 38 pins)
// #define BOARD_ESP32_S3           // ESP32-S3 DevKit
// #define BOARD_CUSTOM             // Custom board

// =============================================================================
// 2. DISPLAY CONTROLLER CONFIGURATION (TFT)
// =============================================================================
// Display controller model used:
// (Enable the corresponding driver for TFT_eSPI or GFX)
#define DISPLAY_CONTROLLER_GC9A01    // 1.28" Round Display (240x240)
// #define DISPLAY_CONTROLLER_ST7789 // 1.3" / 1.54" / 2.0" IPS Display
// #define DISPLAY_CONTROLLER_ILI9341// 2.4" / 2.8" / 3.2" Display (320x240)
// #define DISPLAY_CONTROLLER_ST7735 // 1.8" Display (128x160)

// Native display resolution (in pixels)
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH   240
#endif

#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT  240
#endif

// Display Pins (SPI Bus + Control Signals)
// Adjust these pins according to your wiring to the ESP32:
#ifndef TFT_MOSI_PIN
#define TFT_MOSI_PIN    7   // SPI MOSI Data (SDA / DIN on the display)
#endif

#ifndef TFT_SCLK_PIN
#define TFT_SCLK_PIN    6   // SPI Clock (SCL / SCK on the display)
#endif

#ifndef TFT_CS_PIN
#define TFT_CS_PIN     10   // Chip Select (CS)
#endif

#ifndef TFT_DC_PIN
#define TFT_DC_PIN      2   // Data / Command (DC or RS)
#endif

#ifndef TFT_RST_PIN
#define TFT_RST_PIN    -1   // Reset (-1 if connected to board EN / 3.3V)
#endif

#ifndef TFT_BL_PIN
#define TFT_BL_PIN      3   // Backlight (BL / Backlight, -1 if hardwired to 3.3V)
#endif

#ifndef TFT_MISO_PIN
#define TFT_MISO_PIN   -1   // SPI MISO (-1 if the display does not return MISO data)
#endif

// Display SPI bus frequency (Hz)
#ifndef DISPLAY_SPI_FREQ
#define DISPLAY_SPI_FREQ 80000000 // 80 MHz for smooth 60 FPS rendering
#endif

// =============================================================================
// 3. TOUCH SCREEN CONTROLLER CONFIGURATION
// =============================================================================
// Supported touch controller:
#define TOUCH_CONTROLLER_CST816S     // Capacitive touch for round display (I2C)
// #define TOUCH_CONTROLLER_FT6236   // Alternative capacitive touch (I2C)
// #define TOUCH_CONTROLLER_XPT2046  // Resistive touch (SPI)

// Touch Screen Pins (I2C Bus + Status/Control Signals)
// Adjust these pins according to your wiring to the ESP32:
#ifndef TOUCH_SDA_PIN
#define TOUCH_SDA_PIN   4   // I2C Data (SDA)
#endif

#ifndef TOUCH_SCL_PIN
#define TOUCH_SCL_PIN   5   // I2C Clock (SCL)
#endif

#ifndef TOUCH_RST_PIN
#define TOUCH_RST_PIN   1   // Touch hardware reset (RST)
#endif

#ifndef TOUCH_INT_PIN
#define TOUCH_INT_PIN   0   // Touch interrupt pin (INT / IRQ)
#endif

// Touch I2C bus frequency (Hz)
#ifndef TOUCH_I2C_FREQ
#define TOUCH_I2C_FREQ 400000 // 400 kHz Fast Mode for minimal touch latency
#endif

// ==========================================
// WI-FI CONFIGURATION
// ==========================================
const char* const WIFI_SSID = "YOUR_WIFI_NAME";
const char* const WIFI_PASS = "YOUR_PASSword_WIFI";

// ==========================================
// MOONRAKER CONFIGURATION (KLIPPER)
// ==========================================
// Enter your Raspberry Pi 5 IP address
const char* const MOONRAKER_HOST = "192.168.1.10";
const uint16_t MOONRAKER_PORT = 7125;

// ==========================================
// PRINTER / G-CODE PARAMETERS
// ==========================================
// Extrusion/retraction amount (in mm)
#define EXTRUDE_MM 10
#define RETRACT_MM 10
#define EXTRUDE_FEEDRATE 300 // mm/min

// Jog movement step for X, Y, Z axes (in mm)
#define JOG_STEP_XY 10
#define JOG_STEP_Z  5
#define JOG_FEEDRATE_XY 3000 // mm/min
#define JOG_FEEDRATE_Z  600  // mm/min

// Light G-code macro or command (customizable according to your printer.cfg)
// If you use a Klipper macro named TOGGLE_LIGHT or SET_PIN PIN=caselight:
#define GCODE_LIGHT_ON  "SET_LED LED=chamber_led WHITE=1.00 SYNC=0 TRANSMIT=1"
#define GCODE_LIGHT_OFF "SET_LED LED=chamber_led WHITE=0.00 SYNC=0 TRANSMIT=1"

// Calibration commands and sensors
#define GCODE_BED_LEVEL       "BED_MESH_CALIBRATE"
#define GCODE_RESONANCE       "SHAPER_CALIBRATE"
#define FILAMENT_SENSOR_NAME  "runout_sensor"

// Customizable macros (3 buttons in the Accessories subpage)
#define GCODE_MACRO_1  "MACRO_1"
#define GCODE_MACRO_2  "MACRO_2"
#define GCODE_MACRO_3  "MACRO_3"

#endif // CONFIG_H
