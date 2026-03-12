/**
 * @file  bsp_key.h
 * @brief K1/K2 按键 EXTI 驱动
 *        K1 -> PA0 (按下高电平)  K2 -> PC13 (按下低电平)
 */
#ifndef __BSP_KEY_H
#define __BSP_KEY_H

#include "main.h"

/* 引脚定义 */
#define KEY1_GPIO_PORT GPIOA
#define KEY1_GPIO_PIN GPIO_PIN_0
#define KEY1_ACTIVE_LVL GPIO_PIN_SET

#define KEY2_GPIO_PORT GPIOC
#define KEY2_GPIO_PIN GPIO_PIN_13
#define KEY2_ACTIVE_LVL GPIO_PIN_SET

/* 状态值 */
#define KEY_RELEASED 0
#define KEY_PRESSED 1

/* 接口函数 */
void bsp_key_init(void);
uint8_t bsp_key_get_k1(void);
uint8_t bsp_key_get_k2(void);
uint8_t bsp_key_k1_changed(void);
uint8_t bsp_key_k2_changed(void);

#endif /* __BSP_KEY_H */
