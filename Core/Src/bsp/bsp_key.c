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

    /* K1 — PA0 输入模式 */
    gpio.Pin = KEY1_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(KEY1_GPIO_PORT, &gpio);

    /* K2 — PC13 输入模式 */
    gpio.Pin = KEY2_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(KEY2_GPIO_PORT, &gpio);

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

/* 按钮状态更新函数（在主循环中调用） */
void bsp_key_update(void)
{
    static uint32_t last_update_time = 0;
    uint32_t current_time = HAL_GetTick();

    /* 每10ms更新一次按钮状态 */
    if (current_time - last_update_time >= 10) {
        uint8_t new_k1_state = (HAL_GPIO_ReadPin(KEY1_GPIO_PORT, KEY1_GPIO_PIN) == KEY1_ACTIVE_LVL)
                                 ? KEY_PRESSED
                                 : KEY_RELEASED;
        uint8_t new_k2_state = (HAL_GPIO_ReadPin(KEY2_GPIO_PORT, KEY2_GPIO_PIN) == KEY2_ACTIVE_LVL)
                                 ? KEY_PRESSED
                                 : KEY_RELEASED;

        if (new_k1_state != s_k1_state) {
            s_k1_state = new_k1_state;
            s_k1_changed = 1;
        }

        if (new_k2_state != s_k2_state) {
            s_k2_state = new_k2_state;
            s_k2_changed = 1;
        }

        last_update_time = current_time;
    }
}
