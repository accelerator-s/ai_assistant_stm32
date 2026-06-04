/**
 * @file  display.c
 * @brief LCD 界面实现 — ChatGPT 风格聊天交互界面
 *
 * 界面布局 (240x320):
 *   顶部状态栏   [0,   24)   — WiFi状态 + 会话标题
 *   聊天消息区   [24, 280)   — 消息气泡滚动显示
 *   底部操作栏   [280, 320)  — 操作提示
 */
#include "lcd/display.h"
#include "lcd/lcd.h"
#include "lcd/font.h"
#include "bsp/bsp_spi_flash.h"
#include <stdio.h>
#include <string.h>

/* ===================== 内部状态 ===================== */

/* 当前界面状态 */
static display_state_t current_state = DISPLAY_STATE_CHAT;

/* 消息环形缓冲区 */
static display_msg_t msg_buf[MSG_MAX_COUNT];
static uint8_t msg_count = 0; /* 当前消息总数 */
static uint8_t msg_head = 0;  /* 环形缓冲区头部 */

/* 用户名缓存 */
static char username[32] = "User";

/* 会话标题缓存 */
static char session_title[32] = "新对话1";
static char wifi_status_text[32] = "wifi未连接";
static uint16_t wifi_status_color = COLOR_TEXT_DIM;

/* 历史列表数据 */
static const char **hist_titles = NULL;
static uint8_t hist_count = 0;
static uint8_t hist_selected = 0;
static uint8_t hist_scroll_top = 0; /* 列表滚动偏移 */

/* 消息显示滚动偏移（从最新消息往上的偏移量） */
static int16_t chat_scroll_offset = 0;

/* 调试日志环形缓冲区 */
static char dbg_log_buf[DEBUG_LOG_MAX_COUNT][DEBUG_LOG_LINE_LEN];
static uint8_t dbg_log_count = 0;
static uint8_t dbg_log_head = 0;
static uint8_t dbg_scroll_top = 0;

/* 调试日志布局参数 */
#define DBG_TITLE_H 24u
#define DBG_LINE_H 18u
#define DBG_BOTTOM_H 24u
#define DBG_AREA_Y (STATUS_BAR_H + DBG_TITLE_H)
#define DBG_AREA_H (LCD_HEIGHT - STATUS_BAR_H - DBG_TITLE_H - DBG_BOTTOM_H)
#define DBG_VISIBLE_LINES ((uint8_t)(DBG_AREA_H / DBG_LINE_H))
#define DBG_SCROLLBAR_W 6u

/* ===================== 文本工具函数 ===================== */

static uint8_t text_char_len(const char *p)
{
    if (!p || p[0] == '\0')
        return 0u;
    if ((uint8_t)p[0] <= 0x7Eu)
        return 1u;
    if (p[1] == '\0')
        return 1u;
    return 2u;
}

static uint16_t text_char_width(const char *p)
{
    if (!p || p[0] == '\0')
        return 0u;
    return ((uint8_t)p[0] <= 0x7Eu) ? FONT_W : CN_CHAR_WIDTH;
}

static uint16_t calc_text_width_range(const char *begin, const char *end)
{
    uint16_t width = 0u;
    const char *p = begin;

    while (p && p < end && *p)
    {
        uint8_t clen = text_char_len(p);
        width += text_char_width(p);
        p += clen;
    }

    return width;
}

static void dbg_log_push_raw_line(const char *line)
{
    uint8_t idx;

    if (!line || line[0] == '\0')
        return;

    if (dbg_log_count < DEBUG_LOG_MAX_COUNT)
    {
        idx = (uint8_t)((dbg_log_head + dbg_log_count) % DEBUG_LOG_MAX_COUNT);
        dbg_log_count++;
    }
    else
    {
        idx = dbg_log_head;
        dbg_log_head = (uint8_t)((dbg_log_head + 1u) % DEBUG_LOG_MAX_COUNT);
    }

    strncpy(dbg_log_buf[idx], line, DEBUG_LOG_LINE_LEN - 1u);
    dbg_log_buf[idx][DEBUG_LOG_LINE_LEN - 1u] = '\0';
}

/* ===================== 过渡动画辅助 ===================== */

/**
 * 从上往下逐行填充颜色（擦除动画）
 * 用于界面切换时的视觉过渡
 */
static void anim_wipe_down(uint16_t y_start, uint16_t y_end, uint16_t color)
{
    /* 加速擦除：每次刷8行 */
    for (uint16_t y = y_start; y < y_end; y += 8)
    {
        uint16_t h = 8;
        if (y + h > y_end)
            h = y_end - y;
        lcd_fill_rect(0, y, LCD_WIDTH, h, color);
    }
}

/* ===================== 状态栏绘制 ===================== */

static uint16_t calc_title_width(const char *text)
{
    uint16_t width = 0;

    if (!text)
        return 0;

    while (*text)
    {
        if ((uint8_t)*text <= 0x7E)
        {
            width += FONT_W;
            text++;
        }
        else
        {
            /* GBK 双字节: 如果第二字节为 '\0' 说明在边界被截断，停止解析 */
            if (text[1] == '\0')
                break;
            width += CN_CHAR_WIDTH;
            text += 2;
        }
    }

    return width;
}

static void draw_wifi_status(void)
{
    lcd_fill_circle(8, STATUS_BAR_H / 2, 3, wifi_status_color);
    lcd_draw_string_cn(16, 4, wifi_status_text, wifi_status_color, COLOR_BG_STATUS);
}

static void draw_center_title(void)
{
    uint16_t title_w = calc_title_width(session_title);
    uint16_t max_w = LCD_WIDTH - 48;
    uint16_t x;

    if (title_w > max_w)
    {
        title_w = max_w;
    }

    x = (LCD_WIDTH - title_w) / 2;
    if (x < 26)
    {
        x = 26;
    }

    lcd_draw_string_cn(x, 4, session_title, COLOR_TEXT_PRIMARY, COLOR_BG_STATUS);
}

/**
 * 绘制完整状态栏
 */
static void draw_status_bar(void)
{
    /* 背景填充 */
    lcd_fill_rect(0, 0, LCD_WIDTH, STATUS_BAR_H, COLOR_BG_STATUS);

    /* 左侧: 圆点 + 状态文字 */
    draw_wifi_status();

    /* 居中: 会话标题 */
    draw_center_title();

    /* 底部分割线 */
    lcd_draw_hline(0, STATUS_BAR_H - 1, LCD_WIDTH, COLOR_DIVIDER);
}

/* ===================== 底部操作栏绘制 ===================== */

/**
 * 绘制底部操作栏（默认待机提示）
 */
static void draw_bottom_bar(void)
{
    lcd_fill_rect(0, BOTTOM_BAR_Y, LCD_WIDTH, BOTTOM_BAR_H, COLOR_BG_STATUS);
    lcd_draw_hline(0, BOTTOM_BAR_Y, LCD_WIDTH, COLOR_DIVIDER);

    /* 默认提示: K1录音 K2发送 */
    lcd_draw_string_cn(8, BOTTOM_BAR_Y + 12,
                       "K1短按录音 K2新建",
                       COLOR_TEXT_SECONDARY, COLOR_BG_STATUS);
}

/* ===================== 消息气泡绘制 ===================== */

/**
 * 计算文本在给定宽度下的实际行高（像素）
 */
static uint16_t calc_text_height(const char *text, uint16_t max_w)
{
    uint16_t cx = 0;
    uint16_t lines = 1;
    const char *p = text;

    while (*p)
    {
        if (*p == '\n')
        {
            lines++;
            cx = 0;
            p++;
            continue;
        }

        if ((uint8_t)*p <= 0x7E)
        {
            if (cx + FONT_W > max_w)
            {
                lines++;
                cx = 0;
            }
            cx += FONT_W;
            p++;
        }
        else
        {
            /* GBK 双字节: 如果第二字节为 '\0' 说明在边界被截断，停止解析 */
            if (p[1] == '\0')
                break;
            if (cx + CN_CHAR_WIDTH > max_w)
            {
                lines++;
                cx = 0;
            }
            cx += CN_CHAR_WIDTH;
            p += 2;
        }
    }

    return lines * CN_CHAR_HEIGHT;
}

static uint16_t draw_center_wrapped_text(uint16_t y, const char *text, uint16_t max_w,
                                         uint16_t fg, uint16_t bg)
{
    const char *p = text;
    uint16_t lines = 0u;

    if (!text || text[0] == '\0')
        return CN_CHAR_HEIGHT;

    while (*p)
    {
        const char *line_start = p;
        const char *line_end = p;
        uint16_t line_w = 0u;

        while (*p)
        {
            uint8_t clen;
            uint16_t cw;

            if (*p == '\n')
                break;

            clen = text_char_len(p);
            cw = text_char_width(p);

            if ((line_w + cw) > max_w)
                break;

            line_w += cw;
            line_end = p + clen;
            p += clen;
        }

        if (line_end <= line_start)
        {
            uint8_t clen = text_char_len(p);
            line_end = p + ((clen > 0u) ? clen : 1u);
            line_w = calc_text_width_range(line_start, line_end);
            p = line_end;
        }

        {
            char line_buf[MSG_MAX_LEN];
            size_t copy_len = (size_t)(line_end - line_start);
            if (copy_len >= sizeof(line_buf))
                copy_len = sizeof(line_buf) - 1u;
            memcpy(line_buf, line_start, copy_len);
            line_buf[copy_len] = '\0';

            uint16_t x = (LCD_WIDTH > line_w) ? (uint16_t)((LCD_WIDTH - line_w) / 2u) : 0u;
            lcd_draw_string_cn(x, (uint16_t)(y + lines * CN_CHAR_HEIGHT), line_buf, fg, bg);
        }

        lines++;
        if (*p == '\n')
            p++;
    }

    if (lines == 0u)
        lines = 1u;

    return (uint16_t)(lines * CN_CHAR_HEIGHT);
}

/**
 * 绘制单条消息气泡，返回消息占用的总高度（含上下边距）
 * @param y       起始 Y 坐标
 * @param msg     消息结构体
 * @return 本条消息占用的像素高度
 */
static uint16_t draw_message_bubble(uint16_t y, const display_msg_t *msg)
{
    /* 气泡参数 */
    uint16_t bubble_margin = 4;  /* 外边距 */
    uint16_t bubble_padding = 4; /* 内边距 */
    uint16_t avatar_size = 20;   /* 头像圆大小(直径) */
    uint16_t avatar_gap = 4;     /* 头像与气泡间距 */
    uint16_t max_bubble_w = LCD_WIDTH - avatar_size - avatar_gap - bubble_margin * 2 - bubble_padding * 2;
    uint16_t text_h = calc_text_height(msg->text, max_bubble_w);
    uint16_t bubble_h = text_h + bubble_padding * 2;
    uint16_t bubble_w = max_bubble_w + bubble_padding * 2;
    uint16_t total_h = bubble_h + bubble_margin * 2;

    /* 超出聊天区域则不绘制 */
    if (y + total_h > BOTTOM_BAR_Y)
        return total_h;
    if (y < CHAT_AREA_Y)
        return total_h;

    if (msg->role == MSG_ROLE_USER)
    {
        /* === 用户消息: 右对齐 === */
        uint16_t bubble_x = LCD_WIDTH - bubble_margin - bubble_w;
        uint16_t avatar_cx = LCD_WIDTH - bubble_margin - avatar_size / 2;
        uint16_t avatar_cy = y + bubble_margin + avatar_size / 2;

        /* 头像: 绿色圆带首字母 */
        lcd_fill_circle(avatar_cx, avatar_cy, avatar_size / 2, COLOR_ACCENT_GREEN);
        lcd_draw_char(avatar_cx - FONT_W / 2, avatar_cy - FONT_H / 2,
                      username[0], COLOR_WHITE, COLOR_ACCENT_GREEN);

        /* 气泡 */
        bubble_x = LCD_WIDTH - bubble_margin - avatar_size - avatar_gap - bubble_w;
        lcd_fill_rounded_rect(bubble_x, y + bubble_margin,
                              bubble_w, bubble_h, 6, COLOR_BG_BUBBLE_USR);

        /* 文本 */
        lcd_draw_text_wrap(bubble_x + bubble_padding, y + bubble_margin + bubble_padding,
                           max_bubble_w, text_h,
                           msg->text, COLOR_TEXT_PRIMARY, COLOR_BG_BUBBLE_USR);
    }
    else if (msg->role == MSG_ROLE_ASSISTANT)
    {
        /* === AI 消息: 左对齐 === */
        uint16_t avatar_cx = bubble_margin + avatar_size / 2;
        uint16_t avatar_cy = y + bubble_margin + avatar_size / 2;

        /* 头像: 青色圆带 AI 字样 */
        lcd_fill_circle(avatar_cx, avatar_cy, avatar_size / 2, COLOR_ACCENT_TEAL);
        lcd_draw_string(avatar_cx - 6, avatar_cy - FONT_H / 2,
                        "AI", COLOR_WHITE, COLOR_ACCENT_TEAL);

        /* 气泡 */
        uint16_t bubble_x = bubble_margin + avatar_size + avatar_gap;
        lcd_fill_rounded_rect(bubble_x, y + bubble_margin,
                              bubble_w, bubble_h, 6, COLOR_BG_BUBBLE_AI);

        /* 文本 */
        lcd_draw_text_wrap(bubble_x + bubble_padding, y + bubble_margin + bubble_padding,
                           max_bubble_w, text_h,
                           msg->text, COLOR_TEXT_PRIMARY, COLOR_BG_BUBBLE_AI);
    }
    else
    {
        /* === 系统消息: 居中小字 === */
        uint16_t sys_max_w = LCD_WIDTH - 12u;
        uint16_t text_h = calc_text_height(msg->text, sys_max_w);
        total_h = text_h + bubble_margin * 2;
        draw_center_wrapped_text((uint16_t)(y + bubble_margin), msg->text, sys_max_w,
                                 COLOR_TEXT_DIM, COLOR_BG_DARK);
    }

    return total_h;
}

/* ===================== 聊天区域完整重绘 ===================== */

/**
 * 重绘整个聊天消息区域
 */
static void redraw_chat_area(void)
{
    /* 清空聊天区域 */
    lcd_fill_rect(0, CHAT_AREA_Y, LCD_WIDTH, CHAT_AREA_H, COLOR_BG_DARK);

    if (msg_count == 0)
    {
        /* 空聊天状态: 显示欢迎信息 */
        lcd_draw_string_cn(40, CHAT_AREA_Y + 80,
                           "按住 K1 开始说话",
                           COLOR_TEXT_DIM, COLOR_BG_DARK);
        lcd_draw_string_cn(32, CHAT_AREA_Y + 110,
                           "松手后按 K2 发送",
                           COLOR_TEXT_DIM, COLOR_BG_DARK);
        return;
    }

    uint16_t heights[MSG_MAX_COUNT];
    uint16_t total_height = 0u;
    uint8_t start_idx = 0u;
    uint8_t end_idx = msg_count;
    uint16_t draw_y = CHAT_AREA_Y;

    for (uint8_t i = 0; i < msg_count; i++)
    {
        uint8_t idx = (msg_head + i) % MSG_MAX_COUNT;
        uint16_t max_w = LCD_WIDTH - 20 - 24 - 8 - 8;
        uint16_t text_h = calc_text_height(msg_buf[idx].text, max_w);
        heights[i] = (uint16_t)(text_h + 8u + 8u);
        total_height += heights[i];
    }

    if (total_height <= CHAT_AREA_H)
    {
        chat_scroll_offset = 0;
        draw_y = CHAT_AREA_Y;
    }
    else
    {
        uint8_t bottom_start = msg_count;
        int16_t remain = (int16_t)CHAT_AREA_H;

        while (bottom_start > 0u && remain > 0)
        {
            bottom_start--;
            remain -= (int16_t)heights[bottom_start];
        }
        if (remain < 0)
            bottom_start++;

        if (chat_scroll_offset < 0)
            chat_scroll_offset = 0;
        if (chat_scroll_offset > (int16_t)bottom_start)
            chat_scroll_offset = (int16_t)bottom_start;

        start_idx = (uint8_t)(bottom_start - (uint8_t)chat_scroll_offset);

        {
            uint16_t used_h = 0u;
            end_idx = start_idx;
            while (end_idx < msg_count)
            {
                if ((uint16_t)(used_h + heights[end_idx]) > CHAT_AREA_H)
                    break;
                used_h = (uint16_t)(used_h + heights[end_idx]);
                end_idx++;
            }
        }

        draw_y = CHAT_AREA_Y;

        if (current_state == DISPLAY_STATE_CHAT_SCROLL)
        {
            uint16_t track_x = LCD_WIDTH - 7u;
            uint16_t track_h = CHAT_AREA_H;
            uint16_t thumb_h = (uint16_t)((uint32_t)(end_idx - start_idx) * track_h / msg_count);

            if (thumb_h < 16u)
                thumb_h = 16u;

            lcd_fill_rect(track_x, CHAT_AREA_Y, 6u, track_h, COLOR_BG_SIDEBAR);

            {
                uint16_t max_top = (msg_count > (end_idx - start_idx))
                                       ? (uint16_t)(msg_count - (end_idx - start_idx))
                                       : 0u;
                uint16_t thumb_y = CHAT_AREA_Y;
                if (max_top > 0u)
                {
                    thumb_y = (uint16_t)(CHAT_AREA_Y +
                                         (uint16_t)((uint32_t)start_idx * (track_h - thumb_h) / max_top));
                }
                lcd_fill_rounded_rect(track_x, thumb_y, 6u, thumb_h, 2u, COLOR_TEXT_DIM);
            }
        }
    }

    for (uint8_t i = start_idx; i < end_idx; i++)
    {
        uint8_t idx = (msg_head + i) % MSG_MAX_COUNT;
        uint16_t h = draw_message_bubble(draw_y, &msg_buf[idx]);
        draw_y = (uint16_t)(draw_y + h);
        if (draw_y >= BOTTOM_BAR_Y)
            break;
    }
}

/* ===================== 录音覆盖层 ===================== */

/**
 * 绘制录音状态覆盖层
 */
static void draw_recording_overlay(void)
{
    /* 半透明效果：用深色填充聊天区域 */
    lcd_fill_rect(0, CHAT_AREA_Y, LCD_WIDTH, CHAT_AREA_H, COLOR_BG_DARK);

    /* 中心麦克风图标（大红色圆） */
    uint16_t cx = LCD_WIDTH / 2;
    uint16_t cy = CHAT_AREA_Y + CHAT_AREA_H / 2 - 20;
    lcd_fill_circle(cx, cy, 30, COLOR_RECORDING);

    /* 麦克风符号（白色竖线 + 圆弧） */
    lcd_fill_rect(cx - 4, cy - 14, 8, 18, COLOR_WHITE);
    lcd_fill_rounded_rect(cx - 4, cy - 16, 8, 4, 2, COLOR_WHITE);
    lcd_draw_circle(cx, cy + 2, 14, COLOR_WHITE);
    lcd_draw_vline(cx, cy + 16, 8, COLOR_WHITE);
    lcd_draw_hline(cx - 6, cy + 24, 12, COLOR_WHITE);

    /* 提示文字 */
    lcd_draw_string_cn(72, cy + 50, "正在录音...", COLOR_TEXT_PRIMARY, COLOR_BG_DARK);

    /* 时长显示区域占位 */
    lcd_draw_string(96, cy + 70, "00:00", COLOR_TEXT_SECONDARY, COLOR_BG_DARK);

    /* 更新底部栏提示 */
    lcd_fill_rect(0, BOTTOM_BAR_Y, LCD_WIDTH, BOTTOM_BAR_H, COLOR_BG_STATUS);
    lcd_draw_hline(0, BOTTOM_BAR_Y, LCD_WIDTH, COLOR_DIVIDER);
    lcd_draw_string_cn(16, BOTTOM_BAR_Y + 12,
                       "松开暂停 K2:发送/取消",
                       COLOR_RECORDING, COLOR_BG_STATUS);
}

/* ===================== 历史记录列表绘制 ===================== */

/* 每个列表项高度 */
#define HIST_ITEM_H 32
/* 可见列表项数量 */
#define HIST_VISIBLE ((CHAT_AREA_H + BOTTOM_BAR_H) / HIST_ITEM_H)

/**
 * 绘制历史记录列表
 */
static void draw_history_list(void)
{
    /* 全屏覆盖（状态栏以下） */
    lcd_fill_rect(0, STATUS_BAR_H, LCD_WIDTH, LCD_HEIGHT - STATUS_BAR_H, COLOR_BG_SIDEBAR);

    /* 标题 */
    lcd_fill_rect(0, STATUS_BAR_H, LCD_WIDTH, 28, COLOR_BG_STATUS);
    lcd_draw_string_cn(8, STATUS_BAR_H + 6, "历史对话", COLOR_TEXT_PRIMARY, COLOR_BG_STATUS);
    lcd_draw_hline(0, STATUS_BAR_H + 27, LCD_WIDTH, COLOR_DIVIDER);

    if (hist_count == 0)
    {
        lcd_draw_string_cn(56, STATUS_BAR_H + 80, "暂无历史记录",
                           COLOR_TEXT_DIM, COLOR_BG_SIDEBAR);
        return;
    }

    /* 绘制列表项 */
    uint16_t list_y = STATUS_BAR_H + 28;
    for (uint8_t i = 0; i < HIST_VISIBLE && (hist_scroll_top + i) < hist_count; i++)
    {
        uint8_t idx = hist_scroll_top + i;
        uint16_t item_y = list_y + i * HIST_ITEM_H;
        uint16_t bg = (idx == hist_selected) ? COLOR_BG_BUBBLE_USR : COLOR_BG_SIDEBAR;

        /* 选中项高亮背景 */
        lcd_fill_rect(0, item_y, LCD_WIDTH, HIST_ITEM_H, bg);

        /* 选中项左侧绿色竖条 */
        if (idx == hist_selected)
        {
            lcd_fill_rect(0, item_y, 3, HIST_ITEM_H, COLOR_ACCENT_GREEN);
        }

        /* 序号 */
        char num[8];
        snprintf(num, sizeof(num), "%d.", idx + 1);
        lcd_draw_string(8, item_y + 8, num, COLOR_TEXT_SECONDARY, bg);

        /* 标题文字 */
        if (hist_titles && hist_titles[idx])
        {
            lcd_draw_string_cn(32, item_y + 8, hist_titles[idx], COLOR_TEXT_PRIMARY, bg);
        }

        /* 分割线 */
        lcd_draw_hline(8, item_y + HIST_ITEM_H - 1, LCD_WIDTH - 16, COLOR_DIVIDER);
    }

    /* 底部操作提示 */
    uint16_t tip_y = LCD_HEIGHT - 24;
    lcd_fill_rect(0, tip_y, LCD_WIDTH, 24, COLOR_BG_STATUS);
    lcd_draw_string_cn(8, tip_y + 4, "K1上 K2下 双击载入",
                       COLOR_TEXT_SECONDARY, COLOR_BG_STATUS);
}

/* ===================== 调试日志绘制 ===================== */

/**
 * 绘制调试日志界面（含滚动条）
 */
static void draw_debug_log(void)
{
    uint16_t y;

    /* 标题栏 */
    lcd_fill_rect(0, STATUS_BAR_H, LCD_WIDTH, DBG_TITLE_H, COLOR_BG_STATUS);
    {
        char title[28];
        snprintf(title, sizeof(title), "调试日志 (%u条)", (unsigned)dbg_log_count);
        lcd_draw_string_cn(8, STATUS_BAR_H + 4, title,
                           COLOR_TEXT_PRIMARY, COLOR_BG_STATUS);
    }
    lcd_draw_hline(0, STATUS_BAR_H + DBG_TITLE_H - 1, LCD_WIDTH, COLOR_DIVIDER);

    /* 调试行区域背景 */
    lcd_fill_rect(0, DBG_AREA_Y, LCD_WIDTH, DBG_AREA_H, COLOR_BG_DARK);

    if (dbg_log_count == 0)
    {
        lcd_draw_string_cn(56, DBG_AREA_Y + 60, "暂无调试信息",
                           COLOR_TEXT_DIM, COLOR_BG_DARK);
    }
    else
    {
        /* 绘制可见行 */
        for (uint8_t i = 0; i < DBG_VISIBLE_LINES && (dbg_scroll_top + i) < dbg_log_count; i++)
        {
            uint8_t line_idx = (uint8_t)((dbg_log_head + dbg_scroll_top + i) % DEBUG_LOG_MAX_COUNT);
            y = (uint16_t)(DBG_AREA_Y + (uint16_t)i * DBG_LINE_H + 1u);

            /* 行号 */
            char num[6];
            snprintf(num, sizeof(num), "%2u:", (unsigned)(dbg_scroll_top + i + 1u));
            lcd_draw_string(1, y, num, COLOR_TEXT_DIM, COLOR_BG_DARK);

            /* 行文本 */
            lcd_draw_string_cn(26, y, dbg_log_buf[line_idx],
                               COLOR_TEXT_SECONDARY, COLOR_BG_DARK);
        }
    }

    /* 滚动条 */
    if (dbg_log_count > DBG_VISIBLE_LINES)
    {
        uint16_t track_x = (uint16_t)(LCD_WIDTH - DBG_SCROLLBAR_W - 1u);
        uint16_t track_y = DBG_AREA_Y;
        uint16_t track_h = DBG_AREA_H;

        /* 滚动条轨道 */
        lcd_fill_rect(track_x, track_y, DBG_SCROLLBAR_W, track_h, COLOR_BG_SIDEBAR);

        /* 滚动条滑块 */
        uint16_t thumb_h = (uint16_t)((uint32_t)DBG_VISIBLE_LINES * track_h / dbg_log_count);
        if (thumb_h < 16u)
            thumb_h = 16u;

        uint8_t max_scroll = dbg_log_count - DBG_VISIBLE_LINES;
        uint16_t thumb_y = track_y;
        if (max_scroll > 0u)
        {
            thumb_y = (uint16_t)(track_y +
                                 (uint16_t)((uint32_t)dbg_scroll_top * (uint32_t)(track_h - thumb_h) / max_scroll));
        }

        lcd_fill_rounded_rect(track_x, thumb_y, DBG_SCROLLBAR_W, thumb_h, 2, COLOR_TEXT_DIM);
    }

    /* 底部提示 */
    uint16_t bottom_y = (uint16_t)(LCD_HEIGHT - DBG_BOTTOM_H);
    lcd_fill_rect(0, bottom_y, LCD_WIDTH, DBG_BOTTOM_H, COLOR_BG_STATUS);
    lcd_draw_hline(0, bottom_y, LCD_WIDTH, COLOR_DIVIDER);
    lcd_draw_string_cn(8, bottom_y + 4, "K1↑ K2短↓ K2长按退出",
                       COLOR_TEXT_SECONDARY, COLOR_BG_STATUS);
}

/* ===================== 公开接口实现 ===================== */

void display_init(void)
{
    lcd_init();
    lcd_clear(COLOR_BG_DARK);

    /* 初始化消息缓冲区 */
    memset(msg_buf, 0, sizeof(msg_buf));
    msg_count = 0;
    msg_head = 0;
    chat_scroll_offset = 0;

    /* 绘制初始界面 */
    draw_status_bar();
    redraw_chat_area();
    draw_bottom_bar();

    current_state = DISPLAY_STATE_CHAT;
}

void display_set_state(display_state_t state)
{
    if (state == current_state)
        return;

    display_state_t old_state = current_state;
    current_state = state;

    switch (state)
    {
    case DISPLAY_STATE_CHAT:
        /* 从其他状态切回聊天界面 */
        if (old_state == DISPLAY_STATE_HISTORY ||
            old_state == DISPLAY_STATE_DEBUG_LOG ||
            old_state == DISPLAY_STATE_CHAT_SCROLL)
        {
            /* 从其他列表界面返回: 擦除动画 */
            anim_wipe_down(STATUS_BAR_H, LCD_HEIGHT, COLOR_BG_DARK);
        }
        draw_status_bar();
        redraw_chat_area();
        draw_bottom_bar();
        break;

    case DISPLAY_STATE_RECORDING:
        draw_recording_overlay();
        break;

    case DISPLAY_STATE_HISTORY:
        /* 进入历史列表: 擦除动画 */
        anim_wipe_down(STATUS_BAR_H, LCD_HEIGHT, COLOR_BG_SIDEBAR);
        draw_status_bar();
        draw_history_list();
        break;

    case DISPLAY_STATE_CHAT_SCROLL:
        /* 聊天滚动浏览与聊天界面共用主体，仅底栏提示不同 */
        draw_status_bar();
        redraw_chat_area();
        display_update_bottom_hint("K1上翻 K2短按下翻 长按退出");
        break;

    case DISPLAY_STATE_DEBUG_LOG:
        /* 进入调试日志 */
        anim_wipe_down(STATUS_BAR_H, LCD_HEIGHT, COLOR_BG_DARK);
        draw_status_bar();
        draw_debug_log();
        break;
    }
}

display_state_t display_get_state(void)
{
    return current_state;
}

void display_update_wifi(const char *text, uint16_t color)
{
    if (text && text[0] != '\0')
    {
        strncpy(wifi_status_text, text, sizeof(wifi_status_text) - 1);
        wifi_status_text[sizeof(wifi_status_text) - 1] = '\0';
    }
    wifi_status_color = color;

    /* 整栏重绘，避免局部刷新残影 */
    draw_status_bar();
}

void display_update_title(const char *title)
{
    if (!title)
        return;
    strncpy(session_title, title, sizeof(session_title) - 1);
    session_title[sizeof(session_title) - 1] = '\0';

    /* 整栏重绘，避免局部刷新残影 */
    draw_status_bar();
}

void display_clear_messages(void)
{
    memset(msg_buf, 0, sizeof(msg_buf));
    msg_count = 0;
    msg_head = 0;
    chat_scroll_offset = 0;

    if (current_state == DISPLAY_STATE_CHAT ||
        current_state == DISPLAY_STATE_CHAT_SCROLL)
    {
        redraw_chat_area();
    }
}

void display_add_message(msg_role_t role, const char *text)
{
    if (!text || text[0] == '\0')
        return;

    /* 环形缓冲区写入 */
    uint8_t idx;
    if (msg_count < MSG_MAX_COUNT)
    {
        idx = (msg_head + msg_count) % MSG_MAX_COUNT;
        msg_count++;
    }
    else
    {
        /* 缓冲区已满，覆盖最旧消息 */
        idx = msg_head;
        msg_head = (msg_head + 1) % MSG_MAX_COUNT;
    }

    msg_buf[idx].role = role;
    strncpy(msg_buf[idx].text, text, MSG_MAX_LEN - 1);
    msg_buf[idx].text[MSG_MAX_LEN - 1] = '\0';
    msg_buf[idx].text_len = (uint8_t)strlen(msg_buf[idx].text);

    /* 刷新聊天区域 */
    if (current_state == DISPLAY_STATE_CHAT ||
        current_state == DISPLAY_STATE_CHAT_SCROLL)
    {
        redraw_chat_area();
    }
}

void display_update_last_message(msg_role_t role, const char *text)
{
    uint8_t idx;

    if (!text || text[0] == '\0')
        return;

    if (msg_count == 0)
    {
        display_add_message(role, text);
        return;
    }

    idx = (uint8_t)((msg_head + msg_count - 1u) % MSG_MAX_COUNT);
    if (msg_buf[idx].role != role)
    {
        display_add_message(role, text);
        return;
    }

    strncpy(msg_buf[idx].text, text, MSG_MAX_LEN - 1);
    msg_buf[idx].text[MSG_MAX_LEN - 1] = '\0';
    msg_buf[idx].text_len = (uint8_t)strlen(msg_buf[idx].text);

    if (current_state == DISPLAY_STATE_CHAT ||
        current_state == DISPLAY_STATE_CHAT_SCROLL)
    {
        redraw_chat_area();
    }
}

void display_show_system_hint(const char *text)
{
    display_add_message(MSG_ROLE_SYSTEM, text);
}

void display_start_recording(void)
{
    display_set_state(DISPLAY_STATE_RECORDING);
}

void display_update_recording(uint32_t ms)
{
    if (current_state != DISPLAY_STATE_RECORDING)
        return;

    /* 计算分:秒 */
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    sec %= 60;

    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)min, (unsigned long)sec);

    uint16_t cy = CHAT_AREA_Y + CHAT_AREA_H / 2 - 20;
    /* 清除旧时间显示 */
    lcd_fill_rect(88, cy + 70, 64, 16, COLOR_BG_DARK);
    lcd_draw_string(96, cy + 70, buf, COLOR_TEXT_SECONDARY, COLOR_BG_DARK);

    /* 脉冲动画: 外圈交替显示 */
    static uint8_t pulse_phase = 0;
    uint16_t cx = LCD_WIDTH / 2;
    uint16_t outer_r = 36 + (pulse_phase & 0x01) * 4;
    uint16_t ring_color = (pulse_phase & 0x01) ? COLOR_RECORDING : COLOR_BG_DARK;
    lcd_draw_circle(cx, cy, outer_r, ring_color);
    pulse_phase++;
}

void display_stop_recording(void)
{
    display_set_state(DISPLAY_STATE_CHAT);
}

void display_show_history(const char **titles, uint8_t count)
{
    hist_titles = titles;
    hist_count = count;
    hist_selected = 0;
    hist_scroll_top = 0;

    display_set_state(DISPLAY_STATE_HISTORY);
}

void display_history_up(void)
{
    if (current_state != DISPLAY_STATE_HISTORY || hist_count == 0)
        return;

    if (hist_selected > 0)
    {
        hist_selected--;
        /* 滚动跟随 */
        if (hist_selected < hist_scroll_top)
            hist_scroll_top = hist_selected;
        draw_history_list();
    }
}

void display_history_down(void)
{
    if (current_state != DISPLAY_STATE_HISTORY || hist_count == 0)
        return;

    if (hist_selected < hist_count - 1)
    {
        hist_selected++;
        /* 滚动跟随 */
        if (hist_selected >= hist_scroll_top + HIST_VISIBLE)
            hist_scroll_top = hist_selected - HIST_VISIBLE + 1;
        draw_history_list();
    }
}

uint8_t display_history_get_selected(void)
{
    return hist_selected;
}

void display_set_username(const char *name)
{
    if (!name)
        return;
    strncpy(username, name, sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';
}

void display_update_bottom_hint(const char *text)
{
    lcd_fill_rect(0, BOTTOM_BAR_Y, LCD_WIDTH, BOTTOM_BAR_H, COLOR_BG_STATUS);
    lcd_draw_hline(0, BOTTOM_BAR_Y, LCD_WIDTH, COLOR_DIVIDER);
    if (text && text[0] != '\0')
    {
        lcd_draw_string_cn(8, BOTTOM_BAR_Y + 12, text,
                           COLOR_TEXT_SECONDARY, COLOR_BG_STATUS);
    }
}

/* ===================== 聊天滚动公开接口 ===================== */

void display_enter_chat_scroll(void)
{
    chat_scroll_offset = 0;
    display_set_state(DISPLAY_STATE_CHAT_SCROLL);
}

void display_exit_chat_scroll(void)
{
    chat_scroll_offset = 0;
    display_set_state(DISPLAY_STATE_CHAT);
}

void display_chat_scroll_up(void)
{
    if (current_state != DISPLAY_STATE_CHAT_SCROLL)
        return;

    if (chat_scroll_offset < (int16_t)MSG_MAX_COUNT)
    {
        chat_scroll_offset++;
        redraw_chat_area();
    }
}

void display_chat_scroll_down(void)
{
    if (current_state != DISPLAY_STATE_CHAT_SCROLL)
        return;

    if (chat_scroll_offset > 0)
    {
        chat_scroll_offset--;
        redraw_chat_area();
    }
}

/* ===================== 调试日志公开接口 ===================== */

void display_push_debug_line(const char *line)
{
    if (!line || line[0] == '\0')
        return;

    {
        const uint16_t max_px = LCD_WIDTH - 26u - DBG_SCROLLBAR_W - 4u;
        const char *p = line;

        while (*p)
        {
            char seg[DEBUG_LOG_LINE_LEN];
            uint16_t px = 0u;
            uint8_t used = 0u;

            while (*p && *p != '\n')
            {
                uint8_t clen = text_char_len(p);
                uint16_t cw = text_char_width(p);

                if (used + clen >= (DEBUG_LOG_LINE_LEN - 1u))
                    break;
                if ((px + cw) > max_px)
                    break;

                memcpy(&seg[used], p, clen);
                used = (uint8_t)(used + clen);
                px = (uint16_t)(px + cw);
                p += clen;
            }

            if (used == 0u && *p && *p != '\n')
            {
                uint8_t clen = text_char_len(p);
                if (clen >= DEBUG_LOG_LINE_LEN)
                    clen = DEBUG_LOG_LINE_LEN - 1u;
                memcpy(seg, p, clen);
                used = clen;
                p += clen;
            }

            seg[used] = '\0';
            dbg_log_push_raw_line(seg);

            if (*p == '\n')
                p++;
        }
    }

    /* 在调试日志界面时自动滚动到最新并刷新 */
    if (current_state == DISPLAY_STATE_DEBUG_LOG)
    {
        if (dbg_log_count > DBG_VISIBLE_LINES)
            dbg_scroll_top = dbg_log_count - DBG_VISIBLE_LINES;
        else
            dbg_scroll_top = 0;
        draw_debug_log();
    }
}

void display_enter_debug_log(void)
{
    /* 滚动到最新 */
    if (dbg_log_count > DBG_VISIBLE_LINES)
        dbg_scroll_top = dbg_log_count - DBG_VISIBLE_LINES;
    else
        dbg_scroll_top = 0;

    display_set_state(DISPLAY_STATE_DEBUG_LOG);
}

void display_exit_debug_log(void)
{
    display_set_state(DISPLAY_STATE_CHAT);
}

void display_debug_scroll_up(void)
{
    if (current_state != DISPLAY_STATE_DEBUG_LOG || dbg_log_count == 0)
        return;

    if (dbg_scroll_top > 0)
    {
        dbg_scroll_top--;
        draw_debug_log();
    }
}

void display_debug_scroll_down(void)
{
    if (current_state != DISPLAY_STATE_DEBUG_LOG || dbg_log_count == 0)
        return;

    uint8_t max_scroll = (dbg_log_count > DBG_VISIBLE_LINES)
                             ? (uint8_t)(dbg_log_count - DBG_VISIBLE_LINES)
                             : 0u;

    if (dbg_scroll_top < max_scroll)
    {
        dbg_scroll_top++;
        draw_debug_log();
    }
}
