/**
 * @file  cloud_comm.c
 * @brief 云端通信模块实现（简化版）
 */

#include "cloud/cloud_comm.h"
#include "wifi/esp8266.h"
#include "debug/debug_uart.h"
#include "lcd/display.h"
#include <string.h>
#include <stdio.h>

/* 全局上下文 */
static cloud_context_t g_cloud_context = {0};

/* 初始化云端通信 */
void cloud_comm_init(void)
{
    memset(&g_cloud_context, 0, sizeof(cloud_context_t));

    g_cloud_context.connected = 0;
    g_cloud_context.authenticated = 0;
    g_cloud_context.current_session_id = 0;
    g_cloud_context.last_heartbeat_time = 0;

    g_cloud_context.audio_frame_seq = 0;
    g_cloud_context.total_audio_sent = 0;
    g_cloud_context.audio_buffer_size = 4096;

    g_cloud_context.receiving_audio = 0;
    g_cloud_context.audio_receive_size = 0;
    g_cloud_context.total_audio_received = 0;

    g_cloud_context.error_count = 0;
    g_cloud_context.last_error[0] = '\0';
}

/* 云端通信轮询 */
void cloud_comm_poll(void)
{
    /* 检查TCP连接状态 */
    esp8266_status_t wifi_st = esp8266_get_status();
    if (wifi_st == ESP8266_STATUS_TCP_CONNECTED) {
        if (!g_cloud_context.connected) {
            g_cloud_context.connected = 1;
            display_update_debug("云端连接已建立");
        }
    } else {
        if (g_cloud_context.connected) {
            g_cloud_context.connected = 0;
            g_cloud_context.authenticated = 0;
            display_update_debug("云端连接已断开");
        }
    }

    /* 处理接收数据 */
    cloud_process_incoming_data();
}

/* 检查是否连接 */
bool cloud_comm_is_connected(void)
{
    return g_cloud_context.connected;
}

/* 检查是否认证 */
bool cloud_comm_is_authenticated(void)
{
    return g_cloud_context.authenticated;
}

/* 开始新会话 */
bool cloud_comm_start_session(uint32_t* session_id)
{
    if (!g_cloud_context.connected) {
        strcpy(g_cloud_context.last_error, "未连接到云端");
        g_cloud_context.error_count++;
        return false;
    }

    /* 发送开始会话命令 */
    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.command = CLOUD_CMD_START_SESSION;
    frame.param1 = 0;  /* 协议版本 */
    frame.param2 = 0;  /* 保留 */

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送开始会话命令失败");
        g_cloud_context.error_count++;
        return false;
    }

    /* 简化：假设会话ID为递增数字 */
    static uint32_t next_session_id = 1;
    g_cloud_context.current_session_id = next_session_id;
    *session_id = next_session_id;
    next_session_id++;

    display_update_debug("新会话已创建");
    return true;
}

/* 结束会话 */
bool cloud_comm_end_session(uint32_t session_id)
{
    if (!g_cloud_context.connected) {
        return false;
    }

    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.session_id = session_id;
    frame.command = CLOUD_CMD_END_SESSION;

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送结束会话命令失败");
        g_cloud_context.error_count++;
        return false;
    }

    if (g_cloud_context.current_session_id == session_id) {
        g_cloud_context.current_session_id = 0;
    }

    display_update_debug("会话已结束");
    return true;
}

/* 发送音频帧 */
bool cloud_comm_send_audio_frame(uint32_t session_id, const uint8_t* data, uint32_t size, uint8_t is_last)
{
    if (!g_cloud_context.connected) {
        strcpy(g_cloud_context.last_error, "未连接到云端");
        g_cloud_context.error_count++;
        return false;
    }

    if (size == 0 || data == NULL) {
        return false;
    }

    /* 构建音频帧头 */
    audio_frame_header_t header;
    memset(&header, 0, sizeof(audio_frame_header_t));

    header.session_id = session_id;
    header.frame_seq = g_cloud_context.audio_frame_seq++;
    header.timestamp = HAL_GetTick();
    header.data_size = size;
    header.is_last_frame = is_last;

    /* 发送帧头 */
    if (!cloud_send_audio_data((uint8_t*)&header, sizeof(header))) {
        strcpy(g_cloud_context.last_error, "发送音频帧头失败");
        g_cloud_context.error_count++;
        return false;
    }

    /* 发送音频数据 */
    if (!cloud_send_audio_data(data, size)) {
        strcpy(g_cloud_context.last_error, "发送音频数据失败");
        g_cloud_context.error_count++;
        return false;
    }

    g_cloud_context.total_audio_sent += size;

    /* 调试信息 */
    char debug_msg[64];
    snprintf(debug_msg, sizeof(debug_msg), "发送音频:%lu字节", size);
    display_update_debug(debug_msg);

    return true;
}

/* 发送音频完成标志 */
bool cloud_comm_send_audio_complete(uint32_t session_id)
{
    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.session_id = session_id;
    frame.command = CLOUD_CMD_AUDIO_COMPLETE;

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送音频完成标志失败");
        g_cloud_context.error_count++;
        return false;
    }

    display_update_debug("音频发送完成");
    return true;
}

/* 取消音频发送 */
bool cloud_comm_cancel_audio(uint32_t session_id)
{
    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.session_id = session_id;
    frame.command = CLOUD_CMD_CANCEL_AUDIO;

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送取消音频命令失败");
        g_cloud_context.error_count++;
        return false;
    }

    display_update_debug("音频发送已取消");
    return true;
}

/* 请求历史记录列表 */
bool cloud_comm_request_history_list(void)
{
    if (!g_cloud_context.connected) {
        return false;
    }

    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.command = CLOUD_CMD_REQUEST_HISTORY;

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送历史记录请求失败");
        g_cloud_context.error_count++;
        return false;
    }

    display_update_debug("请求历史记录列表");
    return true;
}

/* 加载历史记录 */
bool cloud_comm_load_history(uint32_t history_id)
{
    if (!g_cloud_context.connected) {
        return false;
    }

    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.command = CLOUD_CMD_LOAD_HISTORY;
    frame.param1 = history_id;

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送加载历史记录命令失败");
        g_cloud_context.error_count++;
        return false;
    }

    char debug_msg[64];
    snprintf(debug_msg, sizeof(debug_msg), "加载历史记录:%lu", history_id);
    display_update_debug(debug_msg);

    return true;
}

/* 发送心跳 */
bool cloud_comm_send_heartbeat(void)
{
    if (!g_cloud_context.connected) {
        return false;
    }

    uint32_t current_time = HAL_GetTick();
    if (current_time - g_cloud_context.last_heartbeat_time < 15000) {
        return true;  /* 15秒内已发送过心跳 */
    }

    control_frame_t frame;
    memset(&frame, 0, sizeof(control_frame_t));

    frame.command = CLOUD_CMD_HEARTBEAT;

    if (!cloud_send_control_frame(&frame)) {
        strcpy(g_cloud_context.last_error, "发送心跳失败");
        g_cloud_context.error_count++;
        return false;
    }

    g_cloud_context.last_heartbeat_time = current_time;
    return true;
}

/* 检查响应 */
cloud_resp_t cloud_comm_check_response(void)
{
    /* 简化：这里应该检查接收缓冲区中的响应 */
    /* 实际实现需要解析从云端接收的数据 */
    return CLOUD_RESP_NONE;
}

/* 获取响应数据 */
bool cloud_comm_get_response_data(uint8_t* buffer, uint32_t* size)
{
    /* 简化：这里应该返回接收到的数据 */
    /* 实际实现需要从接收缓冲区复制数据 */
    *size = 0;
    return false;
}

/* 获取当前会话ID */
uint32_t cloud_comm_get_current_session_id(void)
{
    return g_cloud_context.current_session_id;
}

/* 获取错误计数 */
uint32_t cloud_comm_get_error_count(void)
{
    return g_cloud_context.error_count;
}

/* 获取最后错误信息 */
const char* cloud_comm_get_last_error(void)
{
    return g_cloud_context.last_error;
}

/* 内部函数：发送控制帧 */
bool cloud_send_control_frame(control_frame_t* frame)
{
    if (!g_cloud_context.connected || frame == NULL) {
        return false;
    }

    /* 构建控制帧数据 */
    uint8_t buffer[256];
    uint32_t pos = 0;

    /* 帧头：0xAA 0x55 0x01 (控制帧标识) */
    buffer[pos++] = 0xAA;
    buffer[pos++] = 0x55;
    buffer[pos++] = 0x01;

    /* 会话ID (4字节) */
    buffer[pos++] = (frame->session_id >> 24) & 0xFF;
    buffer[pos++] = (frame->session_id >> 16) & 0xFF;
    buffer[pos++] = (frame->session_id >> 8) & 0xFF;
    buffer[pos++] = frame->session_id & 0xFF;

    /* 命令 (4字节) */
    buffer[pos++] = (frame->command >> 24) & 0xFF;
    buffer[pos++] = (frame->command >> 16) & 0xFF;
    buffer[pos++] = (frame->command >> 8) & 0xFF;
    buffer[pos++] = frame->command & 0xFF;

    /* 参数1 (4字节) */
    buffer[pos++] = (frame->param1 >> 24) & 0xFF;
    buffer[pos++] = (frame->param1 >> 16) & 0xFF;
    buffer[pos++] = (frame->param1 >> 8) & 0xFF;
    buffer[pos++] = frame->param1 & 0xFF;

    /* 参数2 (4字节) */
    buffer[pos++] = (frame->param2 >> 24) & 0xFF;
    buffer[pos++] = (frame->param2 >> 16) & 0xFF;
    buffer[pos++] = (frame->param2 >> 8) & 0xFF;
    buffer[pos++] = frame->param2 & 0xFF;

    /* 消息长度 (1字节) */
    uint8_t msg_len = strlen(frame->message);
    if (msg_len > 63) msg_len = 63;
    buffer[pos++] = msg_len;

    /* 消息内容 */
    if (msg_len > 0) {
        memcpy(&buffer[pos], frame->message, msg_len);
        pos += msg_len;
    }

    /* 帧尾：0x55 0xAA */
    buffer[pos++] = 0x55;
    buffer[pos++] = 0xAA;

    /* 通过ESP8266发送数据 */
    if (esp8266_tcp_send(buffer, pos) != 0) {
        strcpy(g_cloud_context.last_error, "TCP发送失败");
        g_cloud_context.error_count++;
        return false;
    }

    return true;
}

/* 内部函数：发送音频数据 */
bool cloud_send_audio_data(const uint8_t* data, uint32_t size)
{
    if (!g_cloud_context.connected || data == NULL || size == 0) {
        return false;
    }

    /* 通过ESP8266发送数据 */
    if (esp8266_tcp_send(data, size) != 0) {
        strcpy(g_cloud_context.last_error, "音频数据发送失败");
        g_cloud_context.error_count++;
        return false;
    }

    return true;
}

/* 内部函数：处理接收数据 */
bool cloud_process_incoming_data(void)
{
    if (!g_cloud_context.connected) {
        return false;
    }

    /* 检查是否有数据可读 */
    uint16_t available = esp8266_tcp_data_available();
    if (available == 0) {
        return false;
    }

    /* 读取数据 */
    uint8_t buffer[256];
    uint16_t read_len = esp8266_tcp_read(buffer, sizeof(buffer));
    if (read_len == 0) {
        return false;
    }

    /* 简化：这里应该解析接收到的数据 */
    /* 实际实现需要根据通信协议解析控制帧和音频数据 */

    /* 调试信息 */
    char debug_msg[64];
    snprintf(debug_msg, sizeof(debug_msg), "收到数据:%u字节", read_len);
    display_update_debug(debug_msg);

    return true;
}