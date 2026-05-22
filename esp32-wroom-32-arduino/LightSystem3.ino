#define BLINKER_WIFI
#include <WiFi.h>
#include <FastLED.h>
#include "Config.h"
#include "DisplayManager.h"
#include "GameEngine.h"
#include "BlinkerManager.h"
#include "SpectrumManager.h" // 🌟【更新】：引入 4块 8x8 纯直连拼接音律管理器

// --- 全局变量定义 ---
CRGB leds[NUM_LEDS];          
uint8_t gHue = 0;              
int currentMode = 0;          // 当前运行模式
int lastMode = -1;            
String serial1Buffer = "";    // 接收 S3 (大脑) 的指令
String serial2Buffer = "";    // 接收 NVIDIA Jetson Nano (人脸与手势识别) 的指令 (UART2)

// --- 实例化音律随动核心 ---
SpectrumManager spectrum(&leds[0]); // 将 FastLED 的物理灯珠数组首地址挂载进去

// --- 游戏引擎变量定义 (WROOM-32 专用) ---
int snakeX[256], snakeY[256], snakeLen = 3;
int sDx = 1, sDy = 0;
int foodX, foodY;
int snakeScore = 0;
int snakeSpeed = 450; 
unsigned long lastSnakeUpdate = 0;
bool snakeStarted = false;

uint8_t tetrisField[16][16] = {0}; 
int pType, pRot, pX, pY;
int tetrisScore = 0;
int tetrisSpeed = 800;
unsigned long lastTetUpdate = 0;
bool tetrisStarted = false;

/**
 * @brief 统筹模式切换时的初始化工作
 */
void changeToMode(int m) {
    currentMode = m;
    FastLED.clear();
    // 每次切换模式重置一次亮度，防止“呼吸极光”等模式改变了全局亮度
    FastLED.setBrightness(BRIGHTNESS); 
    Serial.printf(">>> [核心]: 模式切换 -> %d\n", m);
}

/**
 * @brief 核心：解析来自 ESP32-S3 的串口命令
 * 逻辑：配备工业级“动态指令去噪铁闸”，全面绝杀后台心跳包无脑重置 0 的覆盖冲突！
 */
/**
 * @brief 核心：解析来自 ESP32-S3 的串口命令
 * 逻辑：加装微秒级连击去抖过滤器，只要当前要切 15 或者音律流在跑，100% 物理蒸发所有的重置 0！
 */
void handleS3Communication() {
    static uint32_t last_valid_mode_time = 0;
    // 🌟 记录上一次收到真正 "f," 音律流的时间戳
    static uint32_t last_fft_stream_time = 0;

    while (Serial1.available() > 0) {
        char c = Serial1.read();
        
        if (c == '\n' || c == '\r') {
            serial1Buffer.trim();
            if (serial1Buffer.length() > 0) {
                
                // 1. 核心流拦截：只要是音律高频流数据，无视状态直接喂给随动引擎
                if (serial1Buffer.startsWith("f,")) {
                    last_fft_stream_time = millis(); // 🎯 只要音律流在跑，实时疯狂更新这个时间戳！
                    if (currentMode == 15) {
                        spectrum.parseSerialData(serial1Buffer); 
                    }
                } 
                // 2. 特殊逻辑：收到 "1" 开启纯红模式
                else if (serial1Buffer == "1") {
                    // 🌟 拦截判定：如果最近 1.2 秒内音乐流还在疯狂涌入，直接拒绝任何切模！
                    if (currentMode == 15 && (millis() - last_fft_stream_time < 1200)) {
                        serial1Buffer = ""; return;
                    }
                    changeToMode(1); 
                    last_valid_mode_time = millis(); 
                } 
                // 3. 常规模式切换指令（如 0, 6, 10, 15）
                else {
                    int cmd = serial1Buffer.toInt();
                    
                    // 🌟🌟🌟【终极免控铁闸：死守音乐大动脉】🌟🌟🌟
                    // 如果当前已经是音乐随动模式（15），且最近 1.2 秒内一直有高频音律数据涌入
                    // 此时突然进来的任何数字（不管是心跳包0，还是误触发的6、10），全部当场物理蒸发！
                    if (currentMode == 15 && (millis() - last_fft_stream_time < 1200)) {
                        if (cmd != 15) { // 如果发过来的不是 15 本身，一律按误触枪毙
                            Serial.printf(">>> [全防御铁闸]: 律动高潮中，成功拦截并蒸发误触发指令: %d！\n", cmd);
                            serial1Buffer = "";
                            return; 
                        }
                    }

                    // 4. 防偷袭去抖（针对非15模式下的正常心跳0防御）
                    if (cmd == 0) {
                        if (currentMode != 0 && (millis() - last_valid_mode_time < 4000) || Serial1.available() > 0) {
                            Serial.printf(">>> [防重置铁闸]: 拦截常规状态心跳误发指令 0！\n");
                            serial1Buffer = "";
                            return; 
                        }
                    }

                    // 5. 确认不是偷袭，放行执行
                    if (cmd != currentMode) {
                        if (cmd == 15) {
                            changeToMode(15);
                            last_valid_mode_time = millis();
                            last_fft_stream_time = millis(); // 初始化流时间
                            while(Serial1.available() > 0) { Serial1.read(); } // 荡平尾气
                            serial1Buffer = "";
                            return;
                        }

                        // 常规合法切模
                        changeToMode(cmd);
                        if (cmd != 0) {
                            last_valid_mode_time = millis();
                        }
                    }
                }
                
                serial1Buffer = ""; 
            }
        } else {
            serial1Buffer += c; 
        }
    }
}

/**
 * @brief 解析并处理来自 NVIDIA Jetson Nano (人脸/手势识别) 的串口指令 (UART2)
 */
void processSerial2Command() {
    serial2Buffer.trim();
    if (serial2Buffer.length() > 0) {
        // 如果是普通控制指令（不是音律流），则打印调试日志，避免音律高频流导致串口拥堵
        if (!serial2Buffer.startsWith("f,")) {
            Serial.printf(">>> [串口2]: 收到控制信号: %s\n", serial2Buffer.c_str());
        }
        
        // 如果是音律包数据且当前处于模式 15，则由随动引擎解析
        if (serial2Buffer.startsWith("f,") && currentMode == 15) {
            spectrum.parseSerialData(serial2Buffer);
        } else {
            // 校验非音律流命令是否为纯数字，防止空包/空格/串口噪声干扰误触发切模到0
            bool isNumeric = true;
            for (unsigned int i = 0; i < serial2Buffer.length(); i++) {
                if (!isDigit(serial2Buffer.charAt(i))) {
                    isNumeric = false;
                    break;
                }
            }
            if (isNumeric) {
                int targetMode = serial2Buffer.toInt();
                changeToMode(targetMode);
            } else {
                Serial.printf(">>> [串口2]: 拦截非数字噪声信号: %s\n", serial2Buffer.c_str());
            }
        }
    }
    serial2Buffer = ""; // 强行清空缓冲区，绝杀超时断帧的潜在空转死循环
}

void handleSerial2Communication() {
    static unsigned long lastByteTime = 0;
    while (Serial2.available() > 0) {
        char c = Serial2.read();
        lastByteTime = millis();
        if (c == '\n' || c == '\r') {
            processSerial2Command();
        } else {
            serial2Buffer += c;
        }
    }
    
    // 【超时自动断帧逻辑】：为了兼容没有发送换行符（如 echo -n "1" 或有些 Python 串口库未加换行）的情况。
    // 如果缓冲区有字符，且距离最后一个字符读入已超过 50 毫秒，则强制触发解析并清空缓冲区。
    if (serial2Buffer.length() > 0 && (millis() - lastByteTime > 50)) {
        processSerial2Command();
    }
}

/**
 * @brief 解析并处理来自 PC USB 调试端口 (Serial/UART0) 的指令
 */
void handleUSBSerialCommunication() {
    static String usbBuffer = "";
    static unsigned long lastUsbByteTime = 0;
    while (Serial.available() > 0) {
        char c = Serial.read();
        lastUsbByteTime = millis();
        if (c == '\n' || c == '\r') {
            if (usbBuffer.length() > 0) {
                usbBuffer.trim();
                if (usbBuffer.length() > 0) {
                    Serial.printf(">>> [USB调试]: 收到指令: %s\n", usbBuffer.c_str());
                    bool isNumeric = true;
                    for (unsigned int i = 0; i < usbBuffer.length(); i++) {
                        if (!isDigit(usbBuffer.charAt(i))) {
                            isNumeric = false;
                            break;
                        }
                    }
                    if (isNumeric) {
                        changeToMode(usbBuffer.toInt());
                    } else {
                        Serial.printf(">>> [USB调试]: 拦截非数字信号: %s\n", usbBuffer.c_str());
                    }
                }
                usbBuffer = "";
            }
        } else {
            usbBuffer += c;
        }
    }
    
    // 超时自动解析：如果距离最后一个字符读入已超过 50 毫秒且没有换行符
    if (usbBuffer.length() > 0 && (millis() - lastUsbByteTime > 50)) {
        usbBuffer.trim();
        if (usbBuffer.length() > 0) {
            Serial.printf(">>> [USB调试]: 超时解析收到指令: %s\n", usbBuffer.c_str());
            bool isNumeric = true;
            for (unsigned int i = 0; i < usbBuffer.length(); i++) {
                if (!isDigit(usbBuffer.charAt(i))) {
                    isNumeric = false;
                    break;
                }
            }
            if (isNumeric) {
                changeToMode(usbBuffer.toInt());
            } else {
                Serial.printf(">>> [USB调试]: 拦截非数字信号: %s\n", usbBuffer.c_str());
            }
        }
        usbBuffer = "";
    }
}

void setup() {
    // 1. 初始化与 PC 串口调试 (UART0)
    Serial.begin(115200);
    
    // 🌟【防浮空电磁屏蔽】：强制拉高串口 RX 物理引脚，防止在杜邦线未接或松动时，引脚高阻抗悬空吸收空间电磁辐射产生垃圾乱码
    pinMode(JETSON_RX, INPUT_PULLUP);
    pinMode(S3_RX, INPUT_PULLUP);
    
    // 1.5 初始化与 NVIDIA Jetson Nano 异步串口通信 (UART2，引脚定义在 Config.h 中)
    Serial2.begin(115200, SERIAL_8N1, JETSON_RX, JETSON_TX);
    
    // 2. 初始化跨板通信串口 (与 S3 对接)
    // 使用 Config.h 中定义的 S3_RX (16) 和 S3_TX (17)
    Serial1.begin(115200, SERIAL_8N1, S3_RX, S3_TX); 
    
    // 3. 联网 (Blinker 远程控制)
    WiFi.begin(ssid, pswd);
    Serial.print(">>> [系统]: 正在连接 WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n>>> [系统]: WiFi 已连接");

    // 4. 初始化 FastLED 与 logic 模块
    initDisplay();    // 内部已包含 FastLED.addLeds
    initBlinker();    
    
    Serial.println("--- [UESTC 智能灯阵执行端: WROOM-32 已启动] ---");
}

void loop() {
    // 1. Blinker 物联网心跳
    Blinker.run(); 

    // 2. 监听 S3 (大脑) 指令
    handleS3Communication(); 

    // 3. 监听 NVIDIA Jetson Nano (人脸/手势识别) 的串口指令 (UART2)
    handleSerial2Communication();

    // 3.5 监听 PC USB 调试串口 (Serial) 的指令
    handleUSBSerialCommunication();

    // 4. 模式切换初始化调度 (仅在切换瞬间运行一次)
    if (currentMode != lastMode) {
        printModeName(currentMode); // 串口打印当前模式名称
        
        if(currentMode == 19) initSnake();
        if(currentMode == 21) initTetris();
        
        lastMode = currentMode;
    }

    // 5. 渲染引擎状态机
    switch (currentMode) {
        case 15:
            // 🌟 15号音乐模式下，整个画面的刷新完全交由串口数据流中断进行“主驱动刷新”
            // 主循环此处保持静止，不执行任何常规特效，腾出全部主频算力死守串口大动脉
            break;
        case 19: runSnake(); break;               
        case 21: runTetris(); break;               
        case 20: drawGameScore(snakeScore); break; 
        case 22: drawGameScore(tetrisScore); break;
        // 16 号模式在 DisplayManager 对应 AI 唤醒反馈表情
        case 16: drawFace(CRGB::Yellow, 1); break; 
        default: runDisplayEffects(currentMode); break; 
    }

    // 6. 全局彩虹色相更新
    EVERY_N_MILLISECONDS(20) { gHue++; }
    
    // 7. 物理刷新灯板 (限制在 30FPS 左右，保证 WROOM-32 稳定性)
    // 🌟【精细微调】：音乐模式 15 下彻底跳过此处的刷新锁，把帧率上限全部释放给 SpectrumManager 内部的 FastLED.show()！
    if (currentMode != 15) {
        static unsigned long lastShow = 0;
        if (millis() - lastShow > 30) { 
            lastShow = millis();
            FastLED.show();
        }
    }
}