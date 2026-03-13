/**
 * @file  display.c
 * @brief LCD 界面显示模块 — 全中文界面
 */
#include "lcd/display.h"
#include "lcd/lcd.h"
#include <stdio.h>

/* 绘制标题栏 */
static void draw_title_bar(const char *title)
{
    lcd_fill_rect(0, 0, LCD_WIDTH, 24, COLOR_BLUE);
    lcd_draw_string_cn(4, 4, title, COLOR_WHITE, COLOR_BLUE);
}

/* 绘制水平分割线 */
static void draw_divider(uint16_t y, uint16_t color)
{
    lcd_fill_rect(0, y, LCD_WIDTH, 2, color);
}

void display_init(void)
{
    lcd_init();
    lcd_clear(COLOR_BLACK);

    /* 标题栏 */
    draw_title_bar("STM32 语音助手");
    draw_divider(26, COLOR_GRAY);

    /* 板载信息 */
    lcd_draw_string_cn(4, 34, "开发板: 野火 F103VET6", COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string_cn(4, 52, "处理器: 72 MHz", COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string_cn(4, 70, "闪存: 512KB 内存: 64KB", COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string_cn(4, 88, "液晶: ILI9341 240x320", COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string_cn(4, 106, "总线: FSMC 16位", COLOR_GREEN, COLOR_BLACK);

    /* 运行时间 / WiFi 状态 / IP */
    draw_divider(124, COLOR_GRAY);
    lcd_draw_string_cn(4, 132, "运行时间(秒):", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_string_cn(4, 152, "WiFi状态:", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_string_cn(4, 172, "IP地址:", COLOR_WHITE, COLOR_BLACK);

    /* 按键状态 */
    draw_divider(194, COLOR_GRAY);
    lcd_draw_string_cn(4, 202, "按键1:", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_string_cn(4, 222, "按键2:", COLOR_WHITE, COLOR_BLACK);

    /* 调试信息区域 */
    draw_divider(242, COLOR_GRAY);
    lcd_draw_string_cn(4, 250, "调试:", COLOR_MAGENTA, COLOR_BLACK);
}

void display_update_uptime(uint32_t seconds)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)seconds);
    lcd_draw_string(4 + 13 * 8, 132, buf, COLOR_ORANGE, COLOR_BLACK);
}

void display_update_wifi(const char *text, uint16_t color)
{
    /* 先清除旧内容再写入新文字 */
    lcd_fill_rect(76, 152, LCD_WIDTH - 76, 16, COLOR_BLACK);
    lcd_draw_string_cn(76, 152, text, color, COLOR_BLACK);
}

void display_update_ip(const char *ip)
{
    lcd_fill_rect(56, 172, LCD_WIDTH - 56, 16, COLOR_BLACK);
    if (ip && ip[0] != '\0')
        lcd_draw_string(56, 172, ip, COLOR_CYAN, COLOR_BLACK);
    else
        lcd_draw_string(56, 172, "--", COLOR_GRAY, COLOR_BLACK);
}

void display_update_key1(uint8_t pressed)
{
    if (pressed)
        lcd_draw_string_cn(56, 202, "按下  ", COLOR_GREEN, COLOR_BLACK);
    else
        lcd_draw_string_cn(56, 202, "松开  ", COLOR_RED, COLOR_BLACK);
}

void display_update_key2(uint8_t pressed)
{
    if (pressed)
        lcd_draw_string_cn(56, 222, "按下  ", COLOR_GREEN, COLOR_BLACK);
    else
        lcd_draw_string_cn(56, 222, "松开  ", COLOR_RED, COLOR_BLACK);
}

void display_update_debug(const char *text)
{
    lcd_fill_rect(0, 250, LCD_WIDTH, 16, COLOR_BLACK);
    if (text && text[0] != '\0')
        lcd_draw_string(4, 250, text, COLOR_MAGENTA, COLOR_BLACK);
}
