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

/* RGB565 常用颜色 */
#define COLOR_BLACK 0x0000u
#define COLOR_WHITE 0xFFFFu
#define COLOR_RED 0xF800u
#define COLOR_GREEN 0x07E0u
#define COLOR_BLUE 0x001Fu
#define COLOR_YELLOW 0xFFE0u
#define COLOR_CYAN 0x07FFu
#define COLOR_MAGENTA 0xF81Fu
#define COLOR_ORANGE 0xFD20u
#define COLOR_GRAY 0x8410u

/* 函数声明 */
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

#endif /* __LCD_H */
