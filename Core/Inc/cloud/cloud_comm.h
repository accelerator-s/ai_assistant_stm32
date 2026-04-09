/**
 * @file  cloud_comm.h
 * @brief 云端通信模块
 */

#ifndef __CLOUD_COMM_H
#define __CLOUD_COMM_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* 云端命令类型 */
typedef enum {
    CLOUD_CMD_NONE = 0,             /* 无命令 */
    CLOUD_CMD_START_SESSION,        /* 开始新会话 */
    CLOUD_CMD_END_SESSION,          /* 结束会话 */
    CLOUD_CMD_SEND_AUDIO,           /* 发送音频数据 */
    CLOUD_CMD_AUDIO_COMPLETE,       /* 音频发送完成 */
    CLOUD_CMD_CANCEL_AUDIO,         /* 取消音频发送 */
    CLOUD_CMD_REQUEST_HISTORY,      /* 请求历史记录 */
    CLOUD_CMD_LOAD_HISTORY,         /* 加载历史记录 */
    CLOUD_CMD_HEARTBEAT,            /* 心跳 */
    CLOUD_CMD_ERROR                 /* 错误 */
} cloud_cmd_t;

/* 云端响应状态 */
typedef enum {
    CLOUD_RESP_NONE = 0,            /* 无响应 */
    CLOUD_RESP_OK,                  /* 成功 */
    CLOUD_RESP_ERROR,               /* 错误 */
    CLOUD_RESP_SESSION_CREATED,     /* 会话已创建 */
    CLOUD_RESP_AUDIO_RECEIVED,      /* 音频已接收 */
    CLOUD_RESP_PROCESSING,          /* 处理中 */
    CLOUD_RESP_RESULT_READY,        /* 结果就绪 */
    CLOUD_RESP_HISTORY_LIST,        /* 历史记录列表 */
    CLOUD_RESP_HISTORY_CONTENT      /* 历史记录内容 */
} cloud_resp_t;

/* 音频帧头 */
typedef struct {
    uint32_t session_id;            /* 会话ID */
    uint32_t frame_seq;             /* 帧序号 */
    uint32_t timestamp;             /* 时间戳 */
    uint32_t data_size;             /* 数据大小 */
    uint8_t  is_last_frame;         /* 是否最后一帧 */
    uint8_t  reserved[3];           /* 保留 */
} audio_frame_header_t;

/* 控制帧 */
typedef struct {
    uint32_t session_id;            /* 会话ID */
    uint32_t command;               /* 命令 */
    uint32_t param1;                /* 参数1 */
    uint32_t param2;                /* 参数2 */
    char     message[64];           /* 消息 */
} control_frame_t;

/* 云端通信上下文 */
typedef struct {
    /* 连接状态 */
    uint8_t connected;              /* 是否连接 */
    uint8_t authenticated;          /* 是否认证 */

    /* 会话信息 */
    uint32_t current_session_id;    /* 当前会话ID */
    uint32_t last_heartbeat_time;   /* 上次心跳时间 */

    /* 音频传输 */
    uint32_t audio_frame_seq;       /* 音频帧序号 */
    uint32_t total_audio_sent;      /* 总发送音频字节数 */
    uint32_t audio_buffer_size;     /* 音频缓冲区大小 */

    /* 接收处理 */
    uint8_t  receiving_audio;       /* 正在接收音频 */
    uint32_t audio_receive_size;    /* 音频接收大小 */
    uint32_t total_audio_received;  /* 总接收音频字节数 */

    /* 错误信息 */
    uint32_t error_count;           /* 错误计数 */
    char     last_error[128];       /* 最后错误信息 */
} cloud_context_t;

/* 函数声明 */
void cloud_comm_init(void);
void cloud_comm_poll(void);

bool cloud_comm_is_connected(void);
bool cloud_comm_is_authenticated(void);

bool cloud_comm_start_session(uint32_t* session_id);
bool cloud_comm_end_session(uint32_t session_id);

bool cloud_comm_send_audio_frame(uint32_t session_id, const uint8_t* data, uint32_t size, uint8_t is_last);
bool cloud_comm_send_audio_complete(uint32_t session_id);
bool cloud_comm_cancel_audio(uint32_t session_id);

bool cloud_comm_request_history_list(void);
bool cloud_comm_load_history(uint32_t history_id);

bool cloud_comm_send_heartbeat(void);

cloud_resp_t cloud_comm_check_response(void);
bool cloud_comm_get_response_data(uint8_t* buffer, uint32_t* size);

uint32_t cloud_comm_get_current_session_id(void);
uint32_t cloud_comm_get_error_count(void);
const char* cloud_comm_get_last_error(void);

/* 内部函数 */
bool cloud_send_control_frame(control_frame_t* frame);
bool cloud_send_audio_data(const uint8_t* data, uint32_t size);
bool cloud_process_incoming_data(void);

#endif /* __CLOUD_COMM_H */