/**
 * @file  display.h
 * @brief LCD 界面显示模块
 */
#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "main.h"
#include <stdint.h>

/* 初始化 LCD 并绘制静态界面 */
void display_init(void);

/* 刷新计数器数值 */
void display_update_counter(uint32_t count);

/* 刷新运行秒数 */
void display_update_uptime(uint32_t seconds);

/* 刷新 K1 按键状态 (1=按下, 0=松开) */
void display_update_key1(uint8_t pressed);

/* 刷新 K2 按键状态 (1=按下, 0=松开) */
void display_update_key2(uint8_t pressed);

#endif /* __DISPLAY_H */
