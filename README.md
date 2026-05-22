# Intelligent Lighting Control System

这是一个智能灯光控制系统的总仓库，包含两个 ESP32 固件工程：

- `esp32-s3r16n8-idf/`：ESP32-S3R16N8 端工程，使用 VS Code + ESP-IDF 开发。
- `esp32-wroom-32-arduino/`：ESP32-WROOM-32 灯阵执行端工程，使用 Arduino 开发。

两个工程属于同一个系统，但面向不同硬件。把它们放在同一个仓库中，便于统一管理版本、文档、接线说明和后续课程项目材料。

## 仓库结构

```text
.
├─ esp32-s3r16n8-idf/          # ESP32-S3R16N8 / ESP-IDF 工程
├─ esp32-wroom-32-arduino/     # ESP32-WROOM-32 / Arduino 工程
├─ .gitignore
└─ README.md
```

## 系统功能

### ESP32-S3R16N8 端

该部分位于 `esp32-s3r16n8-idf/`，使用 ESP-IDF 开发，项目代码来自并保留了原 ESP32 智能语音/MCP 工程结构。代码中包含 WiFi 联网、WebSocket 或 MQTT+UDP 通信、语音交互、显示、设备侧 MCP 控制、LED/GPIO 等硬件控制能力，可作为系统中的智能交互与控制端。

### ESP32-WROOM-32 灯阵执行端

该部分位于 `esp32-wroom-32-arduino/`，主程序是 `LightSystem3.ino`，主要功能包括：

- 驱动 16x16 WS2812B 灯阵，共 256 颗灯珠。
- 支持纯色、彩虹、呼吸、流星、星点、波纹、表情等多种灯光模式。
- 通过 Blinker App 连接 WiFi，实现手机端远程控制。
- 通过 UART 接收 ESP32-S3R16N8 发送的模式指令。
- 通过 UART 接收 NVIDIA Jetson Nano 的人脸识别、手势识别控制指令。
- 支持 USB 串口调试，可直接发送数字模式指令。
- 支持音乐频谱模式，解析 `f,beat,b0,b1,...` 形式的频谱数据并实时刷新灯阵。
- 内置贪吃蛇和俄罗斯方块小游戏，并可通过 Blinker 按键控制方向、旋转和下落。

## Arduino 工程使用说明

1. 打开 `esp32-wroom-32-arduino/LightSystem3.ino`。
2. 安装 Arduino 依赖库：
   - `FastLED`
   - `Blinker`
   - ESP32 Arduino 开发板支持包
3. 复制 `esp32-wroom-32-arduino/Config.example.h` 为 `esp32-wroom-32-arduino/Config.h`。
4. 在本地 `Config.h` 中填写自己的 Blinker auth、WiFi 名称和 WiFi 密码。
5. 选择 ESP32-WROOM-32 对应开发板，编译并上传。

`Config.h` 包含个人 WiFi 和 Blinker 密钥，已经加入 `.gitignore`，不会上传到 GitHub。

## WROOM-32 接线说明

### LED 灯阵

- WS2812B 数据线连接到 ESP32-WROOM-32 的 GPIO4。
- 灯阵数量为 256，对应 16x16 矩阵。
- 灯阵供电需要根据实际灯珠数量提供足够电流，并与 ESP32 共地。

### 与 ESP32-S3R16N8 通信

- WROOM-32 GPIO16/RX2 接 ESP32-S3R16N8 的 TX。
- WROOM-32 GPIO17/TX2 接 ESP32-S3R16N8 的 RX。
- 两块板子需要共地。
- 串口波特率为 `115200`。

### 与 Jetson Nano 通信

- Jetson Nano J41 Pin 8/TX 接 WROOM-32 GPIO25。
- Jetson Nano J41 Pin 10/RX 接 WROOM-32 GPIO26。
- Jetson Nano GND 接 WROOM-32 GND。
- 串口波特率为 `115200`。

Jetson Nano 默认可能占用 `/dev/ttyTHS1`，调试前需要关闭 `nvgetty` 服务，并确保串口有读写权限。

## 指令约定

WROOM-32 端可以通过 USB 串口、ESP32-S3 串口或 Jetson Nano 串口接收指令：

- 数字指令：切换灯光模式，例如 `1` 表示红色模式，`15` 表示音乐频谱模式，`19` 表示贪吃蛇，`21` 表示俄罗斯方块。
- 频谱指令：以 `f,` 开头，例如 `f,beat,b0,b1,...`，用于音乐频谱实时显示。

代码中对串口噪声、无换行输入、音乐模式下的误触发切换做了过滤处理，以减少异常跳回默认模式的问题。

## ESP-IDF 工程使用说明

1. 使用 VS Code 打开 `esp32-s3r16n8-idf/`。
2. 安装 ESP-IDF 插件，并选择 ESP-IDF 5.4 或更高版本。
3. 根据实际硬件选择或调整配置。
4. 编译、烧录并通过串口监视器调试。

该子工程保留了原项目自己的 README、文档和分区表说明，更多细节可以查看 `esp32-s3r16n8-idf/README.md` 和 `esp32-s3r16n8-idf/docs/`。

## Git 忽略策略

仓库会上传源码、文档、默认配置和示例配置，不上传以下内容：

- 构建输出：`build/`、二进制文件、映射文件等。
- IDE 本地配置：`.vscode/`、`.clangd` 等。
- ESP-IDF 本地生成配置：`sdkconfig`、`sdkconfig.old`、`dependencies.lock`、`managed_components/`。
- Arduino 私密配置：`esp32-wroom-32-arduino/Config.h`。
- 压缩包、日志、缓存和临时文件。
