#ifndef CONFIG_H
#define CONFIG_H

// --- 1. 16x16 灯光矩阵配置 ---
#define LED_PIN     4      // WS2812B 数据信号线
#define NUM_LEDS    256    // 灯珠总数
#define BRIGHTNESS  30     // 初始亮度 (0-255)

// --- 2. 跨板串口通信配置 (与 S3 对接) ---
/**
 * 🌟【硬核物理引脚校准】：完全对齐你的最新连线拓扑
 * 你的连线：WROOM-32 的 RX2 (GPIO 16) <- 接 S3 的 17 (TX)
 * 你的连线：WROOM-32 的 TX2 (GPIO 17) -> 接 S3 的 18 (RX)
 * * 注意：由于你在硬件上使用的是 WROOM-32 的硬件串口 2 物理引脚，
 * 这里我们将 S3_RX 定为 16，S3_TX 定为 17。
 * 此时 LightSystem3.ino 中的 Serial1.begin(115200, SERIAL_8N1, S3_RX, S3_TX) 
 * 会通过内部矩阵，自动将底层的异步收发器无缝桥接到你的 RX2/TX2 物理线上，完美兼容！
 */
#define S3_RX       16     // 对应 WROOM-32 的 RX2 接口
#define S3_TX       17     // 对应 WROOM-32 的 TX2 接口

// --- 3. NVIDIA Jetson Nano 串口通信配置 ---
// 使用 GPIO 25 (RX) 和 GPIO 26 (TX) 进行杜邦线串口通信，以完全避开板载 USB 调试串口的电平冲突。
// 物理接线：
//   - Jetson Nano J41 Pin 8 (TX) -> ESP32 GPIO 25
//   - Jetson Nano J41 Pin 10 (RX) -> ESP32 GPIO 26
//   - Jetson Nano GND -> ESP32 GND
#define JETSON_RX   25     
#define JETSON_TX   26     

// --- 4. 统一最大功耗约束 ---
#define MAX_POWER_VOLTS     5      // 最大电压 5V
#define MAX_POWER_MA        1200   // 最大电流 1200mA (1.2A)

// --- 5. Blinker & WiFi 凭证 ---
#if __has_include("Config.private.h")
#include "Config.private.h"
#else
// 如果没有私有配置文件，使用默认占位符（用户复制模版后修改）
const char auth[] = "YOUR_BLINKER_AUTH"; 
const char ssid[] = "YOUR_WIFI_SSID";
const char pswd[] = "YOUR_WIFI_PASSWORD"; 
#endif

#endif