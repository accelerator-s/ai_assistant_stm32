/**
 * @file  display.h
 * @brief LCD 界面显示模块
 */
#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "main.h"
#include "lcd/lcd.h"
#include <stdint.h>

/* 初始化 LCD 并绘制静态界面 */
void display_init(void);

/* 刷新运行秒数 */
void display_update_uptime(uint32_t seconds);

/* 刷新 WiFi 状态文本（文字 + 颜色） */
void display_update_wifi(const char *text, uint16_t color);

/* 刷新 IP 地址显示 */
void display_update_ip(const char *ip);

/* 刷新 K1 按键状态 (1=按下, 0=松开) */
void display_update_key1(uint8_t pressed);

/* 刷新 K2 按键状态 (1=按下, 0=松开) */
void display_update_key2(uint8_t pressed);

/* 刷新调试信息文本 */
void display_update_debug(const char *text);

#endif /* __DISPLAY_H */
