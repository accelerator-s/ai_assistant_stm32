/**
 * @file  debug_uart.h
 * @brief 调试串口模块（USART1 PA9/PA10）
 *        通过板载 CH340 USB 转串口输出调试信息到 PC
 */
#ifndef __DEBUG_UART_H
#define __DEBUG_UART_H

#include "main.h"

/* 初始化 USART1 (115200 8N1) */
void debug_uart_init(void);

/* 格式化输出调试信息 */
void debug_printf(const char *fmt, ...);

#endif /* __DEBUG_UART_H */
