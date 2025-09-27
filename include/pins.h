#pragma once

// ESC pin
#define PIN_ESC 5
// ESP32 RX2 -> PDN_UART
#define TMC_RX_PIN 16
// ESP32 TX2 -> PDN_UART
#define TMC_TX_PIN 17

// HX711 pins (alternative free pins)
#define HX_DT 33
#define HX_SCK 32

// Button pin for selecting left preset
const int BTN_L = 27;
// Button pin for selecting right preset
const int BTN_R = 25;
// Button pin for start switch
const int BTN_START = 26;

