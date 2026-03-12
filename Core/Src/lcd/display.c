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
    draw_title_bar("STM32F103 演示系统");
    draw_divider(26, COLOR_GRAY);

    /* 板载信息 */
    lcd_draw_string_cn(4, 34, "开发板: 野火 F103VET6", COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string_cn(4, 52, "处理器: 72 MHz", COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string_cn(4, 70, "闪存: 512KB 内存: 64KB", COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string_cn(4, 88, "液晶: ILI9341 240x320", COLOR_GREEN, COLOR_BLACK);
    lcd_draw_string_cn(4, 106, "总线: FSMC 16位", COLOR_GREEN, COLOR_BLACK);

    /* 计数器 / 运行时间 */
    draw_divider(124, COLOR_GRAY);
    lcd_draw_string_cn(4, 130, "计数器:", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_string_cn(4, 160, "运行时间(秒):", COLOR_WHITE, COLOR_BLACK);

    /* 提示 */
    draw_divider(186, COLOR_GRAY);
    lcd_draw_string_cn(4, 192, "这是 STM32 演示程序!", COLOR_CYAN, COLOR_BLACK);
    lcd_draw_string_cn(4, 210, "正在运行中...", COLOR_MAGENTA, COLOR_BLACK);

    /* 按键状态 */
    draw_divider(230, COLOR_GRAY);
    lcd_draw_string_cn(4, 238, "按键1:", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_string_cn(4, 258, "按键2:", COLOR_WHITE, COLOR_BLACK);
}

void display_update_counter(uint32_t count)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)count);
    lcd_draw_string(76, 130, buf, COLOR_ORANGE, COLOR_BLACK);
}

void display_update_uptime(uint32_t seconds)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%10lu", (unsigned long)seconds);
    lcd_draw_string(4 + 13 * 8, 160, buf, COLOR_ORANGE, COLOR_BLACK);
}

void display_update_key1(uint8_t pressed)
{
    if (pressed)
        lcd_draw_string_cn(56, 238, "按下  ", COLOR_GREEN, COLOR_BLACK);
    else
        lcd_draw_string_cn(56, 238, "松开  ", COLOR_RED, COLOR_BLACK);
}

void display_update_key2(uint8_t pressed)
{
    if (pressed)
        lcd_draw_string_cn(56, 258, "按下  ", COLOR_GREEN, COLOR_BLACK);
    else
        lcd_draw_string_cn(56, 258, "松开  ", COLOR_RED, COLOR_BLACK);
}
