/* USER CODE BEGIN Header */
/**
 * @file  main.c
 * @brief 主程序入口 — 全异步、无阻塞主循环版本
 *
 * 系统状态:
 *   STATE_IDLE       — 待机状态，等待用户操作
 *   STATE_RECORDING  — 录音中（K1 按住）
 *   STATE_REC_PAUSED — 录音暂停（K1 松开，可继续录或发送）
 *   STATE_WAITING    — 等待 AI 回复
 *   STATE_HISTORY    — 历史记录列表浏览
 *
 * 设计原则:
 *   1. 主循环不使用任何阻塞式网络发送
 *   2. 音频上传仅通过异步 TCP 发送队列推进
 *   3. 麦克风探测改为非阻塞状态机，不再直接同步采样等待
 *   4. 所有控制命令均通过异步文本发送接口入队
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "button/button_app.h"

void SystemClock_Config(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    button_app_init();
    button_app_run();

    while (1)
    {
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}


void Error_Handler(void)
{
    GPIO_InitTypeDef err_gpio = {0};
    err_gpio.Pin = GPIO_PIN_12;
    err_gpio.Mode = GPIO_MODE_OUTPUT_PP;
    err_gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &err_gpio);

    while (1)
    {
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12);
        for (volatile uint32_t i = 0; i < 500000; i++)
        {
        }
    }
}


#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
