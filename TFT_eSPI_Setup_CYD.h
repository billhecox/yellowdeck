// Copy this file into your TFT_eSPI library's "User_Setups" folder,
// then uncomment the matching include line in "User_Setup_Select.h".
//
// Target: ESP32-2432S028R "Cheap Yellow Display" (CYD)
// Display: 2.8" 240x320 ILI9341 over SPI
// Touch: XPT2046 (not used by the hello sketch, but wired)

#define USER_SETUP_INFO "ESP32-2432S028R Cheap Yellow Display"

#define ILI9341_DRIVER

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1      // Reset is pulled high on many boards; -1 lets the driver ignore it
#define TFT_BL   21      // Backlight PWM pin

#define TOUCH_CS 33      // XPT2046 chip select

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY  27000000     // Lowered from 40 MHz: fixes black/garbled screen on some CYD boards
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000
