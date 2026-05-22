#ifndef BLINKER_MANAGER_H
#define BLINKER_MANAGER_H

#include <Blinker.h>
#include "Config.h"
#include "GameEngine.h"

// --- 1. 外部变量声明 ---
// 引用主程序中的全局变量，确保 App 操作能实时改变灯板状态
extern int currentMode;
extern void changeToMode(int m); 

// 🌟🌟🌟【精准对齐修复】：显示声明数组固定边界 256，彻底绝杀链接器不完整类型报错
extern int snakeX[256], snakeY[256], snakeLen;
extern int sDx, sDy;
extern int pType, pRot, pX, pY;
extern bool snakeStarted;
extern bool tetrisStarted;

// --- 2. 声明 App 按钮组件 ---
extern BlinkerButton BtnW; // 上 / 旋转
extern BlinkerButton BtnS; // 下 / 加速
extern BlinkerButton BtnA; // 左
extern BlinkerButton BtnD; // 右

extern BlinkerButton BtnE; // 快捷键：启动贪吃蛇
extern BlinkerButton BtnF; // 快捷键：启动俄罗斯方块

// --- 3. 按键回调函数 (使用 inline 防止多重定义链接报错) ---

/**
 * @brief W 键回调：贪吃蛇向上，俄罗斯方块旋转
 */
inline void buttonW_callback(const String & s) {
    if (currentMode == 19 && snakeStarted) { 
        if (sDy != 1) { sDx = 0; sDy = -1; } 
    } 
    else if (currentMode == 21 && tetrisStarted) { 
        int nR = (pRot + 1) % 4;
        if (!checkTetCol(pType, nR, pX, pY)) pRot = nR; 
    }
}

/**
 * @brief S 键回调：贪吃蛇向下，俄罗斯方块下沉
 */
inline void buttonS_callback(const String & s) {
    if (currentMode == 19 && snakeStarted) { 
        if (sDy != -1) { sDx = 0; sDy = 1; }  
    } 
    else if (currentMode == 21 && tetrisStarted) { 
        if (!checkTetCol(pType, pRot, pX, pY + 1)) pY++; 
    }
}

/**
 * @brief A 键回调：贪吃蛇向左，俄罗斯方块向左
 */
inline void buttonA_callback(const String & s) {
    if (currentMode == 19 && snakeStarted) { 
        if (sDx != 1) { sDx = -1; sDy = 0; } 
    } 
    else if (currentMode == 21 && tetrisStarted) { 
        if (!checkTetCol(pType, pRot, pX - 1, pY)) pX--; 
    }
}

/**
 * @brief D 键回调：贪吃蛇向右，俄罗斯方块向右
 */
inline void buttonD_callback(const String & s) {
    if (currentMode == 19 && snakeStarted) { 
        if (sDx != -1) { sDx = 1; sDy = 0; }  
    } 
    else if (currentMode == 21 && tetrisStarted) { 
        if (!checkTetCol(pType, pRot, pX + 1, pY)) pX++; 
    }
}

/**
 * @brief E 键回调：切换到贪吃蛇模式（带音乐随动硬锁保护）
 */
inline void buttonE_callback(const String & s) {
    // 🌟 音乐随动中，强制封锁手机端切模误触，死守大动脉数据流稳定
    if (currentMode == 15) {
        Serial.println(">>> [Blinker]: 音乐随动中，已拦截 App 切模操作");
        return; 
    }
    Serial.println(">>> [Blinker]: App 启动贪吃蛇");
    changeToMode(19); 
}

/**
 * @brief F 键回调：切换到俄罗斯方块模式（带音乐随动硬锁保护）
 */
inline void buttonF_callback(const String & s) {
    if (currentMode == 15) {
        Serial.println(">>> [Blinker]: 音乐随动中，已拦截 App 切模操作");
        return; 
    }
    Serial.println(">>> [Blinker]: App 启动俄罗斯方块");
    changeToMode(21); 
}

// --- 4. 初始化函数 ---

/**
 * @brief 初始化 Blinker 交互映射
 */
inline void initBlinker() {
    // 使用 Config.h 中的凭证初始化
    Blinker.begin(auth, ssid, pswd); 
    
    // 绑定游戏控制按键
    BtnW.attach(buttonW_callback);
    BtnS.attach(buttonS_callback);
    BtnA.attach(buttonA_callback);
    BtnD.attach(buttonD_callback);

    // 绑定模式快捷按键
    BtnE.attach(buttonE_callback);
    BtnF.attach(buttonF_callback);
    
    Serial.println(">>> [系统]: Blinker IoT 接口初始化成功");
}

#endif