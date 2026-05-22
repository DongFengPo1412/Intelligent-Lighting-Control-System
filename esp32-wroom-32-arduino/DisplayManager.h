#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <FastLED.h>
#include "Config.h"

// --- 外部变量引用 (定义在主 .ino 中) ---
extern CRGB leds[NUM_LEDS];
extern uint8_t gHue; 

// --- 5x3 数字点阵数据 (用于显示贪吃蛇/俄罗斯方块分数) ---
const uint8_t PROGMEM numFont[10][5] = {
  {0x7,0x5,0x5,0x5,0x7}, {0x2,0x2,0x2,0x2,0x2}, {0x7,0x1,0x7,0x4,0x7},
  {0x7,0x1,0x7,0x1,0x7}, {0x5,0x5,0x7,0x1,0x1}, {0x7,0x4,0x7,0x1,0x7},
  {0x7,0x4,0x7,0x5,0x7}, {0x7,0x1,0x1,0x1,0x1}, {0x7,0x5,0x7,0x5,0x7}, {0x7,0x5,0x7,0x1,0x1}
};

// 🌟🌟🌟【黄金加固】：将所有的普通全局函数一律升级为 static 声明，彻底绝杀多重定义编译报错 🌟🌟🌟

/**
 * @brief 坐标映射核心：适配 4 块 8x8 纯直连拼接布局的 16x16 矩阵
 * 像素探针已实锤验证：内部走线完美对齐直连公式
 */
static void drawPixel(int x, int y, CRGB color) {
    if (x < 0 || x >= 16 || y < 0 || y >= 16) return;
    
    int blockID = (y / 8) * 2 + (x / 8); 
    int index = blockID * 64 + (y % 8) * 8 + (x % 8);
    leds[index] = color;
}

/**
 * @brief 在指定位置绘制 5x3 数字
 */
static void drawDigit(int n, int xO, int yO, CRGB c) {
    for (int i = 0; i < 5; i++) {
        uint8_t row = numFont[n % 10][i];
        for (int j = 0; j < 3; j++) {
            if (row & (0x4 >> j)) drawPixel(xO + j, yO + i, c);
        }
    }
}

/**
 * @brief 基础绘图工具：绘制表情 (1:笑脸, 2:哭脸, 3:平静)
 */
static void drawFace(CRGB color, int type) {
    FastLED.clear();
    // 绘制脸部轮廓（圆形）
    for(int y=0; y<16; y++) {
        for(int x=0; x<16; x++) {
            float d = sqrt(pow(x-7.5, 2) + pow(y-7.5, 2));
            if(d < 7.5 && d > 6.5) drawPixel(x, y, color);
        }
    }
    // 绘制眼睛
    drawPixel(5, 5, color); 
    drawPixel(10, 5, color);
    
    // 绘制嘴巴
    if(type == 1) { // 笑脸
        drawPixel(5, 11, color); drawPixel(10, 11, color);
        for(int i=6; i<=9; i++) drawPixel(i, 12, color);
    } else if(type == 2) { // 哭脸
        drawPixel(5, 12, color); drawPixel(10, 12, color);
        for(int i=6; i<=9; i++) drawPixel(i, 11, color);
    } else { // 平静
        for(int i=6; i<=9; i++) drawPixel(i, 11, color);
    }
}

/**
 * @brief 模式名称打印：用于串口调试
 */
static void printModeName(int m) {
    Serial.printf("\n>>> [显示终端]: 切换至模式 %d - ", m);
    switch(m) {
        case 1:  Serial.println("纯红模式"); break;
        case 2:  Serial.println("纯蓝模式"); break;
        case 3:  Serial.println("纯绿模式"); break;
        case 4:  Serial.println("幻彩霓虹"); break;
        case 5:  Serial.println("呼吸极光"); break;
        case 6:  Serial.println("流星划过"); break;
        case 7:  Serial.println("繁星点点"); break;
        case 8:  Serial.println("全屏色彩"); break;
        case 9:  Serial.println("乱序雨滴"); break;
        case 10: Serial.println("中心波纹"); break;
        case 11: Serial.println("笑脸盈盈"); break;
        case 12: Serial.println("垂头丧气"); break;
        case 13: Serial.println("面无表情"); break;
        case 14: Serial.println("瞬息万变"); break;
        case 15: Serial.println("音乐随动模式 (S3 强控中)"); break; // 完美补齐串口随动打印支持
        case 16: Serial.println("AI 对话反馈状态"); break;
        case 19: Serial.println("启动贪吃蛇"); break;
        case 21: Serial.println("启动俄罗斯方块"); break;
        default: Serial.println("指令模式"); break;
    }
}

/**
 * @brief 核心渲染引擎：严格保持原有的 14 种模式动画逻辑
 */
static void runDisplayEffects(int mode) {
    switch (mode) {
        case 1: fill_solid(leds, NUM_LEDS, CRGB::Red); break;
        case 2: fill_solid(leds, NUM_LEDS, CRGB::Blue); break;
        case 3: fill_solid(leds, NUM_LEDS, CRGB::Green); break;
        case 4: fill_rainbow(leds, NUM_LEDS, gHue, 7); break;
        case 5: { // 呼吸极光
            uint8_t br = beatsin8(15, 10, 60); 
            FastLED.setBrightness(br); fill_rainbow(leds, NUM_LEDS, gHue, 4); 
        } break;
        case 6: { // 流星划过
            fadeToBlackBy(leds, NUM_LEDS, 40);
            drawPixel(beatsin8(13, 0, 15), beatsin8(8, 0, 15), CHSV(gHue, 200, 255));
        } break;
        case 7: { // 繁星点点
            fadeToBlackBy(leds, NUM_LEDS, 15);
            if(random8() < 25) drawPixel(random(16), random(16), CRGB::White);
        } break;
        case 8: fill_solid(leds, NUM_LEDS, CHSV(gHue, 255, 200)); break;
        case 9: { // 乱序雨滴
            fadeToBlackBy(leds, NUM_LEDS, 80);
            for(int i=0; i<3; i++) drawPixel(random(16), random(16), CHSV(gHue+random(32), 200, 255));
        } break;
        case 10: { // 中心波纹
            float r = (float)(millis() % 1500) / 100.0f;
            FastLED.clear();
            for(int y=0; y<16; y++) for(int x=0; x<16; x++) {
                float d = sqrt(pow(x-7.5, 2) + pow(y-7.5, 2));
                if(abs(d - r) < 1.0) drawPixel(x, y, CHSV(gHue, 255, 255));
            }
        } break;
        case 11: drawFace(CRGB::Yellow, 1); break;
        case 12: drawFace(CRGB::Blue, 2); break;
        case 13: drawFace(CRGB::Green, 3); break;
        case 14: drawFace(CHSV(gHue, 255, 255), (gHue % 128 > 64) ? 1 : 2); break;
        case 15: /* 音乐模式：由主 loop 的状态机接管主动刷新 */ break;
        case 16: drawFace(CRGB::Yellow, 1); break; // 被 S3 唤醒后的视觉反馈
        default: FastLED.clear(); break;
    }
}

/**
 * @brief 初始化显示系统
 */
inline void initDisplay() {
    // 根据 Config.h 中的引脚和数量初始化 FastLED
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(MAX_POWER_VOLTS, MAX_POWER_MA);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear();
    FastLED.show();
    Serial.println(">>> [显示]: FastLED 矩阵驱动已就绪 (16x16)");
}

#endif