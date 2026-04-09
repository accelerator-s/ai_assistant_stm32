/**
 * @file  stm32f1xx_it.c
 * @brief 中断服务函数
 */

#include "main.h"
#include "stm32f1xx_it.h"
#include "wifi/esp8266.h"
#include "audio/i2s_mic.h"

/******************************************************************************/
/*           Cortex-M3 异常处理                                                */
/******************************************************************************/

void NMI_Handler(void)
{
  while (1)
  {
  }
}

void HardFault_Handler(void)
{
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

/******************************************************************************/
/*           外设中断处理                                                      */
/******************************************************************************/

/* K1 — PA0 */
void EXTI0_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}

/* K2 — PC13 */
void EXTI15_10_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13);
}

/* ESP8266 — USART3 */
void USART3_IRQHandler(void)
{
  esp8266_uart_irq_handler();
}

/* I2S2 麦克风 DMA 接收 — DMA1 通道4 */
void DMA1_Channel4_IRQHandler(void)
{
  HAL_DMA_IRQHandler(i2s_mic_get_dma_handle());
}

/* SPI2 全局中断（I2S2 共用） */
void SPI2_IRQHandler(void)
{
  HAL_I2S_IRQHandler(i2s_mic_get_handle());
}

