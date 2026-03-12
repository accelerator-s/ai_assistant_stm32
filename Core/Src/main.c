/* USER CODE BEGIN Header */
/**
 * @file  main.c
 * @brief 主程序入口
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "bsp/bsp_key.h"
#include "lcd/display.h"

/* Private function prototypes -----------------------------------------------*/
/* 配置系统时钟 72 MHz */
void SystemClock_Config(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  /* 外设初始化 */
  bsp_key_init();
  display_init();

  /* 首次显示按键状态 */
  display_update_key1(bsp_key_get_k1());
  display_update_key2(bsp_key_get_k2());

  /* 主循环 */
  uint32_t counter = 0;

  while (1)
  {
    /* 刷新计数器和运行时间 */
    display_update_counter(counter);
    display_update_uptime(HAL_GetTick() / 1000);

    /* 按键变化时刷新显示 */
    if (bsp_key_k1_changed())
      display_update_key1(bsp_key_get_k1());
    if (bsp_key_k2_changed())
      display_update_key2(bsp_key_get_k2());

    counter++;
    HAL_Delay(200);
  }
}

/* 系统时钟配置: HSE 8MHz -> PLL x9 -> 72MHz */
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

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* 错误处理: LED 闪烁指示 */
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
      ;
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
}
#endif
