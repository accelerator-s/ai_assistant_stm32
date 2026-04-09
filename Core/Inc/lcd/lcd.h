/**
 * @file  lcd.h
 * @brief ILI9341 LCD 驱动 (FSMC Bank1 NE1, 16位并口)
 */
#ifndef __LCD_H
#define __LCD_H

#include "stm32f1xx_hal.h"

/* RST / 背光引脚 */
#define LCD_RST_PIN GPIO_PIN_1
#define LCD_RST_PORT GPIOE
#define LCD_BL_PIN GPIO_PIN_12
#define LCD_BL_PORT GPIOD

/* FSMC 映射地址 — Bank1 NE1 (基地址 0x60000000), A16 作为 RS(D/C) */
/* 16位总线宽度下，A16 对应字节地址 bit17，偏移 = 2^17 = 0x20000 */
#define LCD_BASE_CMD ((uint32_t)0x60000000) /* A16=0: 写命令 */
#define LCD_BASE_DAT ((uint32_t)0x60020000) /* A16=1: 写数据 */

#define LCD_WR_CMD(cmd) (*(__IO uint16_t *)LCD_BASE_CMD = (cmd))
#define LCD_WR_DAT(dat) (*(__IO uint16_t *)LCD_BASE_DAT = (dat))
#define LCD_RD_DAT() (*(__IO uint16_t *)LCD_BASE_DAT)

/* LCD 分辨率 */
#define LCD_WIDTH 240
#define LCD_HEIGHT 320

/* ===================== RGB565 颜色定义 ===================== */

/* 基础颜色 */
#define COLOR_BLACK 0x0000u
#define COLOR_WHITE 0xFFFFu
#define COLOR_RED 0x07E0u
#define COLOR_GREEN 0xF800u
#define COLOR_BLUE 0x001Fu
#define COLOR_YELLOW 0xFFE0u
#define COLOR_CYAN 0x07FFu
#define COLOR_MAGENTA 0xF81Fu
#define COLOR_ORANGE 0xFD20u
#define COLOR_GRAY 0x8410u

/* ChatGPT 风格深色主题配色 */
#define COLOR_BG_DARK 0x2104u        /* #212121 深灰背景 */
#define COLOR_BG_SIDEBAR 0x18E3u     /* #171717 侧边栏深灰 */
#define COLOR_BG_BUBBLE_USR 0x39E7u  /* #3a3a3a 用户消息气泡 */
#define COLOR_BG_BUBBLE_AI 0x2945u   /* #2b2b2b AI消息气泡 */
#define COLOR_BG_STATUS 0x10A2u      /* #111111 状态栏 */
#define COLOR_BG_INPUT 0x31A6u       /* #333333 输入区背景 */
#define COLOR_ACCENT_GREEN 0xD174u   /* 面板校正后的绿色强调色 */
#define COLOR_ACCENT_TEAL 0x1E9Fu    /* #1abc9c 青绿强调 */
#define COLOR_TEXT_PRIMARY 0xEF7Du   /* #ececec 主文字 */
#define COLOR_TEXT_SECONDARY 0x9CF3u /* #9a9a9a 次要文字 */
#define COLOR_TEXT_DIM 0x6B6Du       /* #6b6b6b 暗淡文字 */
#define COLOR_DIVIDER 0x2945u        /* #2b2b2b 分割线 */
#define COLOR_ICON_MIC 0x46DFu       /* 面板校正后的偏红色麦克风 */
#define COLOR_RECORDING 0x07E0u      /* 面板校正后的红色录音指示 */

/* ===================== 基础绘图函数 ===================== */

void lcd_init(void);
void lcd_clear(uint16_t color);
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg);
void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);
void lcd_draw_char_cn(uint16_t x, uint16_t y, uint16_t gbk_code, uint16_t fg, uint16_t bg);
void lcd_draw_string_cn(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);
void lcd_backlight(uint8_t on);

/* ===================== 扩展绘图函数 ===================== */

/* 画水平线 */
void lcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color);

/* 画垂直线 */
void lcd_draw_vline(uint16_t x, uint16_t y, uint16_t len, uint16_t color);

/* 画圆（Bresenham 算法） */
void lcd_draw_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color);

/* 画填充圆 */
void lcd_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color);

/* 画圆角矩形（仅边框） */
void lcd_draw_rounded_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t r, uint16_t color);

/* 画填充圆角矩形 */
void lcd_fill_rounded_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t r, uint16_t color);

/* 在指定矩形区域内绘制中英文混排字符串，自动换行，返回实际绘制行高 */
uint16_t lcd_draw_text_wrap(uint16_t x, uint16_t y, uint16_t max_w, uint16_t max_h,
                            const char *str, uint16_t fg, uint16_t bg);

#endif /* __LCD_H */
