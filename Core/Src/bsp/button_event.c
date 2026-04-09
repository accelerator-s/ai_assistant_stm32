/**
 * @file  button_event.c
 * @brief 按钮事件检测实现
 */

#include "bsp/button_event.h"
#include "bsp/bsp_key.h"
#include <string.h>

/* 全局按钮实例 */
static button_t g_button_k1;
static button_t g_button_k2;

/* 初始化按钮 */
static void button_init(button_t* btn, GPIO_TypeDef* port, uint16_t pin, uint8_t active_level)
{
    memset(btn, 0, sizeof(button_t));

    btn->port = port;
    btn->pin = pin;
    btn->active_level = active_level;

    btn->state = BUTTON_STATE_RELEASED;
    btn->press_start_time = 0;
    btn->last_press_time = 0;
    btn->last_release_time = 0;
    btn->click_count = 0;

    btn->pending_event = BUTTON_EVENT_NONE;
    btn->event_ready = 0;

    btn->debounce_start_time = 0;
    btn->debounce_state = 0;
}

/* 按钮轮询处理 */
static void button_poll(button_t* btn)
{
    uint32_t current_time = HAL_GetTick();
    uint8_t current_level = 0;

    /* 使用bsp_key模块读取按钮状态 */
    if (btn->port == KEY1_GPIO_PORT && btn->pin == KEY1_GPIO_PIN) {
        current_level = (bsp_key_get_k1() == KEY_PRESSED) ? 1 : 0;
    } else if (btn->port == KEY2_GPIO_PORT && btn->pin == KEY2_GPIO_PIN) {
        current_level = (bsp_key_get_k2() == KEY_PRESSED) ? 1 : 0;
    }

    switch (btn->state) {
        case BUTTON_STATE_RELEASED:
            if (current_level == 1) {
                /* 检测到按下，进入消抖状态 */
                btn->state = BUTTON_STATE_DEBOUNCING;
                btn->debounce_start_time = current_time;
                btn->debounce_state = 1; /* 按下消抖 */
            }
            break;

        case BUTTON_STATE_PRESSED:
            if (current_level == 0) {
                /* 检测到释放，进入消抖状态 */
                btn->state = BUTTON_STATE_DEBOUNCING;
                btn->debounce_start_time = current_time;
                btn->debounce_state = 0; /* 释放消抖 */
            } else {
                /* 保持按下状态，检查是否达到长按时间 */
                uint32_t press_duration = current_time - btn->press_start_time;
                if (press_duration >= BUTTON_LONG_PRESS_TIME_MS &&
                    btn->pending_event == BUTTON_EVENT_NONE) {
                    /* 生成长按事件 */
                    btn->pending_event = BUTTON_EVENT_LONG_PRESS;
                    btn->event_ready = 1;
                    btn->state = BUTTON_STATE_WAIT_RELEASE;
                }
            }
            break;

        case BUTTON_STATE_DEBOUNCING:
            if (current_time - btn->debounce_start_time >= BUTTON_DEBOUNCE_TIME_MS) {
                /* 消抖完成 */
                if (btn->debounce_state == 1) {
                    /* 按下消抖完成 */
                    btn->state = BUTTON_STATE_PRESSED;
                    btn->press_start_time = current_time;

                    /* 检查是否为双击 */
                    if (current_time - btn->last_release_time <= BUTTON_DOUBLE_CLICK_TIME_MS) {
                        btn->click_count++;
                        if (btn->click_count >= 2) {
                            /* 生成本次按下事件（双击的第二下） */
                            btn->pending_event = BUTTON_EVENT_DOUBLE_CLICK;
                            btn->event_ready = 1;
                            btn->click_count = 0;
                        }
                    } else {
                        btn->click_count = 1;
                    }

                    /* 记录按下时间 */
                    btn->last_press_time = current_time;

                    /* 生成按下事件（用于按住不放检测） */
                    btn->pending_event = BUTTON_EVENT_HOLD;
                    btn->event_ready = 1;

                } else {
                    /* 释放消抖完成 */
                    btn->state = BUTTON_STATE_RELEASED;
                    btn->last_release_time = current_time;

                    /* 检查是否应该生成短按事件 */
                    uint32_t press_duration = current_time - btn->last_press_time;
                    if (press_duration >= BUTTON_SHORT_PRESS_TIME_MS &&
                        press_duration < BUTTON_LONG_PRESS_TIME_MS &&
                        btn->click_count == 1) {
                        /* 生成短按事件 */
                        btn->pending_event = BUTTON_EVENT_SHORT_PRESS;
                        btn->event_ready = 1;
                    }

                    /* 生成释放事件 */
                    btn->pending_event = BUTTON_EVENT_RELEASE;
                    btn->event_ready = 1;
                }
            }
            break;

        case BUTTON_STATE_WAIT_RELEASE:
            if (current_level == 0) {
                /* 检测到释放，进入消抖状态 */
                btn->state = BUTTON_STATE_DEBOUNCING;
                btn->debounce_start_time = current_time;
                btn->debounce_state = 0; /* 释放消抖 */
            }
            break;
    }

    /* 检查双击超时 */
    if (btn->state == BUTTON_STATE_RELEASED &&
        btn->click_count > 0 &&
        current_time - btn->last_press_time > BUTTON_DOUBLE_CLICK_TIME_MS) {
        /* 双击超时，重置点击计数 */
        btn->click_count = 0;
    }
}

/* 获取按钮事件 */
static button_event_t button_get_event(button_t* btn)
{
    if (btn->event_ready) {
        button_event_t event = btn->pending_event;
        btn->pending_event = BUTTON_EVENT_NONE;
        btn->event_ready = 0;
        return event;
    }
    return BUTTON_EVENT_NONE;
}

/* 检查按钮是否按下 */
static uint8_t button_is_pressed(button_t* btn)
{
    return (btn->state == BUTTON_STATE_PRESSED ||
            btn->state == BUTTON_STATE_WAIT_RELEASE);
}

/* 获取按钮按下持续时间 */
static uint32_t button_get_press_duration(button_t* btn)
{
    if (button_is_pressed(btn)) {
        uint32_t current_time = HAL_GetTick();
        if (current_time >= btn->press_start_time) {
            return current_time - btn->press_start_time;
        }
    }
    return 0;
}

/* 初始化按钮事件检测 */
void button_event_init(void)
{
    /* 初始化K1按钮 */
    button_init(&g_button_k1,
                KEY1_GPIO_PORT,
                KEY1_GPIO_PIN,
                (KEY1_ACTIVE_LVL == GPIO_PIN_SET) ? 1 : 0);

    /* 初始化K2按钮 */
    button_init(&g_button_k2,
                KEY2_GPIO_PORT,
                KEY2_GPIO_PIN,
                (KEY2_ACTIVE_LVL == GPIO_PIN_SET) ? 1 : 0);
}

/* 按钮事件轮询（在主循环中调用） */
void button_event_poll(void)
{
    button_poll(&g_button_k1);
    button_poll(&g_button_k2);
}

/* 获取K1按钮事件 */
button_event_t button_get_k1_event(void)
{
    return button_get_event(&g_button_k1);
}

/* 获取K2按钮事件 */
button_event_t button_get_k2_event(void)
{
    return button_get_event(&g_button_k2);
}

/* 检查K1是否按下 */
uint8_t button_is_k1_pressed(void)
{
    return button_is_pressed(&g_button_k1);
}

/* 检查K2是否按下 */
uint8_t button_is_k2_pressed(void)
{
    return button_is_pressed(&g_button_k2);
}

/* 获取K1按下持续时间 */
uint32_t button_get_k1_press_duration(void)
{
    return button_get_press_duration(&g_button_k1);
}

/* 获取K2按下持续时间 */
uint32_t button_get_k2_press_duration(void)
{
    return button_get_press_duration(&g_button_k2);
}