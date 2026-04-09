/**
 * @file  button_event.h
 * @brief 按钮事件检测（短按、长按、双击）
 */

#ifndef __BUTTON_EVENT_H
#define __BUTTON_EVENT_H

#include "main.h"
#include "system/system_state.h"

/* 按钮配置 */
#define BUTTON_DEBOUNCE_TIME_MS    20    /* 消抖时间 */
#define BUTTON_SHORT_PRESS_TIME_MS 50    /* 短按最小时间 */
#define BUTTON_LONG_PRESS_TIME_MS  1000  /* 长按最小时间（1秒） */
#define BUTTON_DOUBLE_CLICK_TIME_MS 300  /* 双击最大间隔（300ms） */

/* 按钮状态 */
typedef enum {
    BUTTON_STATE_RELEASED = 0,      /* 释放状态 */
    BUTTON_STATE_PRESSED,           /* 按下状态 */
    BUTTON_STATE_DEBOUNCING,        /* 消抖中 */
    BUTTON_STATE_WAIT_RELEASE       /* 等待释放 */
} button_state_t;

/* 按钮实例 */
typedef struct {
    /* 硬件相关 */
    GPIO_TypeDef* port;             /* GPIO端口 */
    uint16_t pin;                   /* GPIO引脚 */
    uint8_t active_level;           /* 有效电平 */

    /* 状态相关 */
    button_state_t state;           /* 当前状态 */
    uint32_t press_start_time;      /* 按下开始时间 */
    uint32_t last_press_time;       /* 上次按下时间 */
    uint32_t last_release_time;     /* 上次释放时间 */
    uint8_t click_count;            /* 点击计数 */

    /* 事件相关 */
    button_event_t pending_event;   /* 待处理事件 */
    uint8_t event_ready;            /* 事件就绪标志 */

    /* 消抖相关 */
    uint32_t debounce_start_time;   /* 消抖开始时间 */
    uint8_t debounce_state;         /* 消抖状态 */
} button_t;

/* 函数声明 */
void button_event_init(void);
void button_event_poll(void);

button_event_t button_get_k1_event(void);
button_event_t button_get_k2_event(void);

uint8_t button_is_k1_pressed(void);
uint8_t button_is_k2_pressed(void);

uint32_t button_get_k1_press_duration(void);
uint32_t button_get_k2_press_duration(void);

/* 内部函数 - 仅在button_event.c中使用 */

#endif /* __BUTTON_EVENT_H */