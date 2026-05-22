#ifndef SPECTRUM_MANAGER_H
#define SPECTRUM_MANAGER_H

#include <Arduino.h>
#include <FastLED.h>

// 宏观整个复合大矩阵的尺寸
#define MATRIX_WIDTH  16
#define MATRIX_HEIGHT 16
#define NUM_LEDS      256

class SpectrumManager {
private:
    CRGB* _leds;
    int _bands[16];
    float _fall_bands[16]; // 用于频谱平滑下落的重力滤波器
    int _is_beat;

    // 精准适配 4 块 8x8 纯直连拼接布局的物理索引解算器
    int getMatrixIndex(int x, int y) {
        // 安全边界保护：防止非正常解算导致内存越界崩溃
        if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) return 0;

        int local_x = 0;    // 子板内的相对 X (0~7)
        int local_y = 0;    // 子板内的相对 Y (0~7)
        int base_index = 0; // 子板在总一维链条中的起始偏移量

        // 1. 判定纵向区间（上半部分还是下半部分）
        if (y < 8) {
            // 上半部分：可能是面板 1 或 面板 2
            local_y = y; 
            if (x < 8) {
                local_x = x;
                base_index = 0;      // 面板 1 拥有前 64 颗灯 (0~63)
            } else {
                local_x = x - 8;
                base_index = 64;     // 面板 2 拥有次 64 颗灯 (64~127)
            }
        } else {
            // 下半部分：可能是面板 3 或 面板 4
            local_y = y - 8; 
            if (x < 8) {
                local_x = x;
                base_index = 128;    // 面板 3 拥有第三批 64 颗灯 (128~191)
            } else {
                local_x = x - 8;
                base_index = 192;    // 面板 4 拥有最后一批 64 颗灯 (192~255)
            }
        }

        // 2. 在具体的 8x8 子面板内部，遵循纯直连走线逻辑（不分奇偶行，统一从左往右）
        int local_index = local_y * 8 + local_x;

        // 3. 最终串联总索引 = 子板起始偏移 + 子板内部纯直连局部索引
        return base_index + local_index;
    }

public:
    SpectrumManager(CRGB* leds_ptr) {
        _leds = leds_ptr;
        _is_beat = 0;
        for (int i = 0; i < 16; i++) {
            _bands[i] = 0;
            _fall_bands[i] = 0.0f;
        }
    }

    // 核心流解析状态机：揉碎主板发来的 "f,beat,b0,b1..." 裸字符串
    void parseSerialData(String cmd) {
        if (!cmd.startsWith("f,")) return;
        cmd = cmd.substring(2); // 剥离 "f," 协议头，留下 "beat,b0,b1..."

        int index = 0;
        int pos = 0;
        
        // 循环 17 次切片（1个beat + 16个频带）
        while ((pos = cmd.indexOf(',')) != -1 && index < 17) {
            int val = cmd.substring(0, pos).toInt();
            if (index == 0) {
                _is_beat = val;
            } else {
                _bands[index - 1] = val;
            }
            cmd = cmd.substring(pos + 1);
            index++;
        }
        if (index == 17) {
            _bands[15] = cmd.toInt(); 
        }
        // 🌟【最左列低频实时补偿】：如果最左边一列因为主板直流分量过滤导致死区
        // 我们让它动态影子克隆第 1 列的次低音震荡，同时在鼓点爆发(beat==1)时强行让它满格冲顶！
        if (_bands[0] <= 1) {
            _bands[0] = _bands[1] + 1; 
        }
        if (_is_beat == 1) {
            _bands[0] = 15; // 鼓点来临，第 0 列作为低音重炮必须全满爆发
        }
        // 驱动渲染引擎刷新画面
        renderSpectrum();
    }

    // 像素阵列图形渲染引擎
    void renderSpectrum() {
        // 1. 整体渐暗：保留轻微拖尾余晖，让频谱舞动更有质感
        fadeToBlackBy(_leds, NUM_LEDS, 160); 

        // 2. 遍历横向 16 列大音轨
        for (int x = 0; x < MATRIX_WIDTH; x++) {
            // 🌟🌟🌟【黄金调校：重力加速度衰减，彻底绝杀半空死锁】🌟🌟🌟
            if ((float)_bands[x] >= _fall_bands[x]) {
                _fall_bands[x] = (float)_bands[x]; // 上升瞬间响应
            } else {
                // 采用“固定步长 + 比例衰减”的工业标准公式
                // 这样当频谱在高位（比如15）时下落平滑优雅，跌到低位（低于3）时会加速坠落，瞬间砸向地面
                _fall_bands[x] -= (0.4f + _fall_bands[x] * 0.05f); 
                
                // 如果当前下落的重力线已经跌到了新输入信号的底噪区间（例如小于1.2），且新信号很弱，直接强行归零
                if (_fall_bands[x] < (float)_bands[x] + 0.2f && _bands[x] <= 2) {
                    _fall_bands[x] = (float)_bands[x];
                }
                
                if (_fall_bands[x] < 0.0f) _fall_bands[x] = 0.0f;
            }

            int current_height = (int)_fall_bands[x];

            // 3. 开始纵向绘制每一列的像素
            for (int y = 0; y <= current_height; y++) {
                // 🌟【核心世界线修正】：利用 15 - y 倒算物理硬件索引！
                // 使得数据从最底部的 3、4号面板（y=0时映射到硬件第15行）开始向上蔓延成长
                int target_physical_y = 15 - y;
                // 严密越界锁死：确保翻转后的物理 Y 轴安全落在 0~15 空间内
                if (target_physical_y < 0) target_physical_y = 0;
                // 颜色渐变逻辑同步对齐：让暖色（红橙）留在底部 3、4号板，冷色（蓝紫）冲向顶部 1、2号板
                CHSV pixel_color = CHSV(y * 14 + 140, 245, 255);

                // 传入翻转后的物理 Y 坐标，计算出正确的串联物理 LED 编号
                int led_idx = getMatrixIndex(x, target_physical_y);
                _leds[led_idx] = pixel_color;
            }
        }

        // 🌟【Beat 瞬态大鼓氛围】：若抓到 beat=1，全屏叠加轻微的冷色微光氛围
        if (_is_beat == 1) {
            for (int i = 0; i < NUM_LEDS; i++) {
                _leds[i] += CRGB(0, 25, 25); 
            }
        }

        // 🌟【硬件级动态功耗上限拦截盾】：锁定5V，最大电流1.2A，彻底断绝瞬态大电流导致复位或烧毁的风险
        FastLED.setMaxPowerInVoltsAndMilliamps(5, 1200); 

        // 瞬间推向物理引脚发光！
        FastLED.show();
    }
};

#endif