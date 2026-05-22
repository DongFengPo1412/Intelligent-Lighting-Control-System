#ifndef GAME_ENGINE_H
#define GAME_ENGINE_H

#include <FastLED.h>
#include "DisplayManager.h"

// --- 1. 外部变量声明 (实际定义在主 .ino 文件中) ---
extern int currentMode;

// 贪吃蛇状态
extern int snakeX[256], snakeY[256], snakeLen;
extern int sDx, sDy;
extern int foodX, foodY;
extern int snakeScore;
extern int snakeSpeed;
extern unsigned long lastSnakeUpdate;
extern bool snakeStarted;

// 俄罗斯方块状态
extern uint8_t tetrisField[16][16]; 
extern int pType, pRot, pX, pY;
extern int tetrisScore;
extern int tetrisSpeed;
extern unsigned long lastTetUpdate;
extern bool tetrisStarted;

// 游戏边界常量
const int BOARD_LEFT = 3;  
const int BOARD_RIGHT = 12; 

// --- 2. 静态数据 (保存在 Flash 中以节省 WROOM-32 的 RAM) ---
static const int8_t shapes[7][4][4][2] PROGMEM = {
  {{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}}}, // O
  {{{0,1},{1,1},{2,1},{3,1}},{{2,0},{2,1},{2,2},{2,3}},{{0,2},{1,2},{2,2},{3,2}},{{1,0},{1,1},{1,2},{1,3}}}, // I
  {{{1,0},{0,1},{1,1},{2,1}},{{1,0},{1,1},{2,1},{1,2}},{{0,1},{1,1},{2,1},{1,2}},{{1,0},{0,1},{1,1},{1,2}}}, // T
  {{{0,1},{1,1},{2,1},{2,0}},{{1,0},{1,1},{1,2},{2,2}},{{0,2},{0,1},{1,1},{2,1}},{{0,0},{1,0},{1,1},{1,2}}}, // L
  {{{0,0},{0,1},{1,1},{2,1}},{{1,2},{1,1},{1,0},{2,0}},{{0,1},{1,1},{2,1},{2,2}},{{0,2},{1,2},{1,1},{1,0}}}, // J
  {{{1,0},{2,0},{0,1},{1,1}},{{1,0},{1,1},{2,1},{2,2}},{{1,1},{2,1},{0,2},{1,2}},{{0,0},{0,1},{1,1},{1,2}}}, // S
  {{{0,0},{1,0},{1,1},{2,1}},{{2,0},{2,1},{1,1},{1,2}},{{0,1},{1,1},{1,2},{2,2}},{{1,0},{1,1},{0,1},{0,2}}}  // Z
};

// --- 3. 游戏通用工具 ---
inline void drawGameScore(int s) {
    FastLED.clear();
    drawDigit(s / 10, 4, 5, CRGB::White);
    drawDigit(s % 10, 9, 5, CRGB::White);
}

// --- 4. 贪吃蛇核心逻辑 ---
inline void initSnake() {
    snakeLen = 3; snakeScore = 0; sDx = 1; sDy = 0; snakeSpeed = 450;
    for(int i=0; i<3; i++) { snakeX[i] = 5-i; snakeY[i] = 7; }
    foodX = random(16); foodY = random(16);
    lastSnakeUpdate = millis(); 
    snakeStarted = true;
}

inline void runSnake() {
    if(!snakeStarted) return;
    if(millis() - lastSnakeUpdate > (unsigned long)snakeSpeed) {
        lastSnakeUpdate = millis();
        for(int i = snakeLen - 1; i > 0; i--) { snakeX[i] = snakeX[i-1]; snakeY[i] = snakeY[i-1]; }
        snakeX[0] = (snakeX[0] + sDx + 16) % 16;
        snakeY[0] = (snakeY[0] + sDy + 16) % 16;
        
        for(int i = 1; i < snakeLen; i++) {
            if(snakeX[0] == snakeX[i] && snakeY[0] == snakeY[i]) {
                snakeStarted = false; 
                currentMode = 20; // 切换到贪吃蛇结算模式
                return;
            }
        }
        
        if(snakeX[0] == foodX && snakeY[0] == foodY) {
            snakeScore++;
            if (snakeLen < 256) {
                snakeLen++;
            }
            snakeSpeed = max(80, 450 - (snakeScore * 15)); 
            foodX = random(16); foodY = random(16);
        }
    }
    FastLED.clear();
    drawPixel(foodX, foodY, CRGB::Red);
    for(int i=0; i<snakeLen; i++) drawPixel(snakeX[i], snakeY[i], (i==0)?CRGB::Lime:CRGB::Green);
}

// --- 5. 俄罗斯方块核心逻辑 ---
inline bool checkTetCol(int t, int r, int x, int y) {
    for(int i=0; i<4; i++){
        int px = x + pgm_read_byte(&shapes[t][r][i][0]);
        int py = y + pgm_read_byte(&shapes[t][r][i][1]);
        if(px < BOARD_LEFT || px > BOARD_RIGHT || py >= 16 || (py >= 0 && tetrisField[py][px])) return true;
    }
    return false;
}

inline void spawnTetrisPiece() {
    pType = random(7); pRot = 0; pX = BOARD_LEFT + 3; pY = 0;
    if(checkTetCol(pType, pRot, pX, pY)) {
        tetrisStarted = false; 
        currentMode = 22; // 切换到俄罗斯方块结算模式
    }
}

inline void initTetris() {
    memset(tetrisField, 0, sizeof(tetrisField));
    tetrisScore = 0; tetrisSpeed = 800;
    lastTetUpdate = millis(); 
    tetrisStarted = true;
    spawnTetrisPiece();
}

inline CRGB getTetColor(int type) {
    static const CRGB colors[] = {CRGB::Cyan, CRGB::Blue, CRGB::Orange, CRGB::Yellow, CRGB::Green, CRGB::Purple, CRGB::Red};
    return colors[type % 7];
}

inline void runTetris() {
    if(!tetrisStarted) return;
    if(millis() - lastTetUpdate > (unsigned long)tetrisSpeed) {
        lastTetUpdate = millis();
        if(!checkTetCol(pType, pRot, pX, pY + 1)) pY++;
        else {
            for(int i=0; i<4; i++) {
                int px = pX + pgm_read_byte(&shapes[pType][pRot][i][0]);
                int py = pY + pgm_read_byte(&shapes[pType][pRot][i][1]);
                if(py >= 0) tetrisField[py][px] = pType + 1;
            }
            for(int y = 15; y >= 0; y--) {
                bool full = true;
                for(int x = BOARD_LEFT; x <= BOARD_RIGHT; x++) if(!tetrisField[y][x]) full = false;
                if(full) {
                    tetrisScore++;
                    tetrisSpeed = max(100, tetrisSpeed - 30);
                    for(int ty = y; ty > 0; ty--) 
                        for(int tx = BOARD_LEFT; tx <= BOARD_RIGHT; tx++) tetrisField[ty][tx] = tetrisField[ty-1][tx];
                    for(int tx = BOARD_LEFT; tx <= BOARD_RIGHT; tx++) tetrisField[0][tx] = 0;
                    y++; 
                }
            }
            spawnTetrisPiece();
        }
    }
    FastLED.clear();
    for(int y=0; y<16; y++) { drawPixel(BOARD_LEFT-1, y, CRGB::Gray); drawPixel(BOARD_RIGHT+1, y, CRGB::Gray); }
    for(int y=0; y<16; y++) 
        for(int x=BOARD_LEFT; x<=BOARD_RIGHT; x++) 
            if(tetrisField[y][x]) drawPixel(x, y, getTetColor(tetrisField[y][x]-1));
    for(int i=0; i<4; i++) 
        drawPixel(pX + pgm_read_byte(&shapes[pType][pRot][i][0]), pY + pgm_read_byte(&shapes[pType][pRot][i][1]), getTetColor(pType));
}

#endif