#include "button/button_context.h"

key_event_t detect_k2_event(void)
{
    uint8_t pressed = bsp_key_get_k2();
    uint32_t now = HAL_GetTick();

    if (pressed && !k2_was_pressed)
    {
        k2_press_tick = now;
        k2_was_pressed = 1;
    }
    else if (!pressed && k2_was_pressed)
    {
        uint32_t hold_time;
        k2_was_pressed = 0;
        hold_time = now - k2_press_tick;

        if (hold_time >= K2_LONG_PRESS_MS)
        {
            k2_press_count = 0;
            return KEY_EVENT_K2_LONG;
        }

        if ((now - k2_last_release_tick) <= K2_DOUBLE_CLICK_MS)
        {
            k2_press_count = 0;
            k2_last_release_tick = 0;
            return KEY_EVENT_K2_DOUBLE;
        }

        k2_press_count++;
        k2_last_release_tick = now;
    }

    if (k2_press_count > 0 && !pressed &&
        ((now - k2_last_release_tick) > K2_DOUBLE_CLICK_MS))
    {
        k2_press_count = 0;
        return KEY_EVENT_K2_SHORT;
    }

    return KEY_EVENT_NONE;
}

