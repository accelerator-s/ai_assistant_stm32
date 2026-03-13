/**
 * @file  debug_uart.c
 * @brief 调试串口实现（USART1 PA9=TX, PA10=RX）
 *        波特率 115200，用于向 PC 输出 AT 指令调试日志
 */
#include "debug/debug_uart.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* USART1 句柄 */
static UART_HandleTypeDef huart_debug;

/**
 * @brief 初始化调试串口（USART1, 115200, 8N1）
 */
void debug_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 使能时钟 */
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA9 = USART1_TX, 复用推挽 */
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PA10 = USART1_RX, 浮空输入 */
    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* USART1 参数配置 */
    huart_debug.Instance = USART1;
    huart_debug.Init.BaudRate = 115200;
    huart_debug.Init.WordLength = UART_WORDLENGTH_8B;
    huart_debug.Init.StopBits = UART_STOPBITS_1;
    huart_debug.Init.Parity = UART_PARITY_NONE;
    huart_debug.Init.Mode = UART_MODE_TX;
    huart_debug.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart_debug.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart_debug);
}

/**
 * @brief 格式化输出调试信息到 USART1
 */
void debug_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
    {
        if (len > (int)sizeof(buf) - 1)
            len = sizeof(buf) - 1;
        HAL_UART_Transmit(&huart_debug, (uint8_t *)buf, (uint16_t)len, 200);
    }
}
