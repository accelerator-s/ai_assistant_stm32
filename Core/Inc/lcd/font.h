/**
 * @file  font.h
 * @brief 8x16 ASCII 点阵字库定义
 */
#ifndef __FONT_H
#define __FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16
#define FONT_FIRST_CHAR 0x20
#define FONT_LAST_CHAR 0x7E

extern const uint8_t g_font_8x16[][FONT_H];

#endif /* __FONT_H */
