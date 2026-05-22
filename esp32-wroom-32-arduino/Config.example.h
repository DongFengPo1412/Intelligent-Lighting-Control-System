#ifndef CONFIG_H
#define CONFIG_H

// 1. 16x16 WS2812B LED matrix configuration.
#define LED_PIN     4
#define NUM_LEDS    256
#define BRIGHTNESS  30

// 2. UART connection to the ESP32-S3 board.
// WROOM-32 RX2 GPIO16 <- ESP32-S3 TX
// WROOM-32 TX2 GPIO17 -> ESP32-S3 RX
#define S3_RX       16
#define S3_TX       17

// 3. UART connection to NVIDIA Jetson Nano.
// Jetson J41 Pin 8  TX -> ESP32 GPIO25
// Jetson J41 Pin 10 RX <- ESP32 GPIO26
// Jetson GND            -> ESP32 GND
#define JETSON_RX   25
#define JETSON_TX   26

// 4. Blinker and WiFi credentials.
// Copy this file to Config.h and fill in your private values locally.
const char auth[] = "YOUR_BLINKER_AUTH";
const char ssid[] = "YOUR_WIFI_SSID";
const char pswd[] = "YOUR_WIFI_PASSWORD";

#endif
