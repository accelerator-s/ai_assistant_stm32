/**
 * @file  stm32f1xx_hal_msp.c
 * @brief HAL MSP 初始化
 */
#include "main.h"

/* 全局 MSP 初始化 */
void HAL_MspInit(void)
{
  __HAL_RCC_AFIO_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  /* 禁用 JTAG，保留 SWD */
  __HAL_AFIO_REMAP_SWJ_NOJTAG();
}
