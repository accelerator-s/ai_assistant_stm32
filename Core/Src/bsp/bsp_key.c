/**
 * @file  bsp_key.c
 * @brief K1/K2 按键 EXTI 驱动
 */
#include "bsp/bsp_key.h"

/* 按键状态 */
static volatile uint8_t s_k1_state;
static volatile uint8_t s_k2_state;
static volatile uint8_t s_k1_changed;
static volatile uint8_t s_k2_changed;

/* 初始化 */
void bsp_key_init(void)
{
    /* 时钟 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* K1 — PA0 双沿 EXTI */
    gpio.Pin = KEY1_GPIO_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(KEY1_GPIO_PORT, &gpio);

    /* K2 — PC13 双沿 EXTI */
    gpio.Pin = KEY2_GPIO_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(KEY2_GPIO_PORT, &gpio);

    /* NVIC */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

    /* 读取初始电平 */
    s_k1_state = (HAL_GPIO_ReadPin(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == KEY1_ACTIVE_LVL)
                     ? KEY_PRESSED
                     : KEY_RELEASED;
    s_k2_state = (HAL_GPIO_ReadPin(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == KEY2_ACTIVE_LVL)
                     ? KEY_PRESSED
                     : KEY_RELEASED;
    s_k1_changed = 1;
    s_k2_changed = 1;
}

/* 查询接口 */
uint8_t bsp_key_get_k1(void) { return s_k1_state; }
uint8_t bsp_key_get_k2(void) { return s_k2_state; }

uint8_t bsp_key_k1_changed(void)
{
    if (s_k1_changed)
    {
        s_k1_changed = 0;
        return 1;
    }
    return 0;
}

uint8_t bsp_key_k2_changed(void)
{
    if (s_k2_changed)
    {
        s_k2_changed = 0;
        return 1;
    }
    return 0;
}

/* EXTI 回调 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == KEY1_GPIO_PIN)
    {
        s_k1_state = (HAL_GPIO_ReadPin(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == KEY1_ACTIVE_LVL)
                         ? KEY_PRESSED
                         : KEY_RELEASED;
        s_k1_changed = 1;
    }
    if (GPIO_Pin == KEY2_GPIO_PIN)
    {
        s_k2_state = (HAL_GPIO_ReadPin(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == KEY2_ACTIVE_LVL)
                         ? KEY_PRESSED
                         : KEY_RELEASED;
        s_k2_changed = 1;
    }
}
